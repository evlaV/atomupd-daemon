/*
 * Copyright © 2022 Collabora Ltd.
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
#include <libelf.h>
#include <string.h>
#include <unistd.h>

#include <gio/gio.h>
#include <glib-unix.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "atomupd-daemon/utils.h"
#include "fixture.h"
#include "mock-defines.h"
#include "services.h"

#define _send_atomupd_message_with_null_reply(_bus, _method, _type, _content)            \
   g_assert_null(_send_atomupd_message(_bus, _method, _type, _content))

#define _send_atomupd_message(_bus, _method, _format, ...)                               \
   _send_message(_bus, AU_ATOMUPD1_INTERFACE, _method, _format, __VA_ARGS__)

#define _send_properties_message(_bus, _method, _format, ...)                            \
   _send_message(_bus, "org.freedesktop.DBus.Properties", _method, _format, __VA_ARGS__)

#define _skip_if_daemon_is_running(_bus, _error)                                         \
   if (au_tests_is_daemon_service_running(bus, _error)) {                                \
      g_test_skip("Can't run this test if another instance of the Atomupd "              \
                  "daemon service is already running");                                  \
      return;                                                                            \
   }

static gulong default_wait = 0.5 * G_USEC_PER_SEC;

typedef struct {
   guint32 version;
   gdouble progress_percentage;
   guint64 estimated_completion_time;
   gsize versions_available_n;
   gsize versions_available_later_n;
   AuUpdateStatus status;
   gchar *update_version;
   gchar *variant;
   gchar *failure_code;
   gchar *failure_message;
   gchar *current_version;
   GStrv known_variants;
} AtomupdProperties;

static void
atomupd_properties_free(AtomupdProperties *atomupd_properties)
{
   g_clear_pointer(&atomupd_properties->update_version, g_free);
   g_clear_pointer(&atomupd_properties->variant, g_free);
   g_clear_pointer(&atomupd_properties->failure_code, g_free);
   g_clear_pointer(&atomupd_properties->failure_message, g_free);
   g_clear_pointer(&atomupd_properties->current_version, g_free);
   g_clear_pointer(&atomupd_properties->known_variants, g_strfreev);

   g_free(atomupd_properties);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AtomupdProperties, atomupd_properties_free)

typedef struct {
   const gchar *buildid;
   const gchar *variant;
   guint64 estimated_size;
   AuUpdateType update_type; /* Defaults to AU_UPDATE_TYPE_MINOR */
   const gchar *requires_buildid;
} VersionsTest;

typedef struct {
   const gchar *update_json;
   const gchar *reboot_for_update;
   VersionsTest versions_available[3];
   VersionsTest versions_available_later[3];
} UpdatesTest;

static const UpdatesTest updates_test[] =
{
  {
    .update_json = "update_one_minor.json",
    .versions_available =
    {
      {
        .buildid = "20220227.3",
        .variant = "steamdeck",
        .estimated_size = 70910463,
      },
    },
  },

  {
    .update_json = "update_empty.json",
  },

  {
    .update_json = "update_one_minor_one_major.json",
    .versions_available =
    {
      {
        .buildid = "20220120.1",
        .variant = "steamdeck",
      },
      {
        .buildid = "20220202.1",
        .variant = "steamdeck",
        .update_type = AU_UPDATE_TYPE_MAJOR,
      },
    },
  },

  {
    .update_json = "update_three_minors.json",
    .versions_available =
    {
      {
        .buildid = "20211225.1",
        .variant = "steamdeck",
        .estimated_size = 40310422,
      },
    },
    .versions_available_later =
    {
      {
        .buildid = "20220101.1",
        .variant = "steamdeck",
        .requires_buildid = "20211225.1",
      },
      {
        .buildid = "20220227.3",
        .variant = "steamdeck",
        .estimated_size = 30410461,
        .requires_buildid = "20220101.1",
      },
    },
  },

  {
    .update_json = "update_two_minors_two_majors.json",
    .versions_available =
    {
      {
        .buildid = "20220110.1",
        .variant = "steamdeck",
        .estimated_size = 4815162342,
      },
      {
        .buildid = "20220201.5",
        .variant = "steamdeck",
        .update_type = AU_UPDATE_TYPE_MAJOR,
      },
    },
    .versions_available_later =
    {
      {
        .buildid = "20220120.1",
        .variant = "steamdeck",
        .requires_buildid = "20220110.1",
      },
      {
        .buildid = "20220202.1",
        .variant = "steamdeck",
        .requires_buildid = "20220201.5",
        .update_type = AU_UPDATE_TYPE_MAJOR,
      },
    }
  },
};

static const UpdatesTest pending_reboot_test[] =
{
  {
    .update_json = "update_one_minor.json",
    /* Pending a different version than the proposed update */
    .reboot_for_update = "20220222.1",
    .versions_available =
    {
      {
        .buildid = "20220227.3",
        .variant = "steamdeck",
        .estimated_size = 70910463,
      },
    },
  },

  {
    .update_json = "update_one_minor.json",
    /* The single update proposed has already been applied */
    .reboot_for_update = "20220227.3",
  },

  {
    .update_json = "update_one_minor_one_major.json",
    /* The proposed minor update has already been applied */
    .reboot_for_update = "20220120.1",
    .versions_available =
    {
      {
        .buildid = "20220202.1",
        .variant = "steamdeck",
        .update_type = AU_UPDATE_TYPE_MAJOR,
      },
    },
  },

  {
    .update_json = "update_three_minors.json",
    /* The minor update has already been applied */
    .reboot_for_update = "20211225.1",
  },

  {
    .update_json = "update_three_minors.json",
    /* This could probably happen when a downgrade is requested.
     * In this situation the daemon shows the available versions as-is,
     * given that the "later" versions cannot be installed without
     * first fulfilling their requirements */
    .reboot_for_update = "20220101.1",
    .versions_available =
    {
      {
        .buildid = "20211225.1",
        .variant = "steamdeck",
        .estimated_size = 40310422,
      },
    },
    .versions_available_later =
    {
      {
        .buildid = "20220101.1",
        .variant = "steamdeck",
        .requires_buildid = "20211225.1",
      },
      {
        .buildid = "20220227.3",
        .variant = "steamdeck",
        .estimated_size = 30410461,
        .requires_buildid = "20220101.1",
      },
    },
  },
};

/*
 * If @body is floating, this method will assume ownership of @body.
 */
static GVariant *
_send_message(GDBusConnection *bus,
              const gchar *interface,
              const gchar *method,
              const gchar *format_message,
              ...)
{
   GVariant *reply_body = NULL;
   va_list ap;
   g_autofree gchar *variant_string = NULL;
   g_autoptr(GDBusMessage) message = NULL;
   g_autoptr(GDBusMessage) reply = NULL;
   g_autoptr(GError) error = NULL;

   message = g_dbus_message_new_method_call(AU_ATOMUPD1_BUS_NAME, AU_ATOMUPD1_PATH,
                                            interface, method);

   if (format_message != NULL) {
      va_start(ap, format_message);
      g_dbus_message_set_body(message, g_variant_new_va(format_message, NULL, &ap));
      va_end(ap);
   }

   reply = g_dbus_connection_send_message_with_reply_sync(
      bus, message, G_DBUS_SEND_MESSAGE_FLAGS_NONE, 3000, NULL, NULL, &error);
   g_assert_no_error(error);

   reply_body = g_dbus_message_get_body(reply);

   if (reply_body == NULL) {
      g_debug("The method \"%s\" didn't return anything", method);
      return NULL;
   }

   variant_string = g_variant_print(reply_body, TRUE);
   g_debug("Reply body of \"%s\": %s", method, variant_string);

   return g_variant_ref_sink(reply_body);
}

static void
_check_available_versions(GVariantIter *available_iter,
                          const VersionsTest *versions_available)
{
   gchar *buildid;          /* borrowed */
   GVariant *values = NULL; /* borrowed */
   g_autoptr(GVariantType) type_string = g_variant_type_new("s");
   g_autoptr(GVariantType) type_uint64 = g_variant_type_new("t");
   g_autoptr(GVariantType) type_uint32 = g_variant_type_new("u");
   gsize i;

   for (i = 0; g_variant_iter_loop(available_iter, "{s@a{sv}}", &buildid, &values); i++) {
      g_autoptr(GVariant) variant = NULL;
      g_autoptr(GVariant) estimated_size = NULL;
      g_autoptr(GVariant) update_type = NULL;
      g_autoptr(GVariant)
         requires
      = NULL;
      const gchar *requires_str = NULL;
      const VersionsTest *expected_update = &versions_available[i];

      g_assert_cmpstr(expected_update->buildid, ==, buildid);

      variant = g_variant_lookup_value(values, "variant", type_string);
      g_assert_cmpstr(expected_update->variant, ==, g_variant_get_string(variant, NULL));

      estimated_size = g_variant_lookup_value(values, "estimated_size", type_uint64);
      g_assert_cmpuint(expected_update->estimated_size, ==,
                       g_variant_get_uint64(estimated_size));

      update_type = g_variant_lookup_value(values, "update_type", type_uint32);
      g_assert_cmpuint(expected_update->update_type, ==,
                       g_variant_get_uint32(update_type));

      requires = g_variant_lookup_value(values, "requires", type_string);
      requires_str =
         requires ==
                     NULL ? NULL : g_variant_get_string(requires, NULL);
      g_assert_cmpstr(expected_update->requires_buildid, ==, requires_str);
   }
   g_assert_cmpstr(versions_available[i].buildid, ==, NULL);
}

static GVariant *
_get_atomupd_property(GDBusConnection *bus, const gchar *property)
{
   g_autoptr(GVariant) reply = NULL;
   g_autoptr(GVariant) reply_variant = NULL;

   g_debug("Getting the \"%s\" property", property);
   reply = _send_properties_message(bus, "Get", "(ss)", AU_ATOMUPD1_INTERFACE, property);
   g_variant_get(reply, "(v)", &reply_variant);

   return g_steal_pointer(&reply_variant);
}

static void
_check_versions_property(GDBusConnection *bus,
                         const gchar *property,
                         const VersionsTest *versions_available)
{
   g_autoptr(GVariant) reply = NULL;
   g_autoptr(GVariantIter) versions_iter = NULL;

   reply = _get_atomupd_property(bus, property);
   g_variant_get(reply, "a{?*}", &versions_iter);

   _check_available_versions(versions_iter, versions_available);
}

static void
_check_string_property(GDBusConnection *bus,
                       const gchar *property,
                       const gchar *expected_value)
{
   g_autoptr(GVariant) reply = NULL;
   const gchar *value;

   reply = _get_atomupd_property(bus, property);
   value = g_variant_get_string(reply, NULL);

   g_assert_cmpstr(value, ==, expected_value);
}

static void
_call_check_for_updates(GDBusConnection *bus,
                        const VersionsTest *versions_available,
                        const VersionsTest *versions_available_later)
{
   g_autoptr(GVariant) reply = NULL;
   g_autoptr(GVariantIter) available_iter = NULL;
   g_autoptr(GVariantIter) available_later_iter = NULL;

   g_debug("Calling the \"CheckForUpdates\" method");

   reply = _send_atomupd_message(bus, "CheckForUpdates", "(a{sv})", NULL);
   g_assert_nonnull(reply);

   g_variant_get(reply, "(a{?*}a{?*})", &available_iter, &available_later_iter);

   if (versions_available != NULL)
      _check_available_versions(available_iter, versions_available);

   if (versions_available_later != NULL)
      _check_available_versions(available_later_iter, versions_available_later);
}

static void
_query_for_updates(Fixture *f, GDBusConnection *bus, const UpdatesTest *test)
{
   g_autoptr(GSubprocess) daemon_proc = NULL;
   g_autofree gchar *update_file_path = NULL;
   g_autofree gchar *reboot_for_update = NULL;
   g_autoptr(GError) error = NULL;

   update_file_path = g_build_filename(f->srcdir, "data", test->update_json, NULL);
   f->test_envp =
      g_environ_setenv(f->test_envp, "G_TEST_UPDATE_JSON", update_file_path, TRUE);

   if (test->reboot_for_update != NULL) {
      int fd;

      fd = g_file_open_tmp("reboot-for-update-XXXXXX", &reboot_for_update, &error);
      g_assert_no_error(error);
      close(fd);
      g_file_set_contents(reboot_for_update, test->reboot_for_update, -1, &error);
      g_assert_no_error(error);

      f->test_envp =
         g_environ_setenv(f->test_envp, "AU_REBOOT_FOR_UPDATE", reboot_for_update, TRUE);
   }

   daemon_proc =
      au_tests_start_daemon_service(bus, f->manifest_path, f->conf_path, f->test_envp);

   _call_check_for_updates(bus, test->versions_available, test->versions_available_later);

   _check_versions_property(bus, "VersionsAvailable", test->versions_available);
   _check_versions_property(bus, "VersionsAvailableLater",
                            test->versions_available_later);

   au_tests_stop_daemon_service(daemon_proc);
}

static void
test_query_updates(Fixture *f, gconstpointer context)
{
   gsize i;
   g_autoptr(GDBusConnection) bus = NULL;

   bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);

   _skip_if_daemon_is_running(bus, NULL);

   for (i = 0; i < G_N_ELEMENTS(updates_test); i++)
      _query_for_updates(f, bus, &updates_test[i]);
}

static AtomupdProperties *
_get_atomupd_properties(GDBusConnection *bus)
{
   g_autoptr(GVariantIter) available_iter = NULL;
   g_autoptr(GVariantIter) available_later_iter = NULL;
   g_auto(GVariantDict) dict = { { { 0 } } };
   g_autoptr(GVariant) reply = NULL;
   g_autoptr(GVariant) properties = NULL;
   g_autoptr(AtomupdProperties) atomupd_properties = g_new0(AtomupdProperties, 1);

#define assert_variant(_variant, _type, _member)                                         \
   g_assert_true(                                                                        \
      g_variant_dict_lookup(&dict, _variant, _type, &atomupd_properties->_member))

#define assert_variant_dict(_variant, _variant_iter, _member)                            \
   g_assert_true(g_variant_dict_lookup(&dict, _variant, "a{?*}", &_variant_iter));       \
   atomupd_properties->_member = g_variant_iter_n_children(_variant_iter)

   reply = _send_properties_message(bus, "GetAll", "(s)", AU_ATOMUPD1_INTERFACE);
   g_variant_get(reply, "(@a{sv})", &properties);
   g_variant_dict_init(&dict, properties);

   assert_variant("Version", "u", version);
   assert_variant("ProgressPercentage", "d", progress_percentage);
   assert_variant("EstimatedCompletionTime", "t", estimated_completion_time);
   assert_variant("UpdateStatus", "u", status);
   assert_variant("UpdateVersion", "s", update_version);
   assert_variant("Variant", "s", variant);
   assert_variant("FailureCode", "s", failure_code);
   assert_variant("FailureMessage", "s", failure_message);
   assert_variant("CurrentVersion", "s", current_version);
   assert_variant("KnownVariants", "^as", known_variants);

   assert_variant_dict("VersionsAvailable", available_iter, versions_available_n);
   assert_variant_dict("VersionsAvailableLater", available_later_iter,
                       versions_available_later_n);

   return g_steal_pointer(&atomupd_properties);
}

typedef struct {
   const gchar *config_name;
   const gchar *variants[10];
} PropertiesTest;

static const PropertiesTest properties_test[] = {
   {
      .config_name = NULL, /* Configuration file missing */
      .variants = { NULL },
   },

   {
      .config_name = "client.conf",
      .variants = { "rel", "rc", "beta", "bc", "main", NULL },
   },

   {
      .config_name = "client_empty_variants.conf",
      .variants = { NULL },
   },

   {
      .config_name = "client_no_variants.conf", /* "Variants" missing from the config */
      .variants = { NULL },
   },

   {
      .config_name = "client_one_variant.conf",
      .variants = { "rel", NULL },
   },

   {
      .config_name = "client_invalid_variant.conf", /* The invalid variants are skipped */
      .variants = { "rel", "Anoth3r-one", "valid", NULL },
   },

   {
      .config_name =
         "client_two_variants.conf", /* "Variants" list ending with a semicolon */
      .variants = { "rel", "beta", NULL },
   },
};

static void
_check_default_properties(Fixture *f, GDBusConnection *bus, const PropertiesTest *test)
{
   g_autoptr(GSubprocess) daemon_proc = NULL;
   g_autofree gchar *config_path = NULL;
   g_autoptr(AtomupdProperties) atomupd_properties = NULL;

   if (test->config_name != NULL)
      config_path = g_build_filename(f->srcdir, "data", test->config_name, NULL);

   daemon_proc =
      au_tests_start_daemon_service(bus, f->manifest_path, config_path, f->test_envp);

   atomupd_properties = _get_atomupd_properties(bus);
   /* The version of this interface is the number 1 */
   g_assert_cmpuint(atomupd_properties->version, ==, 1);
   g_assert_true(atomupd_properties->progress_percentage == 0);
   g_assert_cmpuint(atomupd_properties->estimated_completion_time, ==, 0);
   g_assert_cmpuint(atomupd_properties->status, ==, AU_UPDATE_STATUS_IDLE);
   g_assert_cmpstr(atomupd_properties->update_version, ==, "");
   /* Variant parsed from "manifest.json" */
   g_assert_cmpstr(atomupd_properties->variant, ==, "steamdeck");
   g_assert_cmpstr(atomupd_properties->failure_code, ==, "");
   g_assert_cmpstr(atomupd_properties->failure_message, ==, "");
   g_assert_cmpuint(atomupd_properties->versions_available_n, ==, 0);
   g_assert_cmpuint(atomupd_properties->versions_available_later_n, ==, 0);
   /* Version buildid parsed from "manifest.json" */
   g_assert_cmpstr(atomupd_properties->current_version, ==, "20220205.2");
   g_assert_cmpstrv(atomupd_properties->known_variants, test->variants);

   au_tests_stop_daemon_service(daemon_proc);
}

static void
test_default_properties(Fixture *f, gconstpointer context)
{
   gsize i;
   g_autoptr(GDBusConnection) bus = NULL;

   bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);

   _skip_if_daemon_is_running(bus, NULL);

   for (i = 0; i < G_N_ELEMENTS(properties_test); i++)
      _check_default_properties(f, bus, &properties_test[i]);
}

static void
_check_message_reply(GDBusConnection *bus,
                     const gchar *method,
                     const gchar *message_type,
                     const gchar *message_content,
                     const gchar *expected_reply)
{
   g_autoptr(GVariant) reply = NULL;
   g_autofree gchar *reply_str = NULL;

   reply = _send_atomupd_message(bus, method, message_type, message_content);
   g_variant_get(reply, "(s)", &reply_str);

   g_assert_cmpstr(reply_str, ==, expected_reply);
}

static void
test_unexpected_methods(Fixture *f, gconstpointer context)
{
   g_autoptr(GSubprocess) daemon_proc = NULL;
   g_autoptr(GDBusConnection) bus = NULL;

   bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);

   _skip_if_daemon_is_running(bus, NULL);

   daemon_proc =
      au_tests_start_daemon_service(bus, f->manifest_path, f->conf_path, f->test_envp);

   _check_message_reply(
      bus, "StartUpdate", "(s)", "20220120.1",
      "It is not possible to start an update before calling \"CheckForUpdates\"");
   _check_message_reply(bus, "StartUpdate", "(s)", "",
                        "The provided Buildid is either NULL or empty");
   _check_message_reply(bus, "StartUpdate", "(s)", "2023",
                        "Buildid '2023' doesn't follow the expected YYYYMMDD[.N] format");
   _check_message_reply(bus, "PauseUpdate", NULL, NULL,
                        "There isn't an update in progress that can be paused");
   _check_message_reply(bus, "ResumeUpdate", NULL, NULL,
                        "There isn't a paused update that can be resumed");
   _check_message_reply(bus, "CancelUpdate", NULL, NULL,
                        "There isn't an update in progress that can be cancelled");

   au_tests_stop_daemon_service(daemon_proc);
}

static void
test_start_pause_stop_update(Fixture *f, gconstpointer context)
{
   g_autoptr(GSubprocess) daemon_proc = NULL;
   g_autoptr(GSubprocess) rauc_proc = NULL;
   AuUpdateStatus status;
   g_autofree gchar *update_file_path = NULL;
   g_autoptr(GVariant) reply = NULL;
   g_autoptr(GDBusConnection) bus = NULL;
   g_autoptr(AtomupdProperties) atomupd_properties = NULL;
   g_autoptr(GDateTime) time_now = NULL;

   bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);

   _skip_if_daemon_is_running(bus, NULL);

   update_file_path = g_build_filename(f->srcdir, "data", "update_one_minor.json", NULL);
   f->test_envp =
      g_environ_setenv(f->test_envp, "G_TEST_UPDATE_JSON", update_file_path, TRUE);

   rauc_proc = au_tests_launch_rauc_service(f->rauc_pid_path);

   daemon_proc =
      au_tests_start_daemon_service(bus, f->manifest_path, f->conf_path, f->test_envp);

   _call_check_for_updates(bus, NULL, NULL);

   /* Restart the service. When starting an update we expect that it shouldn't
    * complain that we didn't check for updates, because we already did. */
   au_tests_stop_daemon_service(daemon_proc);
   g_clear_object(&daemon_proc);
   daemon_proc =
      au_tests_start_daemon_service(bus, f->manifest_path, f->conf_path, f->test_envp);

   /* Assert that restarting the daemon successfully killed the old rauc service */
   g_assert_true(g_subprocess_get_if_exited(rauc_proc));

   g_clear_object(&rauc_proc);
   rauc_proc = au_tests_launch_rauc_service(f->rauc_pid_path);

   g_debug("Starting an update that is expected to complete in 1.5 seconds");
   _send_atomupd_message_with_null_reply(bus, "StartUpdate", "(s)", MOCK_SUCCESS);

   /* The update is expected to complete in 1.5 seconds. Wait for 2x as much because
    * there might be a slight delay before the mock process actually receives this
    * D-Bus message and starts the update. There is no need to wait longer with
    * Valgrind because we are just sending a D-Bus message to a process that is
    * already up and running. */
   g_usleep(3 * G_USEC_PER_SEC);

   reply = _get_atomupd_property(bus, "UpdateStatus");
   g_variant_get(reply, "u", &status);
   g_assert_cmpuint(status, ==, AU_UPDATE_STATUS_SUCCESSFUL);

   /* With MOCK_INFINITE we simulate an update that is in progress.
    * To make it more predictable, it will always print a progress of
    * "16.08% 06m35s" until we cancel it with a SIGTERM. */
   g_debug("Starting infinite update");
   _send_atomupd_message_with_null_reply(bus, "StartUpdate", "(s)", MOCK_INFINITE);

   g_usleep(2 * default_wait);
   time_now = g_date_time_new_now_utc();
   atomupd_properties = _get_atomupd_properties(bus);
   g_assert_true(atomupd_properties->progress_percentage == 16.08);
   g_assert_cmpuint(atomupd_properties->estimated_completion_time, >,
                    g_date_time_to_unix(time_now));
   g_assert_cmpuint(atomupd_properties->status, ==, AU_UPDATE_STATUS_IN_PROGRESS);
   g_assert_cmpstr(atomupd_properties->update_version, ==, MOCK_INFINITE);
   g_clear_pointer(&atomupd_properties, atomupd_properties_free);

   _send_atomupd_message_with_null_reply(bus, "PauseUpdate", NULL, NULL);
   atomupd_properties = _get_atomupd_properties(bus);
   g_assert_true(atomupd_properties->progress_percentage == 16.08);
   g_assert_cmpuint(atomupd_properties->status, ==, AU_UPDATE_STATUS_PAUSED);
   /* Assert that the mock rauc service has not been killed.
    * Because it is not our own child, we can't check for "WIFSTOPPED". */
   g_assert_cmpint(
      kill(g_ascii_strtoll(g_subprocess_get_identifier(rauc_proc), NULL, 10), 0), ==, 0);
   g_clear_pointer(&atomupd_properties, atomupd_properties_free);

   _send_atomupd_message_with_null_reply(bus, "ResumeUpdate", NULL, NULL);
   _send_atomupd_message_with_null_reply(bus, "CancelUpdate", NULL, NULL);
   g_usleep(2 * default_wait);
   atomupd_properties = _get_atomupd_properties(bus);
   /* When receiving SIGTERM the mock steamos-atomupd-client will print
    * "17.50% 05m50s" and then quit */
   g_assert_true(atomupd_properties->progress_percentage == 17.50);
   g_assert_cmpuint(atomupd_properties->estimated_completion_time, >,
                    g_date_time_to_unix(time_now));
   g_assert_cmpuint(atomupd_properties->status, ==, AU_UPDATE_STATUS_CANCELLED);
   g_assert_cmpstr(atomupd_properties->update_version, ==, MOCK_INFINITE);
   /* Assert that the CancelUpdate successfully killed the rauc service */
   g_assert_true(g_subprocess_get_if_exited(rauc_proc));

   au_tests_stop_daemon_service(daemon_proc);
}

static void
test_progress_default(Fixture *f, gconstpointer context)
{
   g_autoptr(GSubprocess) daemon_proc = NULL;
   gdouble progress;
   g_autofree gchar *update_file_path = NULL;
   g_autoptr(GVariant) reply = NULL;
   g_autoptr(GDBusConnection) bus = NULL;

   bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);

   _skip_if_daemon_is_running(bus, NULL);

   update_file_path = g_build_filename(f->srcdir, "data", "update_one_minor.json", NULL);
   f->test_envp =
      g_environ_setenv(f->test_envp, "G_TEST_UPDATE_JSON", update_file_path, TRUE);

   daemon_proc =
      au_tests_start_daemon_service(bus, f->manifest_path, f->conf_path, f->test_envp);

   _call_check_for_updates(bus, NULL, NULL);

   g_debug("Starting an update that is expected to complete in 1.5 seconds");
   _send_atomupd_message_with_null_reply(bus, "StartUpdate", "(s)", MOCK_SUCCESS);
   /* Wait for 2x as much to ensure it really finished */
   g_usleep(3 * G_USEC_PER_SEC);

   reply = _get_atomupd_property(bus, "ProgressPercentage");
   g_variant_get(reply, "d", &progress);
   g_assert_true(progress == 100);
   g_clear_pointer(&reply, g_variant_unref);

   /* With MOCK_STUCK we simulate an update that is stuck and never prints progress
    * updates. */
   g_debug("Starting stuck update");
   _send_atomupd_message_with_null_reply(bus, "StartUpdate", "(s)", MOCK_STUCK);
   g_usleep(default_wait);

   reply = _get_atomupd_property(bus, "ProgressPercentage");
   g_variant_get(reply, "d", &progress);
   /* When we start an update, even if RAUC didn't print any progress yet, we
    * expect the progress percentage to be set by default to zero */
   g_assert_true(progress == 0);

   _send_atomupd_message_with_null_reply(bus, "CancelUpdate", NULL, NULL);
   au_tests_stop_daemon_service(daemon_proc);
}

static void
test_multiple_method_calls(Fixture *f, gconstpointer context)
{
   g_autoptr(GSubprocess) daemon_proc = NULL;
   g_autoptr(GSubprocess) rauc_proc = NULL;
   g_autoptr(GVariant) reply = NULL;
   g_autoptr(GDBusConnection) bus = NULL;
   g_autoptr(AtomupdProperties) atomupd_properties = NULL;

   bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);

   _skip_if_daemon_is_running(bus, NULL);

   daemon_proc =
      au_tests_start_daemon_service(bus, f->manifest_path, f->conf_path, f->test_envp);

   /* Launch the RAUC service after the atomupd daemon because in its start up
    * process it will kill any eventual RAUC processes that are already running */
   rauc_proc = au_tests_launch_rauc_service(f->rauc_pid_path);

   _call_check_for_updates(bus, NULL, NULL);
   reply = _send_atomupd_message(bus, "CheckForUpdates", "(a{sv})", NULL);
   g_assert_nonnull(reply);

   _send_atomupd_message_with_null_reply(bus, "StartUpdate", "(s)", MOCK_INFINITE);
   _send_atomupd_message_with_null_reply(bus, "PauseUpdate", NULL, NULL);
   /* Pausing again should not be allowed */
   _check_message_reply(bus, "PauseUpdate", NULL, NULL,
                        "There isn't an update in progress that can be paused");
   /* It is expected to be possible to cancel a paused update */
   _send_atomupd_message_with_null_reply(bus, "CancelUpdate", NULL, NULL);
   g_usleep(2 * default_wait);
   atomupd_properties = _get_atomupd_properties(bus);
   g_assert_cmpuint(atomupd_properties->status, ==, AU_UPDATE_STATUS_CANCELLED);
   g_assert_true(g_subprocess_get_if_exited(rauc_proc));

   au_tests_stop_daemon_service(daemon_proc);
}

typedef struct {
   const gchar *file_content;
   const gchar *expected_update_version;
   AuUpdateStatus expected_status;
} RebootForUpdateTest;

static const RebootForUpdateTest reboot_for_update_test[] = {
   {
      .expected_update_version = "",
      .expected_status = AU_UPDATE_STATUS_IDLE,
   },

   {
      .file_content = "20220914.1",
      .expected_update_version = "20220914.1",
      .expected_status = AU_UPDATE_STATUS_SUCCESSFUL,
   },

   {
      .file_content = "20220911.1\n",
      .expected_update_version = "20220911.1",
      .expected_status = AU_UPDATE_STATUS_SUCCESSFUL,
   },

   {
      .file_content = "20220915.100\n\n",
      .expected_update_version = "20220915.100",
      .expected_status = AU_UPDATE_STATUS_SUCCESSFUL,
   },

   {
      .file_content = "",
      .expected_update_version = "",
      .expected_status = AU_UPDATE_STATUS_SUCCESSFUL,
   },
};

static void
test_restarted_service(Fixture *f, gconstpointer context)
{
   g_autoptr(GDBusConnection) bus = NULL;
   g_autoptr(GError) error = NULL;
   gsize i;

   bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);

   _skip_if_daemon_is_running(bus, NULL);

   for (i = 0; i < G_N_ELEMENTS(reboot_for_update_test); i++) {
      g_autoptr(GSubprocess) daemon_proc = NULL;
      gint fd;
      const RebootForUpdateTest *test = &reboot_for_update_test[i];
      g_autoptr(AtomupdProperties) atomupd_properties = NULL;
      g_autofree gchar *reboot_for_update = NULL;

      if (test->file_content != NULL) {
         fd = g_file_open_tmp("reboot_for_update-XXXXXX", &reboot_for_update, &error);
         g_assert_no_error(error);
         g_assert_cmpint(fd, !=, -1);
         close(fd);

         g_file_set_contents(reboot_for_update, test->file_content, -1, &error);
         g_assert_no_error(error);

         f->test_envp = g_environ_setenv(f->test_envp, "AU_REBOOT_FOR_UPDATE",
                                         reboot_for_update, TRUE);
      } else {
         f->test_envp =
            g_environ_setenv(f->test_envp, "AU_REBOOT_FOR_UPDATE", "/missing_file", TRUE);
      }

      daemon_proc =
         au_tests_start_daemon_service(bus, f->manifest_path, f->conf_path, f->test_envp);

      atomupd_properties = _get_atomupd_properties(bus);
      g_assert_cmpstr(atomupd_properties->update_version, ==,
                      test->expected_update_version);
      g_assert_cmpuint(atomupd_properties->status, ==, test->expected_status);

      au_tests_stop_daemon_service(daemon_proc);
      if (reboot_for_update != NULL)
         g_unlink(reboot_for_update);
   }
}

static void
test_pending_reboot_check(Fixture *f, gconstpointer context)
{
   gsize i;
   g_autoptr(GDBusConnection) bus = NULL;

   bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);

   _skip_if_daemon_is_running(bus, NULL);

   for (i = 0; i < G_N_ELEMENTS(pending_reboot_test); i++)
      _query_for_updates(f, bus, &pending_reboot_test[i]);
}

typedef struct {
   const gchar *initial_file_content; /* Initial content of the file used to store the
                                         chosen branch */
   const gchar *initial_expected_variant;
   const gchar *edited_file_content; /* Simulate a variant change as if this was done with
                                        steamos-select-branch */
   const gchar *edited_expected_variant;
   const gchar *switch_to_variant; /* Change variant with the SwitchToVariant method */
   const gchar *switch_expected_variant; /* Expected value in the branch file */
} VariantTest;

static const VariantTest variant_test[] = {
   {
      .initial_file_content = NULL, /* File missing */
      .initial_expected_variant =
         "steamdeck",                        /* Default value from the f->manifest_path */
      .switch_to_variant = "steamdeck-main", /* This should create the missing file */
      .switch_expected_variant =
         "main", /* Assume "steamdeck-main" is stored in its contracted form */
   },

   {
      .initial_file_content = "rel",
      .initial_expected_variant = "steamdeck",
      .edited_file_content = "bc",
      .edited_expected_variant = "steamdeck-bc",
      .switch_to_variant = "steamdeck",
      .switch_expected_variant = "rel",
   },

   {
      .initial_file_content = "beta\n",
      .initial_expected_variant = "steamdeck-beta",
      .edited_file_content = "rel",
      .edited_expected_variant = "steamdeck",
      .switch_to_variant = "steamdeck-beta",
      .switch_expected_variant = "beta",
   },

   {
      .initial_file_content = "steamdeck-main\n",
      .initial_expected_variant = "steamdeck-main",
      .edited_file_content = "staging",
      .edited_expected_variant = "steamdeck-staging",
      .switch_to_variant = "steamdeck-newer-future-variant",
      .switch_expected_variant = "steamdeck-newer-future-variant",
   },

   {
      .initial_file_content = "",
      .initial_expected_variant =
         "steamdeck", /* Default value from the f->manifest_path */
      .edited_file_content = "steamdeck-main",
      .edited_expected_variant = "steamdeck-main",
      .switch_to_variant = "holo-another-beta",
      .switch_expected_variant = "holo-another-beta",
   },
};

static void
test_switch_variant(Fixture *f, gconstpointer context)
{
   gulong wait = 0.3 * G_USEC_PER_SEC;

   gsize i;
   g_autoptr(GDBusConnection) bus = NULL;
   g_autoptr(GError) error = NULL;

   bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);

   _skip_if_daemon_is_running(bus, NULL);

   for (i = 0; i < G_N_ELEMENTS(variant_test); i++) {
      g_autoptr(GSubprocess) daemon_proc = NULL;
      VariantTest test = variant_test[i];
      g_autofree gchar *steamos_branch = NULL;
      int fd;
      fd = g_file_open_tmp("steamos-branch-XXXXXX", &steamos_branch, &error);
      g_assert_no_error(error);
      close(fd);
      f->test_envp =
         g_environ_setenv(f->test_envp, "AU_CHOSEN_BRANCH_FILE", steamos_branch, TRUE);

      if (test.initial_file_content != NULL) {
         g_file_set_contents(steamos_branch, test.initial_file_content, -1, &error);
         g_assert_no_error(error);

      } else {
         g_unlink(steamos_branch);
      }

      daemon_proc =
         au_tests_start_daemon_service(bus, f->manifest_path, f->conf_path, f->test_envp);

      _check_string_property(bus, "Variant", test.initial_expected_variant);

      if (test.edited_file_content != NULL) {
         g_file_set_contents(steamos_branch, test.edited_file_content, -1, &error);
         g_assert_no_error(error);

         /* Wait a few milliseconds to ensure the inotify kicked in */
         g_usleep(wait);

         _check_string_property(bus, "Variant", test.edited_expected_variant);
      }

      if (test.switch_to_variant != NULL) {
         g_autofree gchar *parsed_variant = NULL;

         _send_atomupd_message_with_null_reply(bus, "SwitchToVariant", "(s)",
                                               test.switch_to_variant);

         /* No need to wait here because we expect the new property to take immediate
          * effect */

         _check_string_property(bus, "Variant", test.switch_to_variant);

         g_file_get_contents(steamos_branch, &parsed_variant, NULL, &error);
         g_assert_no_error(error);
         g_assert_cmpstr(parsed_variant, ==, test.switch_expected_variant);
      }

      au_tests_stop_daemon_service(daemon_proc);
      g_unlink(steamos_branch);
   }
}

/*
 * When the user is not authorized by polkit we should not be able
 * to call the API methods.
 */
static void
test_unauthorized(Fixture *f, gconstpointer context)
{
   g_autoptr(GSubprocess) daemon_proc = NULL;
   g_autoptr(GDBusConnection) bus = NULL;
   const gchar *allowed[] = {};
   const gchar *expected_reply = "User is not allowed to execute this method";

   bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);

   _skip_if_daemon_is_running(bus, NULL);

   daemon_proc =
      au_tests_start_daemon_service(bus, f->manifest_path, f->conf_path, f->test_envp);

   mock_polkit_set_allowed(allowed, 0);

   _check_message_reply(bus, "CheckForUpdates", "(a{sv})", NULL, expected_reply);
   _check_message_reply(bus, "SwitchToVariant", "(s)", "rel", expected_reply);
   _check_message_reply(bus, "StartUpdate", "(s)", MOCK_SUCCESS, expected_reply);
   _check_message_reply(bus, "PauseUpdate", NULL, NULL, expected_reply);
   _check_message_reply(bus, "ResumeUpdate", NULL, NULL, expected_reply);
   _check_message_reply(bus, "CancelUpdate", NULL, NULL, expected_reply);

   au_tests_stop_daemon_service(daemon_proc);
}

int
main(int argc, char **argv)
{
   int ret;

   g_test_init(&argc, &argv, NULL);

   /* Valgrind is really slow, so we need to increase our default wait time */
   if (g_getenv("AU_TEST_VALGRIND") != NULL)
      default_wait = 4 * default_wait;

#define test_add(_name, _test)                                                           \
   g_test_add(_name, Fixture, argv[0], au_tests_setup, _test, au_tests_teardown)

   test_add("/daemon/query_updates", test_query_updates);
   test_add("/daemon/default_properties", test_default_properties);
   test_add("/daemon/unexpected_methods", test_unexpected_methods);
   test_add("/daemon/start_pause_stop_update", test_start_pause_stop_update);
   test_add("/daemon/progress_default", test_progress_default);
   test_add("/daemon/multiple_method_calls", test_multiple_method_calls);
   test_add("/daemon/restarted_service", test_restarted_service);
   test_add("/daemon/pending_reboot_check", test_pending_reboot_check);
   test_add("/daemon/switch_variant", test_switch_variant);
   test_add("/daemon/test_unauthorized", test_unauthorized);

   ret = g_test_run();
   return ret;
}
