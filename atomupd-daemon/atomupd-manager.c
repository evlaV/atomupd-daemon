/*
 * Copyright © 2023 Collabora Ltd.
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <errno.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>

#include <gio/gio.h>
#include <glib-unix.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "utils.h"

#define _send_atomupd_message(_bus, _method, _body, _reply_out, _error)                  \
   _send_message(_bus, AU_ATOMUPD1_INTERFACE, _method, _body, _reply_out, _error)

#define _send_properties_message(_bus, _method, _body, _reply_out, _error)               \
   _send_message(_bus, "org.freedesktop.DBus.Properties", _method, _body, _reply_out,    \
                 _error)

static GMainLoop *main_loop = NULL;

static gboolean opt_session = FALSE;
static gboolean opt_verbose = FALSE;
static gboolean opt_version = FALSE;

static GOptionEntry options[] = {
   { "session", '\0', G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &opt_session,
     "Use the session bus instead of the system bus", NULL },
   { "verbose", '\0', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &opt_verbose,
     "Be more verbose.", NULL },
   { "version", '\0', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &opt_version,
     "Print version number and exit.", NULL },
   { NULL }
};

/*
 * If @body is floating, this method will assume ownership of @body.
 */
static gboolean
_send_message(GDBusConnection *bus,
              const gchar *interface,
              const gchar *method,
              GVariant *body,
              GVariant **reply_out,
              GError **error)
{
   g_autoptr(GVariant) reply_body = NULL;
   g_autoptr(GDBusMessage) message = NULL;
   g_autoptr(GDBusMessage) reply = NULL;

   g_return_val_if_fail(reply_out == NULL || *reply_out == NULL, FALSE);

   message = g_dbus_message_new_method_call(AU_ATOMUPD1_BUS_NAME, AU_ATOMUPD1_PATH,
                                            interface, method);

   if (body != NULL)
      g_dbus_message_set_body(message, body);

   reply = g_dbus_connection_send_message_with_reply_sync(
      bus, message, G_DBUS_SEND_MESSAGE_FLAGS_NONE, 3000, NULL, NULL, error);
   if (reply != NULL && !g_dbus_message_to_gerror(reply, error)) {
      reply_body = g_dbus_message_get_body(reply);
      if (reply_body != NULL)
         g_variant_ref_sink(reply_body);

      if (reply_out != NULL)
         *reply_out = g_steal_pointer(&reply_body);

      return TRUE;
   }

   return FALSE;
}

static void
on_properties_changed(GDBusProxy *proxy,
                      GVariant *changed_properties,
                      const gchar *const *invalidated_properties,
                      gpointer user_data)
{
   g_autoptr(GVariantIter) iter = NULL;
   const gchar *key;
   GVariant *value; /* borrowed */
   gboolean interesting_change = FALSE;
   guint status;
   g_autoptr(GVariant) progress_prop = NULL;
   g_autoptr(GVariant) time_prop = NULL;
   g_autoptr(GVariant) status_prop = NULL;
   g_autoptr(GDateTime) estimated_time = NULL;
   g_autoptr(GDateTime) time_now = g_date_time_new_now_utc();
   GTimeSpan estimated_diff;
   GTimeSpan minutes_diff;
   GTimeSpan seconds_diff;

   if (g_variant_n_children(changed_properties) == 0)
      return;

   g_variant_get(changed_properties, "a{sv}", &iter);
   while (g_variant_iter_loop(iter, "{&sv}", &key, &value)) {
      if (g_strcmp0(key, "ProgressPercentage") == 0 ||
          g_strcmp0(key, "EstimatedCompletionTime") == 0 ||
          g_strcmp0(key, "UpdateStatus") == 0) {
         interesting_change = TRUE;
         /* According to the documentation, if we break out of the
          * g_variant_iter_loop(), we need to manually free the
          * GVariant. */
         g_variant_unref(value);
         break;
      }
   }

   if (!interesting_change)
      return;

   status_prop = g_dbus_proxy_get_cached_property(proxy, "UpdateStatus");
   status = g_variant_get_uint32(status_prop);
   if (status == AU_UPDATE_STATUS_SUCCESSFUL) {
      g_print("\nUpdate completed\n");
      g_main_loop_quit(main_loop);
      return;
   } else if (status == AU_UPDATE_STATUS_FAILED) {
      g_autoptr(GVariant) failure_code = NULL;
      g_autoptr(GVariant) failure_message = NULL;

      failure_code = g_dbus_proxy_get_cached_property(proxy, "FailureCode");
      failure_message = g_dbus_proxy_get_cached_property(proxy, "FailureMessage");
      g_print("\nThe update failed!\n");
      g_print("%s: %s\n", g_variant_get_string(failure_code, NULL),
              g_variant_get_string(failure_message, NULL));

      g_main_loop_quit(main_loop);
      return;
   }

   progress_prop = g_dbus_proxy_get_cached_property(proxy, "ProgressPercentage");
   time_prop = g_dbus_proxy_get_cached_property(proxy, "EstimatedCompletionTime");
   estimated_time = g_date_time_new_from_unix_utc(g_variant_get_uint64(time_prop));
   estimated_diff = g_date_time_difference(estimated_time, time_now);

   minutes_diff = estimated_diff / G_TIME_SPAN_MINUTE;
   seconds_diff = (estimated_diff / G_TIME_SPAN_SECOND) % 60;

   g_print("\r%.2f%%  ", g_variant_get_double(progress_prop));

   if (estimated_diff > 0) {
      if (minutes_diff > 0)
         g_print("%lim", minutes_diff);

      g_print("%02lis", seconds_diff);
   }

   fflush(stdout);
}

static gboolean
on_signal(gpointer user_data)
{
   GDBusConnection *bus = user_data; /* borrowed */

   g_debug("Caught signal. Stopping eventual updates.");

   /* If there is an update in progress, we try to avoid leaving it
    * running in background */
   _send_atomupd_message(bus, "CancelUpdate", NULL, NULL, NULL);

   g_main_loop_quit(main_loop);
   return G_SOURCE_REMOVE;
}

static int
print_usage(GOptionContext *context)
{
   g_autofree gchar *help = NULL;
   help = g_option_context_get_help(context, TRUE, NULL);
   g_print("%s\n", help);
   return EX_USAGE;
}

static void
print_update_info(GVariantIter *available)
{
   const gchar *buildid;
   GVariantIter *values_iter; /* borrowed */

   while (g_variant_iter_loop(available, "{&sa{sv}}", &buildid, &values_iter)) {
      const gchar *key;
      GVariant *value = NULL; /* borrowed */

      g_print("ID: %s", buildid);

      /* Unpack the variant instead of using g_variant_print() directly because
       * we want a consistent output format and we don't want to include the
       * values type */
      while (g_variant_iter_loop(values_iter, "{&sv}", &key, &value)) {
         g_autofree gchar *value_info = g_variant_print(value, FALSE);
         g_print(" - %s: %s", key, value_info);
      }
      g_print("\n");
   }
}

static int
check_updates(GOptionContext *context,
              GDBusConnection *bus,
              G_GNUC_UNUSED const gchar *argument)
{
   g_autoptr(GVariantIter) available = NULL;
   g_autoptr(GVariantIter) available_later = NULL;
   g_autoptr(GError) error = NULL;
   g_autoptr(GVariant) reply = NULL;

   if (!_send_atomupd_message(bus, "CheckForUpdates", g_variant_new("(a{sv})", NULL), &reply,
                              &error)) {
      g_print("An error occurred while checking for updates: %s\n", error->message);
      return EXIT_FAILURE;
   }

   g_variant_get(reply, "(a{?*}a{?*})", &available, &available_later);

   if (g_variant_iter_n_children(available) == 0 &&
       g_variant_iter_n_children(available_later) == 0) {
      g_print("No update available\n");
      return EXIT_SUCCESS;
   }

   if (g_variant_iter_n_children(available) > 0) {
      g_print("Updates available:\n");
      print_update_info(available);
   }

   if (g_variant_iter_n_children(available_later) > 0) {
      g_print("Updates available later:\n");
      print_update_info(available_later);
   }

   return EXIT_SUCCESS;
}

/*
 * Launch an update and wait until it either completes or fails
 */
static int
launch_update(GOptionContext *context, GDBusConnection *bus, const gchar *update_id)
{
   g_autoptr(GDBusProxy) proxy = NULL;
   g_autoptr(GError) error = NULL;
   g_autoptr(GVariant) reply = NULL;

   if (update_id == NULL) {
      g_print("It is not possible to apply an update without its ID\n\n");
      return print_usage(context);
   }

   main_loop = g_main_loop_new(NULL, FALSE);

   proxy = g_dbus_proxy_new_for_bus_sync(
      opt_session ? G_BUS_TYPE_SESSION : G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE,
      NULL, /* GDBusInterfaceInfo */
      AU_ATOMUPD1_BUS_NAME, AU_ATOMUPD1_PATH, AU_ATOMUPD1_INTERFACE,
      NULL, /* GCancellable */
      &error);

   if (proxy == NULL) {
      g_print("An error occurred while starting an update: %s\n", error->message);
      return EXIT_FAILURE;
   }

   g_signal_connect(proxy, "g-properties-changed", G_CALLBACK(on_properties_changed),
                    NULL);

   g_unix_signal_add(SIGINT, on_signal, bus);
   g_unix_signal_add(SIGTERM, on_signal, bus);

   if (!_send_atomupd_message(bus, "StartUpdate", g_variant_new("(s)", update_id), &reply,
                              &error)) {
      g_print("An error occurred while starting an update: %s\n", error->message);
      return EXIT_FAILURE;
   }

   g_main_loop_run(main_loop);

   return EXIT_SUCCESS;
}

static int
switch_variant(GOptionContext *context, GDBusConnection *bus, const gchar *variant)
{
   g_autoptr(GError) error = NULL;
   g_autoptr(GVariant) reply = NULL;

   if (variant == NULL) {
      g_print("The required variant has not been provided\n\n");
      return print_usage(context);
   }

   if (!_send_atomupd_message(bus, "SwitchToVariant", g_variant_new("(s)", variant), &reply,
                              &error)) {
      g_print("An error occurred while switching variant: %s\n", error->message);
      return EXIT_FAILURE;
   }

   return EXIT_SUCCESS;
}

static int
switch_branch(GOptionContext *context, GDBusConnection *bus, const gchar *branch)
{
   g_autoptr(GError) error = NULL;
   g_autoptr(GVariant) reply = NULL;

   if (branch == NULL) {
      g_print("The required branch has not been provided\n\n");
      return print_usage(context);
   }

   if (!_send_atomupd_message(bus, "SwitchToBranch", g_variant_new("(s)", branch), &reply,
                              &error)) {
      g_print("An error occurred while switching branch: %s\n", error->message);
      return EXIT_FAILURE;
   }

   return EXIT_SUCCESS;
}

static GVariant *
get_atomupd_property(GDBusConnection *bus, const gchar *property, GError **error)
{
   g_autoptr(GVariant) reply = NULL;
   g_autoptr(GVariant) variant_reply = NULL;
   GVariant *body = NULL; /* floating */

   body = g_variant_new("(ss)", AU_ATOMUPD1_INTERFACE, property);

   if (!_send_properties_message(bus, "Get", body, &reply, error))
      return NULL;

   g_variant_get(reply, "(v)", &variant_reply);

   return g_steal_pointer(&variant_reply);
}

static int
list_variants(G_GNUC_UNUSED GOptionContext *context,
              GDBusConnection *bus,
              G_GNUC_UNUSED const gchar *argument)
{
   g_autofree const gchar **variants = NULL;
   g_autoptr(GVariant) variant_reply = NULL;
   g_autoptr(GError) error = NULL;
   gsize length;

   variant_reply = get_atomupd_property(bus, "KnownVariants", &error);
   if (variant_reply == NULL) {
      g_print("An error occurred while listing known variants: %s\n", error->message);
      return EXIT_FAILURE;
   }

   variants = g_variant_get_strv(variant_reply, &length);

   for (gsize i = 0; i < length; i++)
      g_print("%s\n", variants[i]);

   return EXIT_SUCCESS;
}

static int
list_branches(G_GNUC_UNUSED GOptionContext *context,
              GDBusConnection *bus,
              G_GNUC_UNUSED const gchar *argument)
{
   g_autofree const gchar **variants = NULL;
   g_autoptr(GVariant) variant_reply = NULL;
   g_autoptr(GError) error = NULL;
   gsize length;

   variant_reply = get_atomupd_property(bus, "KnownBranches", &error);
   if (variant_reply == NULL) {
      g_print("An error occurred while listing known branches: %s\n", error->message);
      return EXIT_FAILURE;
   }

   variants = g_variant_get_strv(variant_reply, &length);

   for (gsize i = 0; i < length; i++)
      g_print("%s\n", variants[i]);

   return EXIT_SUCCESS;
}

static int
tracked_variant(G_GNUC_UNUSED GOptionContext *context,
                GDBusConnection *bus,
                G_GNUC_UNUSED const gchar *argument)
{
   g_autoptr(GVariant) variant_reply = NULL;
   g_autoptr(GError) error = NULL;

   variant_reply = get_atomupd_property(bus, "Variant", &error);
   if (variant_reply == NULL) {
      g_print("An error occurred while getting the variant: %s\n", error->message);
      return EXIT_FAILURE;
   }

   g_print("%s\n", g_variant_get_string(variant_reply, NULL));

   return EXIT_SUCCESS;
}

static int
tracked_branch(G_GNUC_UNUSED GOptionContext *context,
               GDBusConnection *bus,
               G_GNUC_UNUSED const gchar *argument)
{
   g_autoptr(GVariant) variant_reply = NULL;
   g_autoptr(GError) error = NULL;

   variant_reply = get_atomupd_property(bus, "Branch", &error);
   if (variant_reply == NULL) {
      g_print("An error occurred while getting the branch: %s\n", error->message);
      return EXIT_FAILURE;
   }

   g_print("%s\n", g_variant_get_string(variant_reply, NULL));

   return EXIT_SUCCESS;
}

typedef struct {
   const gchar *command;
   const gchar *argument;
   const gchar *description;
   int (*command_function)(GOptionContext *, GDBusConnection *, const gchar *);
} LaunchCommands;

static const LaunchCommands launch_commands[] = {
   {
      .command = "check",
      .description = "Check for available OS updates",
      .command_function = check_updates,
   },

   {
      .command = "update",
      .argument = "ID",
      .description = "Apply the update build ID",
      .command_function = launch_update,
   },

   {
      .command = "switch-variant",
      .argument = "VARIANT",
      .description = "Select a different variant",
      .command_function = switch_variant,
   },

   {
      .command = "switch-branch",
      .argument = "BRANCH",
      .description = "Select a different branch",
      .command_function = switch_branch,
   },

   {
      .command = "list-variants",
      .description = "List the known variants",
      .command_function = list_variants,
   },

   {
      .command = "list-branches",
      .description = "List the known branches",
      .command_function = list_branches,
   },

   {
      .command = "tracked-variant",
      .description = "Get the variant that is currently being tracked",
      .command_function = tracked_variant,
   },

   {
      .command = "tracked-branch",
      .description = "Get the branch that is currently being tracked",
      .command_function = tracked_branch,
   },
};

/*
 * Creates a string with the commands and their description, e.g.:
 *
 * switch-variant VARIANT    Select a different variant
 * [...]
 */
static gchar *
commands_summary(const LaunchCommands *commands, gsize commands_len)
{
   GString *string = g_string_new("");
   gsize i;
   gsize alignment = 0;
   /* Every new line has two leading spaces */
   gsize initial_spacing = 2;
   /* Add a few spaces before the command description */
   gsize final_spacing = 5;

   g_return_val_if_fail(commands != NULL, NULL);
   g_return_val_if_fail(commands_len > 0, NULL);

   /* Calculate an adequate common alignment for the commands description */
   for (i = 0; i < commands_len; i++) {
      gsize cmd_len = strlen(commands[i].command);
      gsize argument_len =
         commands[i].argument == NULL ? 0 : strlen(commands[i].argument);

      if (alignment < (cmd_len + argument_len))
         alignment = cmd_len + argument_len;
   }

   alignment += initial_spacing + final_spacing;

   for (i = 0; i < commands_len; i++) {
      g_autoptr(GString) line = g_string_new("");

      /* Use a brand new GString for every line to easily keep track of
       * their length */
      g_string_append_printf(line, "  %s", commands[i].command);

      if (commands[i].argument != NULL)
         g_string_append_printf(line, " %s", commands[i].argument);

      while (line->len < alignment)
         g_string_append_c(line, ' ');

      g_string_append_printf(line, "%s\n", commands[i].description);

      g_string_append(string, line->str);
   }

   /* Remove the last newline because GOptionContext will automatically
    * add it too. */
   g_string_erase(string, string->len - 1, 1);

   return g_string_free(string, FALSE);
}

int
main(int argc, char *argv[])
{
   g_autoptr(GError) error = NULL;
   g_autoptr(GOptionContext) context = NULL;
   g_autoptr(GDBusConnection) bus = NULL;
   g_autofree gchar *summary = NULL;

   context = g_option_context_new("");
   g_option_context_add_main_entries(context, options, NULL);
   summary = commands_summary(launch_commands, G_N_ELEMENTS(launch_commands));
   g_option_context_set_summary(context, summary);
   g_option_context_set_description(context,
                                    "This tool let developers to control "
                                    "atomupd-daemon, allowing them to check and install "
                                    "OS updates.");

   if (!g_option_context_parse(context, &argc, &argv, &error)) {
      g_print("%s\n", error->message);
      return print_usage(context);
   }

   if (opt_version) {
      g_print("%s:\n"
              " Package: atomupd-daemon\n"
              " Version: %s\n",
              g_get_prgname(), VERSION);
      return EXIT_SUCCESS;
   }

   if (opt_verbose)
      g_log_set_handler(G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, g_log_default_handler, NULL);

   bus = g_bus_get_sync(opt_session ? G_BUS_TYPE_SESSION : G_BUS_TYPE_SYSTEM, NULL, NULL);

   for (gsize i = 0; i < G_N_ELEMENTS(launch_commands); i++) {
      if (g_strcmp0(launch_commands[i].command, argv[1]) == 0)
         return launch_commands[i].command_function(context, bus, argv[2]);
   }

   return print_usage(context);
}
