/*
 * Copyright © 2022-2024 Collabora Ltd.
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
#include "tests-utils.h"

#define _send_atomupd_message_with_null_reply(_bus, _method, _type, _content)            \
   g_assert_null(_send_atomupd_message(_bus, _method, _type, _content))

#define _send_atomupd_message(_bus, _method, _format, ...)                               \
   send_atomupd_message(_bus, AU_ATOMUPD1_PATH, AU_ATOMUPD1_INTERFACE, _method, _format, \
                        __VA_ARGS__)

#define _send_properties_message(_bus, _method, _format, ...)                            \
   send_atomupd_message(_bus, AU_ATOMUPD1_PATH, "org.freedesktop.DBus.Properties",       \
                        _method, _format, __VA_ARGS__)

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
   gsize updates_available_n;
   gsize updates_available_later_n;
   AuUpdateStatus status;
   gchar *update_build_id;
   gchar *update_version;
   gchar *variant;
   gchar *failure_code;
   gchar *failure_message;
   gchar *current_version;
   gchar *current_build_id;
   GStrv known_variants;
   GStrv known_branches;
} AtomupdProperties;

static void
atomupd_properties_free(AtomupdProperties *atomupd_properties)
{
   g_clear_pointer(&atomupd_properties->update_build_id, g_free);
   g_clear_pointer(&atomupd_properties->update_version, g_free);
   g_clear_pointer(&atomupd_properties->variant, g_free);
   g_clear_pointer(&atomupd_properties->failure_code, g_free);
   g_clear_pointer(&atomupd_properties->failure_message, g_free);
   g_clear_pointer(&atomupd_properties->current_version, g_free);
   g_clear_pointer(&atomupd_properties->current_build_id, g_free);
   g_clear_pointer(&atomupd_properties->known_variants, g_strfreev);
   g_clear_pointer(&atomupd_properties->known_branches, g_strfreev);

   g_free(atomupd_properties);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AtomupdProperties, atomupd_properties_free)

typedef struct {
   const gchar *buildid;
   const gchar *version;
   const gchar *variant;
   guint64 estimated_size;
   const gchar *requires_buildid;
} UpdatesTest;

typedef struct {
   const gchar *update_json;
   const gchar *reboot_for_update;
   const gchar *tracked_variant;
   gboolean preferences_updated;
   UpdatesTest updates_available[3];
   UpdatesTest updates_available_later[3];
} CheckUpdatesTest;

static const UpdatesTest mock_infinite_update[] = {
   {
      .buildid = MOCK_INFINITE,
      .version = "3.6.0",
      .variant = "steamdeck",
      .estimated_size = 60112233,
   },

   {},
};

static const CheckUpdatesTest updates_test[] =
{
  {
    .update_json = "update_one_minor.json",
    .tracked_variant = "steamdeck",
    .updates_available =
    {
      {
        .buildid = "20220227.3",
        .version = "snapshot",
        .variant = "steamdeck",
        .estimated_size = 70910463,
      },
    },
  },

  {
    .update_json = "update_empty.json",
    .tracked_variant = "steamdeck",
  },

  {
    .update_json = "update_three_minors.json",
    .tracked_variant = "steamdeck",
    .updates_available =
    {
      {
        .buildid = "20211225.1",
        .version = "snapshot",
        .variant = "steamdeck",
        .estimated_size = 40310422,
      },
    },
    .updates_available_later =
    {
      {
        .buildid = "20220101.1",
        .version = "snapshot",
        .variant = "steamdeck",
        .requires_buildid = "20211225.1",
      },
      {
        .buildid = "20220227.3",
        .version = "3.4.6",
        .variant = "steamdeck",
        .estimated_size = 30410461,
        .requires_buildid = "20220101.1",
      },
    },
  },

  {
    .update_json = "update_eol_variant.json",
    /* steamdeck has been marked as EOL, we expect the client to automatically
     * switch to the suggested steamdeck-replacement */
    .tracked_variant = "steamdeck-replacement",
    /* When switching to the new variant we expect that info to be stored in the
     * preferences file as well */
    .preferences_updated = TRUE,
    .updates_available =
    {
      {
        .buildid = "20240508.1",
        .version = "3.7.1",
        .variant = "steamdeck-replacement",
        .estimated_size = 70910463,
      },
    },
  },
};

static const CheckUpdatesTest pending_reboot_test[] =
{
  {
    .update_json = "update_one_minor.json",
    .tracked_variant = "steamdeck",
    /* Pending a different ID than the proposed update */
    .reboot_for_update = "20220222.1",
    .updates_available =
    {
      {
        .buildid = "20220227.3",
        .version = "snapshot",
        .variant = "steamdeck",
        .estimated_size = 70910463,
      },
    },
  },

  {
    .update_json = "update_one_minor.json",
    .tracked_variant = "steamdeck",
    /* The single update proposed has already been applied */
    .reboot_for_update = "20220227.3",
  },

  {
    .update_json = "update_three_minors.json",
    .tracked_variant = "steamdeck",
    /* The minor update has already been applied */
    .reboot_for_update = "20211225.1",
  },

  {
    .update_json = "update_three_minors.json",
    .tracked_variant = "steamdeck",
    /* This could probably happen when a downgrade is requested.
     * In this situation the daemon shows the available updates as-is,
     * given that the "later" updates cannot be installed without
     * first fulfilling their requirements */
    .reboot_for_update = "20220101.1",
    .updates_available =
    {
      {
        .buildid = "20211225.1",
        .version = "snapshot",
        .variant = "steamdeck",
        .estimated_size = 40310422,
      },
    },
    .updates_available_later =
    {
      {
        .buildid = "20220101.1",
        .version = "snapshot",
        .variant = "steamdeck",
        .requires_buildid = "20211225.1",
      },
      {
        .buildid = "20220227.3",
        .version = "3.4.6",
        .variant = "steamdeck",
        .estimated_size = 30410461,
        .requires_buildid = "20220101.1",
      },
    },
  },
};

static void
_check_available_updates(GVariantIter *available_iter,
                         const UpdatesTest *updates_available)
{
   gchar *buildid;          /* borrowed */
   GVariant *values = NULL; /* borrowed */
   g_autoptr(GVariantType) type_string = g_variant_type_new("s");
   g_autoptr(GVariantType) type_uint64 = g_variant_type_new("t");
   g_autoptr(GVariantType) type_uint32 = g_variant_type_new("u");
   gsize i;

   for (i = 0; g_variant_iter_loop(available_iter, "{s@a{sv}}", &buildid, &values); i++) {
      g_autoptr(GVariant) version = NULL;
      g_autoptr(GVariant) variant = NULL;
      g_autoptr(GVariant) estimated_size = NULL;
      g_autoptr(GVariant)
         requires
      = NULL;
      const gchar *requires_str = NULL;
      const UpdatesTest *expected_update = &updates_available[i];

      g_assert_cmpstr(expected_update->buildid, ==, buildid);

      version = g_variant_lookup_value(values, "version", type_string);
      g_assert_cmpstr(expected_update->version, ==, g_variant_get_string(version, NULL));

      variant = g_variant_lookup_value(values, "variant", type_string);
      g_assert_cmpstr(expected_update->variant, ==, g_variant_get_string(variant, NULL));

      estimated_size = g_variant_lookup_value(values, "estimated_size", type_uint64);
      g_assert_cmpuint(expected_update->estimated_size, ==,
                       g_variant_get_uint64(estimated_size));

      requires = g_variant_lookup_value(values, "requires", type_string);
      requires_str =
         requires ==
                     NULL ? NULL : g_variant_get_string(requires, NULL);
      g_assert_cmpstr(expected_update->requires_buildid, ==, requires_str);
   }
   g_assert_cmpstr(updates_available[i].buildid, ==, NULL);
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
_check_updates_property(GDBusConnection *bus,
                        const gchar *property,
                        const UpdatesTest *updates_available)
{
   g_autoptr(GVariant) reply = NULL;
   g_autoptr(GVariantIter) updates_iter = NULL;

   reply = _get_atomupd_property(bus, property);
   g_variant_get(reply, "a{?*}", &updates_iter);

   _check_available_updates(updates_iter, updates_available);
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
                        const UpdatesTest *updates_available,
                        const UpdatesTest *updates_available_later)
{
   g_autoptr(GVariant) reply = NULL;
   g_autoptr(GVariantIter) available_iter = NULL;
   g_autoptr(GVariantIter) available_later_iter = NULL;

   g_debug("Calling the \"CheckForUpdates\" method");

   reply = _send_atomupd_message(bus, "CheckForUpdates", "(a{sv})", NULL);
   g_assert_nonnull(reply);

   g_variant_get(reply, "(a{?*}a{?*})", &available_iter, &available_later_iter);

   if (updates_available != NULL)
      _check_available_updates(available_iter, updates_available);

   if (updates_available_later != NULL)
      _check_available_updates(available_later_iter, updates_available_later);
}

static void
_query_for_updates(Fixture *f, GDBusConnection *bus, const CheckUpdatesTest *test)
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

   daemon_proc = au_tests_start_daemon_service(bus, f->manifest_path, f->conf_dir,
                                               f->test_envp, FALSE);

   _call_check_for_updates(bus, test->updates_available, test->updates_available_later);

   _check_updates_property(bus, "UpdatesAvailable", test->updates_available);
   _check_updates_property(bus, "UpdatesAvailableLater", test->updates_available_later);

   _check_string_property(bus, "Variant", test->tracked_variant);

   if (test->preferences_updated) {
      g_autoptr(GKeyFile) parsed_preferences = NULL;
      g_autofree gchar *parsed_variant = NULL;

      g_assert_true(g_file_test(f->preferences_path, G_FILE_TEST_EXISTS));

      parsed_preferences = g_key_file_new();
      g_key_file_load_from_file(parsed_preferences, f->preferences_path, G_KEY_FILE_NONE,
                                &error);
      g_assert_no_error(error);

      /* If the server informed us that the requested variant is EOL, the client should
       * update its chosen variant in the preferences file. */
      parsed_variant =
         g_key_file_get_string(parsed_preferences, "Choices", "Variant", &error);
      g_assert_no_error(error);
      g_assert_cmpstr(parsed_variant, ==, test->tracked_variant);
   }

   au_tests_stop_process(daemon_proc);
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
   assert_variant("UpdateBuildID", "s", update_build_id);
   assert_variant("UpdateVersion", "s", update_version);
   assert_variant("Variant", "s", variant);
   assert_variant("FailureCode", "s", failure_code);
   assert_variant("FailureMessage", "s", failure_message);
   assert_variant("CurrentVersion", "s", current_version);
   assert_variant("CurrentBuildID", "s", current_build_id);
   assert_variant("KnownVariants", "^as", known_variants);
   assert_variant("KnownBranches", "^as", known_branches);

   assert_variant_dict("UpdatesAvailable", available_iter, updates_available_n);
   assert_variant_dict("UpdatesAvailableLater", available_later_iter,
                       updates_available_later_n);

   return g_steal_pointer(&atomupd_properties);
}

typedef struct {
   const gchar *config_name;
   const gchar *variants[5];
   const gchar *branches[10];
   const gchar *existing_info_file_content;
   const gchar *local_server_relative_path;
   gboolean fail;
} PropertiesTest;

static const PropertiesTest properties_test[] = {
   {
      .config_name = NULL, /* Configuration file missing */
      .fail = TRUE,
   },

   {
      .config_name = "client.conf",
      .variants = { "steamdeck", NULL },
      .branches = { "stable", "rc", "beta", "bc", "main", NULL },
   },

   {
      .config_name = "client_empty_variants.conf",
      /* "steamdeck" is coming from the image manifest */
      .variants = { "steamdeck" },
      .branches = { "stable", "beta", NULL },
   },

   {
      .config_name = "client_no_variants_empty_branches.conf",
      .fail = TRUE,
   },

   {
      .config_name = "client_no_branches.conf", /* "Branches" missing */
      .fail = TRUE,
   },

   {
      .config_name = "client_invalid_variants_and_branches.conf",
      /* The invalid variants and branches are skipped */
      .variants = { "steamdeck", "Anoth3r-one", "valid", NULL },
      .branches = { "stable", "S3cret-branch-", NULL },
   },

   {
      .config_name = "client_semicolon.conf", /* The lists end with a semicolon */
      .variants = { "steamdeck", "vanilla", NULL },
      /* "stable" is coming from the image manifest */
      .branches = { "beta", "bc", "stable", NULL },
   },

   {
      .config_name = "client.conf",
      /* The list of variants and branches are from the remote-info.conf file */
      .variants = { "steamdeck", "vanilla", NULL },
      .branches = { "stable", "rc", "beta", "bc", "preview", "pc", "main", NULL },
      .local_server_relative_path = "client_meta",
   },

   {
      .config_name = "client.conf",
      .variants = { "steamdeck", NULL },
      .branches = { "stable", "rc", "beta", "bc", "main", NULL },
      /* Simulate a server that returns 404 when looking for the remote-info.conf file */
      .local_server_relative_path = "empty_meta",
   },

   {
      .config_name = "client.conf",
      /* The list of variants and branches are from the server side remote-info.conf file,
       * which is expected to replace our older local version */
      .variants = { "steamdeck", "vanilla", NULL },
      .branches = { "stable", "rc", "beta", "bc", "preview", "pc", "main", NULL },
      .local_server_relative_path = "client_meta",
      .existing_info_file_content =
         "[Server]\nVariants = steamtest\nBranches = stable;nightly",
   },

   {
      .config_name = "client.conf",
      /* We already have a remote-info.conf file, that is equal to the one on the server */
      .variants = { "steamdeck", "vanilla", NULL },
      .branches = { "stable", "rc", "beta", "bc", "preview", "pc", "main", NULL },
      .local_server_relative_path = "client_meta",
      .existing_info_file_content = "[Server]\
Variants = steamdeck;vanilla\
Branches = stable;rc;beta;bc;preview;pc;main",
   },

   {
      .config_name = "client.conf",
      /* Simulate the server being unavailable, we use the local remote-info.conf as-is.
       * "steamdeck" is coming from the image manifest */
      .variants = { "steamtest", "steamdeck", NULL },
      .branches = { "stable", "nightly", NULL },
      .existing_info_file_content =
         "[Server]\nVariants = steamtest\nBranches = stable;nightly",
   },

   {
      .config_name = "client.conf",
      .variants = { "steamdeck", "vanilla", NULL },
      .branches = { "stable", "rc", "beta", "bc", "preview", "pc", "main", NULL },
      /* If the remote-info.conf file has additional values, they should be ignored */
      .local_server_relative_path = "extra_fields_meta",
   },

   {
      .config_name = "client.conf",
      /* "steamdeck" is coming from the image manifest */
      .variants = { "steamtest", "steamdeck", NULL },
      .branches = { "stable", "rc", "beta", "bc", "main", NULL },
      /* remote-info.conf with only the list of variants */
      .existing_info_file_content = "[Server]\nVariants = steamtest",
   },

   {
      .config_name = "client.conf",
      .variants = { "steamdeck", NULL },
      /* "stable" is appended from the image manifest */
      .branches = { "daily", "nightly", "stable", NULL },
      /* remote-info.conf with only the list of branches */
      .existing_info_file_content = "[Server]\nBranches = daily;nightly;",
   },

   {
      .config_name = "client.conf",
      .variants = { "steamdeck", NULL },
      .branches = { "stable", "rc", "beta", "bc", "main", NULL },
      /* remote-info.conf has unexpected content */
      .existing_info_file_content = "[Unexpected]\nUnexpected file",
   },
};

static void
_check_default_properties(Fixture *f, GDBusConnection *bus, const PropertiesTest *test)
{
   g_autoptr(GSubprocess) daemon_proc = NULL;
   g_autoptr(GSubprocess) http_server_proc = NULL;
   g_autofree gchar *tmp_config_dir = NULL;
   g_autofree gchar *local_server_dir = NULL;
   g_autoptr(AtomupdProperties) atomupd_properties = NULL;
   g_autoptr(GError) error = NULL;

   if (test->config_name != NULL) {
      g_autofree gchar *config_path = NULL;
      g_autofree gchar *source_config_path = NULL;
      g_autoptr(GFile) source_file = NULL;
      g_autoptr(GFile) dest_file = NULL;
      tmp_config_dir = g_dir_make_tmp("atomupd-daemon-prop-XXXXXX", &error);
      g_assert_no_error(error);
      config_path = g_build_filename(tmp_config_dir, "client.conf", NULL);
      source_config_path = g_build_filename(f->srcdir, "data", test->config_name, NULL);

      source_file = g_file_new_for_path(source_config_path);
      dest_file = g_file_new_for_path(config_path);

      g_file_copy(source_file, dest_file, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL,
                  &error);
      g_assert_no_error(error);
   }

   if (test->fail) {
      au_tests_start_daemon_service(bus, f->manifest_path, tmp_config_dir, f->test_envp,
                                    TRUE);
      return;
   }

   if (test->existing_info_file_content != NULL) {
      g_file_set_contents(f->remote_info_path, test->existing_info_file_content, -1,
                          &error);
      g_assert_no_error(error);
   } else {
      /* Cleanup any eventual remote info file from previous executions */
      g_unlink(f->remote_info_path);
   }

   if (test->local_server_relative_path != NULL) {
      local_server_dir =
         g_build_filename(f->srcdir, "data", test->local_server_relative_path, NULL);
      http_server_proc = au_tests_start_local_http_server(local_server_dir);
   }

   daemon_proc = au_tests_start_daemon_service(bus, f->manifest_path, tmp_config_dir,
                                               f->test_envp, FALSE);

   atomupd_properties = _get_atomupd_properties(bus);
   g_assert_cmpuint(atomupd_properties->version, ==, ATOMUPD_VERSION);
   g_assert_true(atomupd_properties->progress_percentage == 0);
   g_assert_cmpuint(atomupd_properties->estimated_completion_time, ==, 0);
   g_assert_cmpuint(atomupd_properties->status, ==, AU_UPDATE_STATUS_IDLE);
   g_assert_cmpstr(atomupd_properties->update_build_id, ==, "");
   g_assert_cmpstr(atomupd_properties->update_version, ==, "");
   /* Variant parsed from "manifest.json" */
   g_assert_cmpstr(atomupd_properties->variant, ==, "steamdeck");
   g_assert_cmpstr(atomupd_properties->failure_code, ==, "");
   g_assert_cmpstr(atomupd_properties->failure_message, ==, "");
   g_assert_cmpuint(atomupd_properties->updates_available_n, ==, 0);
   g_assert_cmpuint(atomupd_properties->updates_available_later_n, ==, 0);
   /* Version buildid parsed from "manifest.json" */
   g_assert_cmpstr(atomupd_properties->current_build_id, ==, "20220205.2");
   g_assert_cmpstr(atomupd_properties->current_version, ==, "snapshot");
   g_assert_cmpstrv(atomupd_properties->known_variants, test->variants);
   g_assert_cmpstrv(atomupd_properties->known_branches, test->branches);

   au_tests_stop_process(daemon_proc);

   if (test->local_server_relative_path != NULL)
      au_tests_stop_process(http_server_proc);

   if (!rm_rf(tmp_config_dir))
      g_debug("Unable to remove temp directory: %s", tmp_config_dir);
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
test_dev_config(Fixture *f, gconstpointer context)
{
   g_autoptr(GSubprocess) daemon_proc = NULL;
   g_autoptr(GSubprocess) http_server_proc = NULL;
   g_autoptr(GDBusConnection) bus = NULL;
   g_autofree gchar *tmp_config_dir = NULL;
   g_autofree gchar *config_path = NULL;
   g_autofree gchar *config_dev_path = NULL;
   g_autofree gchar *local_server_dir = NULL;
   const gchar *client_variants[] = { "steamdeck", NULL };
   const gchar *client_branches[] = { "stable", "rc", "beta", "bc", "main", NULL };
   const gchar *client_dev_variants[] = { "steamdeck", "vanilla", NULL };
   /* Dev branches, including "stable" from the image manifest */
   const gchar *client_dev_branches[] = { "beta", "bc", "stable", NULL };
   const gchar *client_remote_variants[] = { "steamdeck", "vanilla", NULL };
   const gchar *client_remote_branches[] = { "stable",  "rc", "beta", "bc",
                                             "preview", "pc", "main", NULL };
   g_autoptr(AtomupdProperties) atomupd_properties = NULL;
   g_autoptr(GError) error = NULL;

   bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);

   _skip_if_daemon_is_running(bus, NULL);

   tmp_config_dir = g_dir_make_tmp("atomupd-daemon-prop-XXXXXX", &error);
   g_assert_no_error(error);
   config_path = g_build_filename(tmp_config_dir, "client.conf", NULL);
   config_dev_path = g_build_filename(tmp_config_dir, "client-dev.conf", NULL);

   /* Fill the client.conf file */
   {
      g_autofree gchar *source_config_path = NULL;
      g_autoptr(GFile) source_file = NULL;
      g_autoptr(GFile) dest_file = NULL;

      source_config_path = g_build_filename(f->srcdir, "data", "client.conf", NULL);
      source_file = g_file_new_for_path(source_config_path);
      dest_file = g_file_new_for_path(config_path);
      g_file_copy(source_file, dest_file, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL,
                  &error);
      g_assert_no_error(error);
   }

   daemon_proc = au_tests_start_daemon_service(bus, f->manifest_path, tmp_config_dir,
                                               f->test_envp, FALSE);

   atomupd_properties = _get_atomupd_properties(bus);
   /* We only have the client.conf file */
   g_assert_cmpstrv(atomupd_properties->known_variants, client_variants);
   g_assert_cmpstrv(atomupd_properties->known_branches, client_branches);

   /* Fill the client-dev.conf file */
   {
      g_autofree gchar *source_config_path = NULL;
      g_autoptr(GFile) source_file = NULL;
      g_autoptr(GFile) dest_file = NULL;

      source_config_path =
         g_build_filename(f->srcdir, "data", "client_semicolon.conf", NULL);
      source_file = g_file_new_for_path(source_config_path);
      dest_file = g_file_new_for_path(config_dev_path);
      g_file_copy(source_file, dest_file, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL,
                  &error);
      g_assert_no_error(error);
   }

   g_file_set_contents(f->remote_info_path,
                       "[Server]\nVariants = steamtest\nBranches = stable;nightly", -1,
                       &error);

   g_clear_pointer(&atomupd_properties, atomupd_properties_free);
   atomupd_properties = _get_atomupd_properties(bus);
   /* The daemon was already running when we created the client dev file.
    * This means that it should still point to the canonical client.conf */
   g_assert_cmpstrv(atomupd_properties->known_variants, client_variants);
   g_assert_cmpstrv(atomupd_properties->known_branches, client_branches);

   /* Reload the configuration. This time it should notice the new client-dev file */
   _send_atomupd_message_with_null_reply(bus, "ReloadConfiguration", "(a{sv})", NULL);

   g_clear_pointer(&atomupd_properties, atomupd_properties_free);
   atomupd_properties = _get_atomupd_properties(bus);
   /* We expect the dev file to take precedence here.
    * Even if we have a local remote-info.conf file, it should be skipped because we are
    * using a -dev config file. */
   g_assert_cmpstrv(atomupd_properties->known_variants, client_dev_variants);
   g_assert_cmpstrv(atomupd_properties->known_branches, client_dev_branches);

   au_tests_stop_process(daemon_proc);
   g_clear_pointer(&atomupd_properties, atomupd_properties_free);
   g_clear_object(&daemon_proc);
   g_unlink(f->remote_info_path);

   local_server_dir = g_build_filename(f->srcdir, "data", "client_meta", NULL);
   http_server_proc = au_tests_start_local_http_server(local_server_dir);

   daemon_proc = au_tests_start_daemon_service(bus, f->manifest_path, tmp_config_dir,
                                               f->test_envp, FALSE);

   atomupd_properties = _get_atomupd_properties(bus);
   g_assert_cmpstrv(atomupd_properties->known_variants, client_dev_variants);
   g_assert_cmpstrv(atomupd_properties->known_branches, client_dev_branches);
   /* We expect to have downloaded the remote-info.conf file, even if we are not currently
    * using it due to the -dev config taking precedence. */
   g_assert_true(g_file_test(f->remote_info_path, G_FILE_TEST_EXISTS));

   au_tests_stop_process(daemon_proc);
   g_clear_pointer(&atomupd_properties, atomupd_properties_free);
   g_clear_object(&daemon_proc);

   /* Create an invalid client-dev.conf file */
   {
      g_autofree gchar *source_config_path = NULL;
      g_autoptr(GFile) source_file = NULL;
      g_autoptr(GFile) dest_file = NULL;

      source_config_path =
         g_build_filename(f->srcdir, "data", "client_no_branches.conf", NULL);
      source_file = g_file_new_for_path(source_config_path);
      dest_file = g_file_new_for_path(config_dev_path);
      g_file_copy(source_file, dest_file, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL,
                  &error);
      g_assert_no_error(error);
   }

   daemon_proc = au_tests_start_daemon_service(bus, f->manifest_path, tmp_config_dir,
                                               f->test_envp, FALSE);

   atomupd_properties = _get_atomupd_properties(bus);
   /* We expect the canonical file plus the remote-info to be picked up as a fallback */
   g_assert_cmpstrv(atomupd_properties->known_variants, client_remote_variants);
   g_assert_cmpstrv(atomupd_properties->known_branches, client_remote_branches);

   /* Reload, we should still use the canonical file plus the remote-info */
   _send_atomupd_message_with_null_reply(bus, "ReloadConfiguration", "(a{sv})", NULL);

   g_clear_pointer(&atomupd_properties, atomupd_properties_free);
   atomupd_properties = _get_atomupd_properties(bus);
   g_assert_cmpstrv(atomupd_properties->known_variants, client_remote_variants);
   g_assert_cmpstrv(atomupd_properties->known_branches, client_remote_branches);

   au_tests_stop_process(daemon_proc);
   au_tests_stop_process(http_server_proc);

   if (!rm_rf(tmp_config_dir))
      g_debug("Unable to remove temp directory: %s", tmp_config_dir);
}

static void
test_remote_config(Fixture *f, gconstpointer context)
{
   g_autoptr(GSubprocess) daemon_proc = NULL;
   g_autoptr(GSubprocess) http_server_proc = NULL;
   g_autoptr(GDBusConnection) bus = NULL;
   g_autofree gchar *tmp_config_dir = NULL;
   g_autofree gchar *config_path = NULL;
   g_autofree gchar *local_server_dir = NULL;
   const gchar *client_variants[] = { "steamdeck", NULL };
   const gchar *client_branches[] = { "stable", "rc", "beta", "bc", "main", NULL };
   const gchar *client_remote_variants[] = { "steamdeck", "vanilla", NULL };
   const gchar *client_remote_branches[] = { "stable",  "rc", "beta", "bc",
                                             "preview", "pc", "main", NULL };
   g_autoptr(AtomupdProperties) atomupd_properties = NULL;
   g_autoptr(GError) error = NULL;

   bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);

   _skip_if_daemon_is_running(bus, NULL);

   tmp_config_dir = g_dir_make_tmp("atomupd-daemon-prop-XXXXXX", &error);
   g_assert_no_error(error);
   config_path = g_build_filename(tmp_config_dir, "client.conf", NULL);

   /* Fill the client.conf file */
   {
      g_autofree gchar *source_config_path = NULL;
      g_autoptr(GFile) source_file = NULL;
      g_autoptr(GFile) dest_file = NULL;

      source_config_path = g_build_filename(f->srcdir, "data", "client.conf", NULL);
      source_file = g_file_new_for_path(source_config_path);
      dest_file = g_file_new_for_path(config_path);
      g_file_copy(source_file, dest_file, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL,
                  &error);
      g_assert_no_error(error);
   }

   daemon_proc = au_tests_start_daemon_service(bus, f->manifest_path, tmp_config_dir,
                                               f->test_envp, FALSE);

   /* We didn't start the http server, so we don't expect to have the remote-info file */
   g_assert_false(g_file_test(f->remote_info_path, G_FILE_TEST_EXISTS));

   _call_check_for_updates(bus, NULL, NULL);
   g_assert_false(g_file_test(f->remote_info_path, G_FILE_TEST_EXISTS));

   local_server_dir = g_build_filename(f->srcdir, "data", "client_meta", NULL);
   http_server_proc = au_tests_start_local_http_server(local_server_dir);

   /* This time we expect the remote-info file to be downloaded */
   _call_check_for_updates(bus, NULL, NULL);
   g_assert_true(g_file_test(f->remote_info_path, G_FILE_TEST_EXISTS));

   atomupd_properties = _get_atomupd_properties(bus);
   /* We expect the new remote-info to be loaded */
   g_assert_cmpstrv(atomupd_properties->known_variants, client_remote_variants);
   g_assert_cmpstrv(atomupd_properties->known_branches, client_remote_branches);

   g_file_set_contents(f->remote_info_path, "pre-existing file", -1, &error);
   g_assert_no_error(error);
   _send_atomupd_message_with_null_reply(bus, "ReloadConfiguration", "(a{sv})", NULL);

   _call_check_for_updates(bus, NULL, NULL);
   g_clear_pointer(&atomupd_properties, atomupd_properties_free);
   atomupd_properties = _get_atomupd_properties(bus);
   /* When calling CheckForUpdates, we already had a remote-info.conf file locally.
    * For this reason we don't expect atomupd to re-download it again. */
   g_assert_cmpstrv(atomupd_properties->known_variants, client_variants);
   g_assert_cmpstrv(atomupd_properties->known_branches, client_branches);

   au_tests_stop_process(daemon_proc);
   au_tests_stop_process(http_server_proc);

   if (!rm_rf(tmp_config_dir))
      g_debug("Unable to remove temp directory: %s", tmp_config_dir);
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
_check_message_reply_prefix(GDBusConnection *bus,
                            const gchar *method,
                            const gchar *message_type,
                            const gchar *message_content,
                            const gchar *expected_reply_prefix)
{
   g_autoptr(GVariant) reply = NULL;
   g_autofree gchar *reply_str = NULL;

   reply = _send_atomupd_message(bus, method, message_type, message_content);
   g_variant_get(reply, "(s)", &reply_str);

   g_assert_true(g_str_has_prefix(reply_str, expected_reply_prefix));
}

static void
test_unexpected_methods(Fixture *f, gconstpointer context)
{
   g_autoptr(GSubprocess) daemon_proc = NULL;
   g_autoptr(GDBusConnection) bus = NULL;

   bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);

   _skip_if_daemon_is_running(bus, NULL);

   daemon_proc = au_tests_start_daemon_service(bus, f->manifest_path, f->conf_dir,
                                               f->test_envp, FALSE);

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

   au_tests_stop_process(daemon_proc);
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

   update_file_path =
      g_build_filename(f->srcdir, "data", "update_mock_infinite.json", NULL);
   f->test_envp =
      g_environ_setenv(f->test_envp, "G_TEST_UPDATE_JSON", update_file_path, TRUE);

   rauc_proc = au_tests_launch_rauc_service(f->rauc_pid_path);

   daemon_proc = au_tests_start_daemon_service(bus, f->manifest_path, f->conf_dir,
                                               f->test_envp, FALSE);

   _call_check_for_updates(bus, NULL, NULL);

   /* Restart the service. When starting an update we expect that it shouldn't
    * complain that we didn't check for updates, because we already did. */
   au_tests_stop_process(daemon_proc);
   g_clear_object(&daemon_proc);
   daemon_proc = au_tests_start_daemon_service(bus, f->manifest_path, f->conf_dir,
                                               f->test_envp, FALSE);

   /* Assert that restarting the daemon successfully killed the old rauc service */
   g_assert_true(g_subprocess_get_if_exited(rauc_proc));

   g_clear_object(&rauc_proc);
   rauc_proc = au_tests_launch_rauc_service(f->rauc_pid_path);

   /* Assert that the daemon successfully loaded the previous state of available updates */
   _check_updates_property(bus, "UpdatesAvailable", mock_infinite_update);

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
   g_assert_cmpstr(atomupd_properties->update_build_id, ==, MOCK_INFINITE);
   g_assert_cmpstr(atomupd_properties->update_version, ==, mock_infinite_update->version);
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
   g_assert_cmpstr(atomupd_properties->update_build_id, ==, MOCK_INFINITE);
   g_assert_cmpstr(atomupd_properties->update_version, ==, mock_infinite_update->version);
   /* Assert that the CancelUpdate successfully killed the rauc service */
   g_assert_true(g_subprocess_get_if_exited(rauc_proc));

   au_tests_stop_process(daemon_proc);
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

   daemon_proc = au_tests_start_daemon_service(bus, f->manifest_path, f->conf_dir,
                                               f->test_envp, FALSE);

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
   au_tests_stop_process(daemon_proc);
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

   daemon_proc = au_tests_start_daemon_service(bus, f->manifest_path, f->conf_dir,
                                               f->test_envp, FALSE);

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

   au_tests_stop_process(daemon_proc);
}

typedef struct {
   const gchar *file_content;
   const gchar *expected_update_build_id;
   const gchar *expected_update_version;
   AuUpdateStatus expected_status;
} RebootForUpdateTest;

static const RebootForUpdateTest reboot_for_update_test[] = {
   {
      .expected_update_build_id = "",
      .expected_update_version = "",
      .expected_status = AU_UPDATE_STATUS_IDLE,
   },

   {
      .file_content = "20220914.1",
      .expected_update_build_id = "20220914.1",
      .expected_update_version = "",
      .expected_status = AU_UPDATE_STATUS_SUCCESSFUL,
   },

   {
      .file_content = "20220911.1\n",
      .expected_update_build_id = "20220911.1",
      .expected_update_version = "",
      .expected_status = AU_UPDATE_STATUS_SUCCESSFUL,
   },

   {
      .file_content = "20220915.100\n\n",
      .expected_update_build_id = "20220915.100",
      .expected_update_version = "",
      .expected_status = AU_UPDATE_STATUS_SUCCESSFUL,
   },

   {
      .file_content = "20230929.101-3.6.1",
      .expected_update_build_id = "20230929.101",
      .expected_update_version = "3.6.1",
      .expected_status = AU_UPDATE_STATUS_SUCCESSFUL,
   },

   {
      .file_content = "20230929.101-3.6.2 \n\n",
      .expected_update_build_id = "20230929.101",
      .expected_update_version = "3.6.2",
      .expected_status = AU_UPDATE_STATUS_SUCCESSFUL,
   },

   {
      .file_content = "20230929.101-3.6.2\n",
      .expected_update_build_id = "20230929.101",
      .expected_update_version = "3.6.2",
      .expected_status = AU_UPDATE_STATUS_SUCCESSFUL,
   },

   {
      .file_content = "\n",
      .expected_update_build_id = "",
      .expected_update_version = "",
      .expected_status = AU_UPDATE_STATUS_SUCCESSFUL,
   },

   {
      .file_content = "",
      .expected_update_build_id = "",
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

      daemon_proc = au_tests_start_daemon_service(bus, f->manifest_path, f->conf_dir,
                                                  f->test_envp, FALSE);

      atomupd_properties = _get_atomupd_properties(bus);
      g_assert_cmpstr(atomupd_properties->update_build_id, ==,
                      test->expected_update_build_id);
      g_assert_cmpstr(atomupd_properties->update_version, ==,
                      test->expected_update_version);
      g_assert_cmpuint(atomupd_properties->status, ==, test->expected_status);

      au_tests_stop_process(daemon_proc);
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
   const gchar *variant;
   const gchar *branch;
} PrefsEntries;

typedef struct {
   const gchar *custom_manifest; /* Use a custom manifest instead of f->manifest_path */
   const gchar *legacy_conf_file_content; /* Content of the legacy "steamos-branch" file */
   gboolean unreadable_legacy_conf_file;
   PrefsEntries
      initial_file; /* Initial variant and branch values in the preferences file */
   gboolean preferences_file_missing;
   PrefsEntries initial_expected;
   const gchar *switch_to_variant; /* Change variant with the SwitchToVariant method */
   const gchar *switch_to_branch;  /* Change branch with the SwitchToBranch method */
   PrefsEntries switch_expected;   /* Expected values in the preferences file */
} PreferencesTest;

static const PreferencesTest preferences_test[] = {
   {
      .preferences_file_missing = TRUE,
      .initial_expected = {
         /* Default values from the f->manifest_path */
         .variant = "steamdeck",
         .branch = "stable",
      },
      .switch_expected = {
         .variant = "steamdeck",
         .branch = "stable",
      },
   },

   {
      .preferences_file_missing = TRUE,
      .custom_manifest = "manifest_steamtest.json",
      .initial_expected = {
         /* Default values from manifest_steamtest.json */
         .variant = "steamtest",
         .branch = "beta",
      },
      .switch_expected = {
         .variant = "steamtest",
         .branch = "beta",
      },
   },

   {
      .preferences_file_missing = TRUE,
      .custom_manifest = "manifest_steamtest_missing_branch.json",
      .initial_expected = {
         /* Expecting stable as the hardcoded fallback value */
         .variant = "steamtest",
         .branch = "stable",
      },
      .switch_expected = {
         .variant = "steamtest",
         .branch = "stable",
      },
   },

   {
      /* Manifest that is missing the necessary "variant" field */
      .custom_manifest = "manifest_invalid.json",
      .initial_file = {
         .variant = "steamdeck",
         .branch = "main",
      },
      .initial_expected = {
         /* Preferences takes precedence, so the borked manifest shouldn't be an issue */
         .variant = "steamdeck",
         .branch = "main",
      },
      .switch_to_branch = "stable",
      .switch_expected = {
         .variant = "steamdeck",
         .branch = "stable",
      },
   },

   {
      .legacy_conf_file_content = "beta\n",
      .preferences_file_missing = TRUE,
      .initial_expected = {
         .variant = "steamdeck",
         .branch = "beta",
      },
      .switch_expected = {
         .variant = "steamdeck",
         .branch = "beta",
      },
   },

   {
      .legacy_conf_file_content = "steamdeck-main\n",
      .preferences_file_missing = TRUE,
      .initial_expected = {
         .variant = "steamdeck",
         .branch = "main",
      },
      .switch_to_variant = "vanilla",
      .switch_expected = {
         .variant = "vanilla",
         .branch = "main",
      },
   },

   {
      .legacy_conf_file_content = "rel",
      /* We have both the new preferences file and the legacy one.
       * In this situation we expect the legacy file to still take precedence */
      .initial_file = {
         .variant = "steamdeck",
         .branch = "main",
      },
      .initial_expected = {
         .variant = "steamdeck",
         .branch = "stable",
      },
      .switch_to_variant = "vanilla",
      .switch_to_branch = "bc",
      .switch_expected = {
         .variant = "vanilla",
         .branch = "bc",
      },
   },

   {
      .initial_file = {
         .variant = "vanilla",
         .branch = "stable",
      },
      .initial_expected = {
         .variant = "vanilla",
         .branch = "stable",
      },
      .switch_to_variant = "steamdeck",
      .switch_to_branch = "beta",
      .switch_expected = {
         .variant = "steamdeck",
         .branch = "beta",
      },
   },

   {
      .unreadable_legacy_conf_file = TRUE,
      /* Given the unreadable legacy config file, it should fallback to the new preferences file */
      .initial_file = {
         .variant = "steamdeck",
         .branch = "main",
      },
      .initial_expected = {
         .variant = "steamdeck",
         .branch = "main",
      },
      .switch_to_variant = "vanilla",
      .switch_expected = {
         .variant = "vanilla",
         .branch = "main",
      },
   },

   {
      /* Malformed conf file */
      .legacy_conf_file_content = "steamdeck-beta\nsteamdeck-main",
      .preferences_file_missing = TRUE,
      /* It should fallback to the manifest */
      .initial_expected = {
         .variant = "steamdeck",
         .branch = "stable",
      },
      .switch_expected = {
         .variant = "steamdeck",
         .branch = "stable",
      },
   },

   {
      /* Malformed conf file */
      .legacy_conf_file_content = "\nsteamdeck-beta",
      .initial_file = {
         .variant = "steamdeck",
         .branch = "main",
      },
      .initial_expected = {
         .variant = "steamdeck",
         .branch = "main",
      },
      .switch_expected = {
         .variant = "steamdeck",
         .branch = "main",
      },
   },

   {
      /* Conf file with unexpected content */
      .legacy_conf_file_content = "unknown-beta",
      .initial_file = {
         .variant = "steamdeck",
         .branch = "main",
      },
      .initial_expected = {
         .variant = "steamdeck",
         .branch = "main",
      },
      .switch_expected = {
         .variant = "steamdeck",
         .branch = "main",
      },
   },

   {
      .unreadable_legacy_conf_file = TRUE,
      .initial_file = {
         /* Empty prefs file */
      },
      .initial_expected = {
         /* Default values from the f->manifest_path */
         .variant = "steamdeck",
         .branch = "stable",
      },
      .switch_expected = {
         .variant = "steamdeck",
         .branch = "stable",
      },
   },

   {
      .initial_file = {
         /* Prefs file missing the branch */
         .variant = "vanilla",
      },
      .initial_expected = {
         /* Default values from the f->manifest_path */
         .variant = "steamdeck",
         .branch = "stable",
      },
      .switch_expected = {
         .variant = "steamdeck",
         .branch = "stable",
      },
   },

   {
      .initial_file = {
         /* Prefs file missing the variant */
         .branch = "main",
      },
      .initial_expected = {
         /* Default values from the f->manifest_path */
         .variant = "steamdeck",
         .branch = "stable",
      },
      .switch_expected = {
         .variant = "steamdeck",
         .branch = "stable",
      },
   },
};

static void
test_preferences(Fixture *f, gconstpointer context)
{
   gsize i;
   g_autoptr(GDBusConnection) bus = NULL;
   g_autoptr(GError) error = NULL;

   bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);

   _skip_if_daemon_is_running(bus, NULL);

   for (i = 0; i < G_N_ELEMENTS(preferences_test); i++) {
      g_autoptr(GSubprocess) daemon_proc = NULL;
      PreferencesTest test = preferences_test[i];
      g_autofree gchar *legacy_steamos_branch = NULL;
      g_autofree gchar *preferences_path = NULL;
      g_autofree gchar *parsed_variant = NULL;
      g_autofree gchar *parsed_branch = NULL;
      g_autofree gchar *manifest_path = NULL;
      g_autoptr(GKeyFile) parsed_preferences = NULL;
      int fd;

      fd = g_file_open_tmp("steamos-branch-XXXXXX", &legacy_steamos_branch, &error);
      g_assert_no_error(error);
      close(fd);
      f->test_envp = g_environ_setenv(f->test_envp, "AU_CHOSEN_BRANCH_FILE",
                                      legacy_steamos_branch, TRUE);

      fd = g_file_open_tmp("preferences-XXXXXX", &preferences_path, &error);
      g_assert_no_error(error);
      close(fd);
      f->test_envp = g_environ_setenv(f->test_envp, "AU_USER_PREFERENCES_FILE",
                                      preferences_path, TRUE);

      if (test.legacy_conf_file_content != NULL) {
         g_file_set_contents(legacy_steamos_branch, test.legacy_conf_file_content, -1,
                             &error);
         g_assert_no_error(error);
      } else if (test.unreadable_legacy_conf_file) {
         int result;
         g_unlink(legacy_steamos_branch);

         /* Create a directory instead of a text file, to test the code path where the
          * path exists but we can't actually read it. Another alternative would be to
          * remove the read permission from the file, but that doesn't work in our test
          * cases if we execute them with root privileges. */
         result = g_mkdir(legacy_steamos_branch, 0775);
         g_assert_cmpint(result, ==, 0);
      } else {
         g_unlink(legacy_steamos_branch);
      }

      if (!test.preferences_file_missing) {
         g_autoptr(GKeyFile) preferences = g_key_file_new();

         if (test.initial_file.variant != NULL)
            g_key_file_set_string(preferences, "Choices", "Variant",
                                  test.initial_file.variant);

         if (test.initial_file.branch != NULL)
            g_key_file_set_string(preferences, "Choices", "Branch",
                                  test.initial_file.branch);

         g_key_file_save_to_file(preferences, preferences_path, &error);
         g_assert_no_error(error);

      } else {
         g_unlink(preferences_path);
      }

      if (test.custom_manifest)
         manifest_path = g_build_filename(f->srcdir, "data", test.custom_manifest, NULL);
      else
         manifest_path = g_strdup(f->manifest_path);

      daemon_proc = au_tests_start_daemon_service(bus, manifest_path, f->conf_dir,
                                                  f->test_envp, FALSE);

      _check_string_property(bus, "Variant", test.initial_expected.variant);
      _check_string_property(bus, "Branch", test.initial_expected.branch);

      /* The daemon should always create the preferences file */
      g_assert_true(g_file_test(preferences_path, G_FILE_TEST_EXISTS));

      if (test.switch_to_variant != NULL)
         _send_atomupd_message_with_null_reply(bus, "SwitchToVariant", "(s)",
                                               test.switch_to_variant);

      if (test.switch_to_branch != NULL)
         _send_atomupd_message_with_null_reply(bus, "SwitchToBranch", "(s)",
                                               test.switch_to_branch);

      parsed_preferences = g_key_file_new();
      g_key_file_load_from_file(parsed_preferences, preferences_path, G_KEY_FILE_NONE,
                                &error);
      g_assert_no_error(error);
      parsed_variant =
         g_key_file_get_string(parsed_preferences, "Choices", "Variant", &error);
      g_assert_no_error(error);
      parsed_branch =
         g_key_file_get_string(parsed_preferences, "Choices", "Branch", &error);
      g_assert_no_error(error);

      g_assert_cmpstr(parsed_variant, ==, test.switch_expected.variant);
      g_assert_cmpstr(parsed_branch, ==, test.switch_expected.branch);

      au_tests_stop_process(daemon_proc);

      if (test.unreadable_legacy_conf_file)
         g_rmdir(legacy_steamos_branch);
      else
         g_unlink(legacy_steamos_branch);

      g_unlink(preferences_path);
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

   daemon_proc = au_tests_start_daemon_service(bus, f->manifest_path, f->conf_dir,
                                               f->test_envp, FALSE);

   mock_polkit_set_allowed(allowed, 0);

   _check_message_reply(bus, "CheckForUpdates", "(a{sv})", NULL, expected_reply);
   _check_message_reply(bus, "SwitchToVariant", "(s)", "steamdeck", expected_reply);
   _check_message_reply(bus, "SwitchToBranch", "(s)", "stable", expected_reply);
   _check_message_reply(bus, "StartUpdate", "(s)", MOCK_SUCCESS, expected_reply);
   _check_message_reply(bus, "PauseUpdate", NULL, NULL, expected_reply);
   _check_message_reply(bus, "ResumeUpdate", NULL, NULL, expected_reply);
   _check_message_reply(bus, "CancelUpdate", NULL, NULL, expected_reply);

   au_tests_stop_process(daemon_proc);
}

typedef struct {
   const gchar *file_content;
   UpdatesTest updates_available[3];
   UpdatesTest updates_available_later[3];
} ExistingUpdatesJson;

static const ExistingUpdatesJson existing_updates_json_test[] = {
   {
      .file_content = NULL,
   },

   {
      .file_content = "",
   },

   {
      .file_content = "{}",
   },

   {
      .file_content = "{ \
  \"minor\": { \
    \"release\": \"holo\", \
    \"candidates\": [ \
      { \
        \"image\": { \
          \"product\": \"steamos\", \
          \"release\": \"holo\", \
          \"variant\": \"steamdeck\", \
          \"arch\": \"amd64\", \
          \"version\": \"3.6.0\", \
          \"buildid\": \"20300101.100\", \
          \"checkpoint\": false, \
          \"estimated_size\": 60112233 \
        }, \
        \"update_path\": \"steamdeck/20300101.100/foo-3.6.0.raucb\" \
      } \
    ] \
  } \
}",
      .updates_available =
      {
         {
            .buildid = "20300101.100",
            .version = "3.6.0",
            .variant = "steamdeck",
            .estimated_size = 60112233,
         },
      },
   },

   {
      .file_content = "{ \
  \"minor\": { \
    \"release\": \"holo\", \
    \"candidates\": [ \
      { \
        \"image\": { \
          \"product\": \"steamos\", \
          \"release\": \"holo\", \
          \"variant\": \"steamdeck\", \
          \"arch\": \"amd64\", \
          \"version\": \"snapshot\", \
          \"buildid\": \"20230810.1\", \
          \"checkpoint\": true, \
          \"estimated_size\": 4815162342 \
        }, \
        \"update_path\": \"steamdeck-20230810.1-snapshot.raucb\" \
      }, \
      { \
        \"image\": { \
          \"product\": \"steamos\", \
          \"release\": \"holo\", \
          \"variant\": \"steamdeck\", \
          \"arch\": \"amd64\", \
          \"version\": \"3.7.1\", \
          \"buildid\": \"20231120.1\" \
        }, \
        \"update_path\": \"20231120.1/steamdeck-20231120.1-3.7.1.raucb\" \
      } \
    ] \
  } \
}",
      .updates_available =
      {
         {
            .buildid = "20230810.1",
            .version = "snapshot",
            .variant = "steamdeck",
            .estimated_size = 4815162342,
         },
      },
      .updates_available_later =
      {
         {
            .buildid = "20231120.1",
            .version = "3.7.1",
            .variant = "steamdeck",
            .requires_buildid = "20230810.1"
         },
      },
   },
};

static void
test_parsing_existing_updates_json(Fixture *f, gconstpointer context)
{
   g_autoptr(GDBusConnection) bus = NULL;
   g_autoptr(GError) error = NULL;
   gsize i;

   bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);

   _skip_if_daemon_is_running(bus, NULL);

   for (i = 0; i < G_N_ELEMENTS(existing_updates_json_test); i++) {
      g_autoptr(GSubprocess) daemon_proc = NULL;
      gint fd;
      const ExistingUpdatesJson *test = &existing_updates_json_test[i];
      g_autofree gchar *atomupd_updates = NULL;

      if (test->file_content != NULL) {
         fd = g_file_open_tmp("atomupd-udpates-XXXXXX", &atomupd_updates, &error);
         g_assert_no_error(error);
         g_assert_cmpint(fd, !=, -1);
         close(fd);

         g_file_set_contents(atomupd_updates, test->file_content, -1, &error);
         g_assert_no_error(error);

         f->test_envp =
            g_environ_setenv(f->test_envp, "AU_UPDATES_JSON_FILE", atomupd_updates, TRUE);
      } else {
         f->test_envp =
            g_environ_setenv(f->test_envp, "AU_UPDATES_JSON_FILE", "/missing_file", TRUE);
      }

      daemon_proc = au_tests_start_daemon_service(bus, f->manifest_path, f->conf_dir,
                                                  f->test_envp, FALSE);

      _check_updates_property(bus, "UpdatesAvailable", test->updates_available);
      _check_updates_property(bus, "UpdatesAvailableLater",
                              test->updates_available_later);

      au_tests_stop_process(daemon_proc);
      if (atomupd_updates != NULL)
         g_unlink(atomupd_updates);
   }
}

typedef struct {
   gboolean preferences_file_missing;
   const gchar *custom_manifest; /* Use a custom manifest instead of f->manifest_path */
   PrefsEntries initial_prefs;   /* Initial variant and branch in the preferences file */
   PrefsEntries updated_prefs;   /* Values after the 4xx from the server */
   const gchar *failed_message;  /* The expected prefix of the error message returned by
                                    the daemon */
} CheckUpdates4xxTest;

static const CheckUpdates4xxTest check_updates_4xx_test[] = {
   {
      .custom_manifest = "manifest_steamdeck.json",
      .initial_prefs = {
         .variant = "steamdeck",
         .branch = "betaaa",
      },
      .updated_prefs = {
         /* Default values from manifest_steamdeck.json */
         .variant = "steamdeck",
         .branch = "stable",
      },
   },

   {
      .custom_manifest = "manifest_steamdeck.json",
      .initial_prefs = {
         .variant = "steamdeck",
         .branch = "stable",
      },
      .updated_prefs = {
         /* Default values from manifest_steamdeck.json */
         .variant = "steamdeck",
         .branch = "stable",
      },
      .failed_message = "The server query returned HTTP 4xx. We are already following the default ",
   },

   {
      .custom_manifest = "manifest_steamdeck.json",
      .initial_prefs = {
         .variant = "steamdeck",
         .branch = "main",
      },
      .updated_prefs = {
         /* Default values from manifest_steamdeck.json */
         .variant = "steamdeck",
         .branch = "stable",
      },
   },

   {
      .custom_manifest = "manifest_steamtest.json",
      .initial_prefs = {
         .variant = "customvariant",
         .branch = "main",
      },
      .updated_prefs = {
         /* Default values from manifest_steamtest.json */
         .variant = "steamtest",
         .branch = "beta",
      },
   },

   {
      .custom_manifest = "manifest_steamtest.json",
      .preferences_file_missing = TRUE,
      .initial_prefs = {
         /* Values from manifest_steamtest.json */
         .variant = "steamtest",
         .branch = "beta",
      },
      .updated_prefs = {
         /* After a 4xx we keep the same values because these are the defaults */
         .variant = "steamtest",
         .branch = "beta",
      },
      .failed_message = "The server query returned HTTP 4xx. We are already following the default ",
   },

   {
      .custom_manifest = "manifest_no_variant.json",
      .initial_prefs = {
         .variant = "steamdeck",
         .branch = "rc",
      },
      .updated_prefs = {
         /* After a 4xx we keep the same values because we can't parse the default variant from the manifest */
         .variant = "steamdeck",
         .branch = "rc",
      },
      .failed_message = "The server query returned HTTP 4xx and parsing the default variant from the image manifest failed",
   },

   {
      .custom_manifest = "manifest.json",
      .initial_prefs = {
         .variant = "steamdtest",
         .branch = "rc",
      },
      .updated_prefs = {
         /* The manifest doesn't have a branch, so we expect stable to be used as the hardcoded fallback */
         .variant = "steamdeck",
         .branch = "stable",
      },
   },
};

static void
test_query_updates_4xx(Fixture *f, gconstpointer context)
{
   gsize i;
   g_autoptr(GDBusConnection) bus = NULL;
   g_autoptr(GError) error = NULL;

   bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);

   _skip_if_daemon_is_running(bus, NULL);

   for (i = 0; i < G_N_ELEMENTS(check_updates_4xx_test); i++) {
      g_autoptr(GSubprocess) daemon_proc = NULL;
      CheckUpdates4xxTest test = check_updates_4xx_test[i];
      g_autofree gchar *legacy_steamos_branch = NULL;
      g_autofree gchar *preferences_path = NULL;
      g_autofree gchar *manifest_path = NULL;
      g_autofree gchar *expected_reply = NULL;
      int fd;

      fd = g_file_open_tmp("steamos-branch-XXXXXX", &legacy_steamos_branch, &error);
      g_assert_no_error(error);
      close(fd);
      f->test_envp = g_environ_setenv(f->test_envp, "AU_CHOSEN_BRANCH_FILE",
                                      legacy_steamos_branch, TRUE);
      g_unlink(legacy_steamos_branch);

      fd = g_file_open_tmp("preferences-XXXXXX", &preferences_path, &error);
      g_assert_no_error(error);
      close(fd);
      f->test_envp = g_environ_setenv(f->test_envp, "AU_USER_PREFERENCES_FILE",
                                      preferences_path, TRUE);

      if (!test.preferences_file_missing) {
         g_autoptr(GKeyFile) preferences = g_key_file_new();

         if (test.initial_prefs.variant != NULL)
            g_key_file_set_string(preferences, "Choices", "Variant",
                                  test.initial_prefs.variant);

         if (test.initial_prefs.branch != NULL)
            g_key_file_set_string(preferences, "Choices", "Branch",
                                  test.initial_prefs.branch);

         g_key_file_save_to_file(preferences, preferences_path, &error);
         g_assert_no_error(error);

      } else {
         g_unlink(preferences_path);
      }

      if (test.custom_manifest)
         manifest_path = g_build_filename(f->srcdir, "data", test.custom_manifest, NULL);
      else
         manifest_path = g_strdup(f->manifest_path);

      f->test_envp = g_environ_setenv(f->test_envp, "G_TEST_CLIENT_QUERY_4xx", "1", TRUE);

      daemon_proc = au_tests_start_daemon_service(bus, manifest_path, f->conf_dir,
                                                  f->test_envp, FALSE);

      _check_string_property(bus, "Variant", test.initial_prefs.variant);
      _check_string_property(bus, "Branch", test.initial_prefs.branch);

      /* The daemon should always create the preferences file */
      g_assert_true(g_file_test(preferences_path, G_FILE_TEST_EXISTS));

      if (test.failed_message != NULL)
         expected_reply = g_strdup(test.failed_message);
      else
         expected_reply =
            g_strdup_printf("The server query returned HTTP 4xx. The tracked variant and "
                            "branch have been reverted to the default values: '%s', '%s'",
                            test.updated_prefs.variant, test.updated_prefs.branch);

      _check_message_reply_prefix(bus, "CheckForUpdates", "(a{sv})", NULL,
                                  expected_reply);

      /* The tracked variant and branch should be updated after the HTTP 4xx error */
      _check_string_property(bus, "Variant", test.updated_prefs.variant);
      _check_string_property(bus, "Branch", test.updated_prefs.branch);

      au_tests_stop_process(daemon_proc);
      g_unlink(legacy_steamos_branch);
      g_unlink(preferences_path);
   }
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
   test_add("/daemon/query_updates_4xx", test_query_updates_4xx);
   test_add("/daemon/default_properties", test_default_properties);
   test_add("/daemon/dev_config", test_dev_config);
   test_add("/daemon/remote_config", test_remote_config);
   test_add("/daemon/unexpected_methods", test_unexpected_methods);
   test_add("/daemon/start_pause_stop_update", test_start_pause_stop_update);
   test_add("/daemon/progress_default", test_progress_default);
   test_add("/daemon/multiple_method_calls", test_multiple_method_calls);
   test_add("/daemon/restarted_service", test_restarted_service);
   test_add("/daemon/pending_reboot_check", test_pending_reboot_check);
   test_add("/daemon/preferences", test_preferences);
   test_add("/daemon/test_unauthorized", test_unauthorized);
   test_add("/daemon/test_parsing_existing_updates_json",
            test_parsing_existing_updates_json);

   ret = g_test_run();
   return ret;
}
