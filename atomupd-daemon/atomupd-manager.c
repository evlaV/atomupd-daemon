/*
 * Copyright © 2023-2024 Collabora Ltd.
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
#include <systemd/sd-journal.h>

#include "enums.h"
#include "utils.h"

#define _send_atomupd_message(_bus, _method, _body, _reply_out, _error)                  \
   _send_message(_bus, AU_ATOMUPD1_PATH, AU_ATOMUPD1_INTERFACE, _method, _body,          \
                 _reply_out, _error)

#define _send_properties_message(_bus, _method, _body, _reply_out, _error)               \
   _send_message(_bus, AU_ATOMUPD1_PATH, "org.freedesktop.DBus.Properties", _method,     \
                 _body, _reply_out, _error)

G_DEFINE_AUTOPTR_CLEANUP_FUNC(sd_journal, sd_journal_close)

static const gchar *AU_CONFIG = "client.conf";
static const gchar *AU_DEV_CONFIG = "client-dev.conf";

static GMainLoop *main_loop = NULL;
static int main_loop_result = EXIT_SUCCESS;

static gboolean opt_session = FALSE;
static gboolean opt_verbose = FALSE;
static gboolean opt_penultimate = FALSE;
static gboolean opt_version = FALSE;
static gboolean opt_skip_reload = FALSE;
static gchar **opt_additional_variants = NULL;
static gchar *opt_username = NULL;
static gchar *opt_password = NULL;

static GOptionEntry options[] = {
   { "session", '\0', G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &opt_session,
     "Use the session bus instead of the system bus", NULL },
   { "verbose", '\0', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &opt_verbose,
     "Be more verbose, including debug messages from atomupd-daemon.", NULL },
   { "penultimate-update", '\0', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE,
     &opt_penultimate, "Request the penultimate update that has been released", NULL },
   { "version", '\0', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &opt_version,
     "Print version number and exit.", NULL },
   { NULL }
};

static GOptionEntry create_dev_conf_options[] = {
   { "additional-variant", '\0', G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING_ARRAY,
     &opt_additional_variants, "Additional known variant, can be repeated", "VARIANT" },
   { "username", '\0', G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, &opt_username,
     "Username for the eventual HTTP authentication", NULL },
   { "password", '\0', G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, &opt_password,
     "Password for the eventual HTTP authentication", NULL },
   { "skip-reload", '\0', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &opt_skip_reload,
     "Do not execute the ReloadConfiguration method of the API", NULL },
   { NULL }
};

static void
log_handler(const gchar *log_domain,
            GLogLevelFlags log_level,
            const gchar *message,
            gpointer user_data)
{
   const gchar *level_prefix;

   /* These are the only expected log levels that we handle. All the other more
    * severe errors are left to the default handler */
   if (log_level & G_LOG_LEVEL_WARNING)
      level_prefix = "W";
   else if (log_level & G_LOG_LEVEL_MESSAGE)
      level_prefix = "N"; /* consistent with apt, which calls this a "notice" */
   else if (log_level & G_LOG_LEVEL_INFO)
      level_prefix = "I";
   else if (log_level & G_LOG_LEVEL_DEBUG)
      level_prefix = "D";
   else
      level_prefix = "?!";

   g_printerr("%s[%s]: %s\n", g_get_prgname(), level_prefix, message);
}

/*
 * If @body is floating, this method will assume ownership of @body.
 */
static gboolean
_send_message(GDBusConnection *bus,
              const gchar *path,
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

   message =
      g_dbus_message_new_method_call(AU_ATOMUPD1_BUS_NAME, path, interface, method);

   if (body != NULL)
      g_dbus_message_set_body(message, body);

   /* The D-Bus response is usually immediate. However, on very slow Internet connections
    * it could take up to a few seconds to download the meta JSON file for the available
    * updates. If, for whatever reason, the server is not providing the pre-estimated
    * update size, we may also need to download an additional extra file.
    * To err on the safe side, we set a timeout of 30s */
   reply = g_dbus_connection_send_message_with_reply_sync(
      bus, message, G_DBUS_SEND_MESSAGE_FLAGS_NONE, 30000, NULL, NULL, error);
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
   g_autoptr(GVariant) failure_code = NULL;
   g_autoptr(GVariant) failure_message = NULL;
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
   switch (status) {
   case AU_UPDATE_STATUS_SUCCESSFUL:
      g_print("\nUpdate completed\n");
      g_main_loop_quit(main_loop);
      return;
   case AU_UPDATE_STATUS_FAILED:
      failure_code = g_dbus_proxy_get_cached_property(proxy, "FailureCode");
      failure_message = g_dbus_proxy_get_cached_property(proxy, "FailureMessage");
      g_print("\nThe update failed!\n");
      g_print("%s: %s\n", g_variant_get_string(failure_code, NULL),
              g_variant_get_string(failure_message, NULL));

      main_loop_result = EXIT_FAILURE;
      g_main_loop_quit(main_loop);
      return;
   case AU_UPDATE_STATUS_CANCELLED:
   case AU_UPDATE_STATUS_IDLE:
   case AU_UPDATE_STATUS_IN_PROGRESS:
   case AU_UPDATE_STATUS_PAUSED:
   default:
      break;
   }

   progress_prop = g_dbus_proxy_get_cached_property(proxy, "ProgressPercentage");
   time_prop = g_dbus_proxy_get_cached_property(proxy, "EstimatedCompletionTime");
   estimated_time = g_date_time_new_from_unix_utc(g_variant_get_uint64(time_prop));
   estimated_diff = g_date_time_difference(estimated_time, time_now);

   minutes_diff = estimated_diff / G_TIME_SPAN_MINUTE;
   seconds_diff = (estimated_diff / G_TIME_SPAN_SECOND) % 60;

   /* The second \r is necessary to avoid breaking the legacy steamos-update
    * script parsing */
   g_print("\r\033[K\r%.2f%%  ", g_variant_get_double(progress_prop));

   if (estimated_diff > 0) {
      if (minutes_diff > 0)
         g_print("%lim", minutes_diff);

      g_print("%02lis", seconds_diff);
   }

   /* Print newlines when using the verbose mode in order to have a more readable output */
   if (opt_verbose)
      g_print("\n");

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

static gboolean
ensure_daemon_debug_enabled(GDBusConnection *bus,
                            gboolean *edited_value_out,
                            GError **error)
{
   g_autoptr(GVariant) reply = NULL;
   g_autoptr(GVariant) variant_reply = NULL;
   GVariant *body = NULL; /* floating */
   gboolean previous_value;

   g_return_val_if_fail(edited_value_out != NULL, FALSE);

   body = g_variant_new("(ss)", "org.gtk.Debugging", "DebugEnabled");

   if (!_send_message(bus, "/org/gtk/Debugging", "org.freedesktop.DBus.Properties", "Get",
                      body, &reply, error))
      return FALSE;

   g_variant_get(reply, "(v)", &variant_reply);

   previous_value = g_variant_get_boolean(variant_reply);

   if (previous_value) {
      g_debug("Debugging for the atomupd daemon is already enabled");
      *edited_value_out = FALSE;
      return TRUE;
   }

   if (!_send_message(bus, "/org/gtk/Debugging", "org.gtk.Debugging", "SetDebugEnabled",
                      g_variant_new("(b)", TRUE), NULL, error)) {
      return FALSE;
   }

   *edited_value_out = TRUE;

   return TRUE;
}

static void
print_journal_messages(sd_journal *journal)
{
   while (sd_journal_next(journal) > 0) {
      const char *message;
      const char *field;
      size_t message_len;
      size_t field_len;
      int ret;

      ret = sd_journal_get_data(journal, "MESSAGE", (const void **)&field, &field_len);
      if (ret < 0)
         continue;

      /* Remove the field prefix */
      message = (const char *)memchr(field, '=', field_len);
      if (message == NULL)
         continue;

      message++;
      message_len = field_len - (message - field);

      g_debug("%.*s", (int)message_len, message);
   }
}

static gboolean
journal_callback(G_GNUC_UNUSED GIOChannel *channel,
                 G_GNUC_UNUSED GIOCondition condition,
                 gpointer user_data)
{
   sd_journal *journal = user_data;
   print_journal_messages(journal);
   return TRUE;
}

static sd_journal *
open_atomupd_daemon_journal(GError **error)
{
   g_autoptr(sd_journal) journal = NULL;
   int ret;
   int flags = SD_JOURNAL_LOCAL_ONLY;

   if (!opt_session)
      flags |= SD_JOURNAL_SYSTEM;

   ret = sd_journal_open(&journal, flags);
   if (ret < 0)
      return au_throw_error_null(error, "Failed to open the journal: %s",
                                 g_strerror(-ret));

   ret = sd_journal_add_match(journal, "SYSLOG_IDENTIFIER=atomupd-daemon", 0);
   if (ret < 0)
      return au_throw_error_null(error, "Failed to add a match for the journal: %s",
                                 g_strerror(-ret));

   ret = sd_journal_seek_tail(journal);
   if (ret < 0)
      return au_throw_error_null(error, "Failed to move to the end of the journal: %s",
                                 g_strerror(-ret));

   ret = sd_journal_previous(journal);
   if (ret < 0)
      return au_throw_error_null(error, "Failed to move the journal head position: %s",
                                 g_strerror(-ret));

   return g_steal_pointer(&journal);
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
   g_auto(GVariantBuilder) builder;
   g_autoptr(sd_journal) journal = NULL;
   gboolean edited_debug_value = FALSE;
   gboolean ret;

   g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));

   if (opt_verbose) {
      journal = open_atomupd_daemon_journal(&error);
      if (journal == NULL) {
         g_print("%s", error->message);
         return EXIT_FAILURE;
      }

      if (!ensure_daemon_debug_enabled(bus, &edited_debug_value, &error)) {
         g_print("%s", error->message);
         return EXIT_FAILURE;
      }
   }

   if (opt_penultimate)
      g_variant_builder_add(&builder, "{sv}", "penultimate", g_variant_new_boolean(TRUE));

   ret = _send_atomupd_message(bus, "CheckForUpdates", g_variant_new("(a{sv})", &builder),
                               &reply, &error);

   if (opt_verbose) {
      print_journal_messages(journal);

      if (edited_debug_value) {
         /* Reset the debug flag to its original value */
         g_autoptr(GError) local_error = NULL;
         if (!_send_message(bus, "/org/gtk/Debugging", "org.gtk.Debugging",
                            "SetDebugEnabled", g_variant_new("(b)", FALSE), NULL,
                            &local_error)) {
            g_warning("Failed to restore the debug value of atomupd-daemon: %s",
                      local_error->message);
         }
      }
   }

   if (!ret) {
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
launch_update(GDBusConnection *bus, const gchar *update_id, const gchar *update_url)
{
   g_autoptr(GDBusProxy) proxy = NULL;
   g_autoptr(GError) error = NULL;
   GVariant *body = NULL; /* floating */
   g_autoptr(GVariant) reply = NULL;
   g_autoptr(sd_journal) journal = NULL;
   g_autoptr(GIOChannel) channel = NULL;
   gboolean edited_debug_value = FALSE;
   const gchar *method = NULL;

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

   if (opt_verbose) {
      journal = open_atomupd_daemon_journal(&error);
      if (journal == NULL) {
         g_print("%s", error->message);
         return EXIT_FAILURE;
      }

      int fd = sd_journal_get_fd(journal);
      channel = g_io_channel_unix_new(fd);
      g_io_add_watch(channel, G_IO_IN, (GIOFunc)journal_callback, journal);

      if (!ensure_daemon_debug_enabled(bus, &edited_debug_value, &error)) {
         g_print("%s", error->message);
         return EXIT_FAILURE;
      }
   }

   if (update_url != NULL) {
      GVariantBuilder builder;

      method = "StartCustomUpdate";
      g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
      g_variant_builder_add(&builder, "{sv}", "url", g_variant_new_string(update_url));
      body = g_variant_new("(@a{sv})", g_variant_builder_end(&builder));
   } else {
      method = "StartUpdate";
      body = g_variant_new("(s)", update_id);
   }

   if (!_send_atomupd_message(bus, method, body, &reply, &error)) {
      g_print("An error occurred while starting the update: %s\n", error->message);
      main_loop_result = EXIT_FAILURE;
      goto cleanup;
   }

   g_main_loop_run(main_loop);

cleanup:
   if (edited_debug_value) {
      /* Reset the debug flag to its original value */
      g_clear_error(&error);
      if (!_send_message(bus, "/org/gtk/Debugging", "org.gtk.Debugging",
                         "SetDebugEnabled", g_variant_new("(b)", FALSE), NULL, &error)) {
         g_warning("Failed to restore the debug value of atomupd-daemon: %s",
                   error->message);
      }
   }

   return main_loop_result;
}

static int
update_command(GOptionContext *context, GDBusConnection *bus, const gchar *update_id)
{
   if (update_id == NULL) {
      g_print("It is not possible to apply an update without its ID\n\n");
      return print_usage(context);
   }

   return launch_update(bus, update_id, NULL);
}

static int
custom_update_command(GOptionContext *context,
                      GDBusConnection *bus,
                      const gchar *update_url)
{
   if (update_url == NULL) {
      g_print("It is not possible to apply an update without its URL\n\n");
      return print_usage(context);
   }

   return launch_update(bus, NULL, update_url);
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

   if (g_str_equal(branch, "staging"))
      g_print("The staging branch has a high risk of breaking.\n"
              "Do NOT use it unless you know what you are doing.\n");

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

static int
update_status(G_GNUC_UNUSED GOptionContext *context,
              GDBusConnection *bus,
              G_GNUC_UNUSED const gchar *argument)
{
   g_autoptr(GVariant) variant_reply = NULL;
   g_autoptr(GError) error = NULL;
   g_autoptr(GEnumClass) class = NULL;
   GEnumValue *enum_value = NULL;
   guint status;

   variant_reply = get_atomupd_property(bus, "UpdateStatus", &error);
   if (variant_reply == NULL) {
      g_print("An error occurred while getting the update status: %s\n", error->message);
      return EXIT_FAILURE;
   }

   status = g_variant_get_uint32(variant_reply);
   class = g_type_class_ref(AU_TYPE_UPDATE_STATUS);
   enum_value = g_enum_get_value(class, status);

   if (enum_value == NULL) {
      g_print("The update status is unknown\n");
      return EXIT_FAILURE;
   }

   g_print("%s\n", enum_value->value_nick);

   return EXIT_SUCCESS;
}

static int
create_dev_conf(G_GNUC_UNUSED GOptionContext *context,
                GDBusConnection *bus,
                G_GNUC_UNUSED const gchar *argument)
{
   const gchar *config_dir = NULL;
   g_autofree gchar *initial_variants = NULL;
   g_autoptr(GString) variants = NULL;
   g_autofree gchar *config_path = NULL;
   g_autofree gchar *dev_config_path = NULL;
   g_autoptr(GError) error = NULL;
   g_autoptr(GKeyFile) client_config = g_key_file_new();
   gsize i;

   /* This environment variable is used for debugging and automated tests */
   config_dir = g_getenv("AU_CONFIG_DIR");
   if (config_dir == NULL)
      config_dir = "/etc/steamos-atomupd";

   config_path = g_build_filename(config_dir, AU_CONFIG, NULL);

   if (!g_key_file_load_from_file(client_config, config_path, G_KEY_FILE_NONE, &error)) {
      g_print("An error occurred while loading the client configuration: %s\n",
              error->message);
      return EXIT_FAILURE;
   }

   if (opt_username)
      g_key_file_set_string(client_config, "Server", "Username", opt_username);

   if (opt_password)
      g_key_file_set_string(client_config, "Server", "Password", opt_password);

   if (opt_additional_variants) {
      initial_variants =
         g_key_file_get_string(client_config, "Server", "Variants", &error);

      if (error != NULL) {
         g_print("An error occurred while loading the Variants from the client "
                 "configuration: %s\n",
                 error->message);
         return EXIT_FAILURE;
      }

      if (g_str_has_suffix(initial_variants, ";"))
         initial_variants[strlen(initial_variants) - 1] = 0;

      variants = g_string_new(initial_variants);

      for (i = 0; opt_additional_variants[i] != NULL; i++)
         g_string_append_printf(variants, ";%s", opt_additional_variants[i]);

      g_key_file_set_string(client_config, "Server", "Variants", variants->str);
   }

   dev_config_path = g_build_filename(config_dir, AU_DEV_CONFIG, NULL);
   if (!g_key_file_save_to_file(client_config, dev_config_path, &error)) {
      g_print("An error occurred while creating the dev client configuration: %s\n",
              error->message);
      return EXIT_FAILURE;
   }

   if (opt_skip_reload)
      return EXIT_SUCCESS;

   if (!_send_atomupd_message(bus, "ReloadConfiguration", g_variant_new("(a{sv})", NULL),
                              NULL, &error)) {
      g_print("An error occurred while reloading the configuration: %s\n",
              error->message);
      return EXIT_FAILURE;
   }

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
      .command_function = update_command,
   },

   {
      .command = "custom-update",
      .argument = "URL",
      .description = "Apply a custom update from a specific RAUC bundle",
      .command_function = custom_update_command,
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

   {
      .command = "get-update-status",
      .description = "Get the update status, possible values are: idle, in-progress, "
                     "paused, successful, failed, cancelled",
      .command_function = update_status,
   },

   {
      .command = "create-dev-conf",
      .description = "Create a custom client-dev.conf file for the atomic updates",
      .command_function = create_dev_conf,
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
   GOptionGroup *dev_config_group = NULL;
   g_autoptr(GDBusConnection) bus = NULL;
   g_autofree gchar *summary = NULL;
   GLogLevelFlags log_levels = G_LOG_LEVEL_MESSAGE | G_LOG_LEVEL_WARNING;

   context = g_option_context_new("");
   g_option_context_add_main_entries(context, options, NULL);
   summary = commands_summary(launch_commands, G_N_ELEMENTS(launch_commands));
   g_option_context_set_summary(context, summary);
   g_option_context_set_description(context,
                                    "This tool let developers to control "
                                    "atomupd-daemon, allowing them to check and install "
                                    "OS updates.");

   dev_config_group = g_option_group_new("create-dev-conf", "create-dev-conf Options:",
                                         "Show create-dev-conf help options", NULL, NULL);
   g_option_group_add_entries(dev_config_group, create_dev_conf_options);
   g_option_context_add_group(context, dev_config_group);

   if (!g_option_context_parse(context, &argc, &argv, &error)) {
      g_print("%s\n", error->message);
      return print_usage(context);
   }

   if (opt_verbose)
      log_levels |= G_LOG_LEVEL_DEBUG | G_LOG_LEVEL_INFO;

   g_log_set_handler(G_LOG_DOMAIN, log_levels, log_handler, NULL);

   if (opt_version) {
      g_print("%s:\n"
              " Package: atomupd-daemon\n"
              " Version: %s\n",
              g_get_prgname(), VERSION);
      return EXIT_SUCCESS;
   }

   if (argc < 2)
      return print_usage(context);

   if (!g_str_equal(argv[1], "create-dev-conf")) {
      /* These options are only relevant for the create-dev-conf command */
      if (opt_additional_variants != NULL || opt_username != NULL ||
          opt_password != NULL || opt_skip_reload)
         return print_usage(context);
   }

   /* The authentication requires both username and password to be set */
   if ((opt_username == NULL && opt_password != NULL) || (opt_username != NULL && opt_password == NULL))
      return print_usage(context);

   bus = g_bus_get_sync(opt_session ? G_BUS_TYPE_SESSION : G_BUS_TYPE_SYSTEM, NULL, NULL);

   for (gsize i = 0; i < G_N_ELEMENTS(launch_commands); i++) {
      if (g_strcmp0(launch_commands[i].command, argv[1]) == 0)
         return launch_commands[i].command_function(context, bus, argv[2]);
   }

   return print_usage(context);
}
