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

#define _skip_if_daemon_is_running(_bus, _error)                                         \
   if (au_tests_is_daemon_service_running(bus, _error)) {                                \
      g_test_skip("Can't run this test if another instance of the Atomupd "              \
                  "daemon service is already running");                                  \
      return;                                                                            \
   }

static gchar *
_au_execute_manager(const gchar *command,
                    const gchar *argument,
                    gboolean verbose,
                    gchar **envp,
                    GError **error)
{
   g_autofree gchar *output = NULL;
   gint wait_status = 0;

   const gchar *systemctl_argv[] = {
      "atomupd-manager",
      "--session",
      command,
      argument,
      verbose ? "--verbose" : NULL,
      NULL,
   };

   g_return_val_if_fail(error == NULL || *error == NULL, NULL);

   if (!g_spawn_sync(NULL, /* working directory */
                     (gchar **)systemctl_argv, envp, G_SPAWN_SEARCH_PATH,
                     NULL,          /* child setup */
                     NULL,          /* user data */
                     &output, NULL, /* stderr */
                     &wait_status, error)) {
      return NULL;
   }

   g_print("atomupd-manager output: %s\n", output);

   if (!g_spawn_check_wait_status(wait_status, error))
      return NULL;

   return g_steal_pointer(&output);
}

typedef struct {
   const gchar *update_json;
   const gchar *output_contains[10];
   const gchar *output_does_not_contain[10];
} CheckTest;

static const CheckTest check_test[] =
{
  {
    .update_json = "update_empty.json",
    .output_contains =
    {
      "No update available",
    },
    .output_does_not_contain =
    {
      "Updates available:",
      "Updates available later:",
    },
  },

  {
    .update_json = "update_one_minor.json",
    .output_contains =
    {
      "Updates available:",
      "20220227.3",
      "snapshot",
    },
    .output_does_not_contain =
    {
      "Updates available later:",
      "No update available",
    },
  },

  {
    .update_json = "update_three_minors.json",
    .output_contains =
    {
      "Updates available:",
      "20211225.1",
      "snapshot",
      "steamdeck",
      "40310422",
      "Updates available later:",
      "20220101.1",
      "20220227.3",
      "3.4.6",
    },
    .output_does_not_contain =
    {
      "No update available",
    },
  },
};

static void
test_check_updates(Fixture *f, gconstpointer context)
{
   g_autoptr(GDBusConnection) bus = NULL;
   g_autoptr(GError) error = NULL;
   gsize i, j;

   bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);

   _skip_if_daemon_is_running(bus, NULL);

   for (i = 0; i < G_N_ELEMENTS(check_test); i++) {
      g_autoptr(GSubprocess) daemon_proc = NULL;
      g_autofree gchar *update_file_path = NULL;
      g_autofree gchar *output = NULL;
      const CheckTest ct = check_test[i];

      update_file_path = g_build_filename(f->srcdir, "data", ct.update_json, NULL);
      f->test_envp =
         g_environ_setenv(f->test_envp, "G_TEST_UPDATE_JSON", update_file_path, TRUE);

      daemon_proc = au_tests_start_daemon_service(bus, f->manifest_path, f->conf_path,
                                                  f->test_envp, FALSE);

      output = _au_execute_manager("check", NULL, FALSE, f->test_envp, &error);
      g_assert_no_error(error);

      g_debug("%s", output);

      for (j = 0; ct.output_contains[j] != NULL; j++)
         g_assert_nonnull(strstr(output, ct.output_contains[j]));

      for (j = 0; ct.output_does_not_contain[j] != NULL; j++)
         g_assert_null(strstr(output, ct.output_does_not_contain[j]));

      au_tests_stop_daemon_service(daemon_proc);
   }
}

static void
test_multiple_method_calls(Fixture *f, gconstpointer context)
{
   g_autoptr(GSubprocess) daemon_proc = NULL;
   g_autoptr(GDBusConnection) bus = NULL;
   g_autoptr(GError) error = NULL;
   g_autofree gchar *update_file_path = NULL;

   bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);

   _skip_if_daemon_is_running(bus, NULL);

   update_file_path = g_build_filename(f->srcdir, "data", "update_one_minor.json", NULL);
   f->test_envp =
      g_environ_setenv(f->test_envp, "G_TEST_UPDATE_JSON", update_file_path, TRUE);

   daemon_proc =
      au_tests_start_daemon_service(bus, f->manifest_path, f->conf_path, f->test_envp, FALSE);

   {
      g_autofree gchar *output = NULL;
      g_autofree gchar *parsed_variant = NULL;
      g_autofree gchar *parsed_branch = NULL;
      g_autofree gchar *variants_list = NULL;
      g_autofree gchar *branches_list = NULL;
      g_autofree gchar *initial_variant = NULL;
      g_autofree gchar *initial_branch = NULL;
      g_autofree gchar *tracked_variant = NULL;
      g_autofree gchar *tracked_branch = NULL;
      g_autofree gchar *update_status = NULL;
      g_autoptr(GKeyFile) parsed_preferences = NULL;

      initial_variant =
         _au_execute_manager("tracked-variant", NULL, FALSE, f->test_envp, &error);
      g_assert_cmpstr(initial_variant, ==, "steamdeck\n");
      initial_branch =
         _au_execute_manager("tracked-branch", NULL, FALSE, f->test_envp, &error);
      g_assert_cmpstr(initial_branch, ==, "stable\n");

      output =
         _au_execute_manager("switch-variant", "vanilla", FALSE, f->test_envp, &error);
      g_assert_no_error(error);
      g_clear_pointer(&output, g_free);
      output = _au_execute_manager("switch-branch", "main", FALSE, f->test_envp, &error);
      g_assert_no_error(error);
      parsed_preferences = g_key_file_new();
      g_key_file_load_from_file(parsed_preferences, f->preferences_path, G_KEY_FILE_NONE,
                                &error);
      g_assert_no_error(error);
      parsed_variant =
         g_key_file_get_string(parsed_preferences, "Choices", "Variant", NULL);
      g_assert_cmpstr(parsed_variant, ==, "vanilla");
      parsed_branch =
         g_key_file_get_string(parsed_preferences, "Choices", "Branch", NULL);
      g_assert_cmpstr(parsed_branch, ==, "main");

      variants_list =
         _au_execute_manager("list-variants", NULL, FALSE, f->test_envp, NULL);
      g_assert_cmpstr(variants_list, ==, "steamdeck\n");
      branches_list =
         _au_execute_manager("list-branches", NULL, FALSE, f->test_envp, NULL);
      g_assert_cmpstr(branches_list, ==, "stable\nrc\nbeta\nbc\nmain\n");

      tracked_variant =
         _au_execute_manager("tracked-variant", NULL, FALSE, f->test_envp, NULL);
      g_assert_cmpstr(tracked_variant, ==, "vanilla\n");
      tracked_branch =
         _au_execute_manager("tracked-branch", NULL, FALSE, f->test_envp, NULL);
      g_assert_cmpstr(tracked_branch, ==, "main\n");

      update_status =
         _au_execute_manager("get-update-status", NULL, FALSE, f->test_envp, NULL);
      g_assert_cmpstr(update_status, ==, "idle\n");
   }

   {
      g_autofree gchar *output = NULL;
      g_autofree gchar *parsed_variant = NULL;
      g_autofree gchar *parsed_branch = NULL;
      g_autofree gchar *tracked_variant = NULL;
      g_autofree gchar *tracked_branch = NULL;
      g_autoptr(GKeyFile) parsed_preferences = NULL;

      output =
         _au_execute_manager("switch-variant", "steamdeck", FALSE, f->test_envp, &error);
      g_assert_no_error(error);
      g_clear_pointer(&output, g_free);
      output =
         _au_execute_manager("switch-branch", "stable", FALSE, f->test_envp, &error);
      g_assert_no_error(error);
      parsed_preferences = g_key_file_new();
      g_key_file_load_from_file(parsed_preferences, f->preferences_path, G_KEY_FILE_NONE,
                                &error);
      g_assert_no_error(error);
      parsed_variant =
         g_key_file_get_string(parsed_preferences, "Choices", "Variant", NULL);
      g_assert_cmpstr(parsed_variant, ==, "steamdeck");
      parsed_branch =
         g_key_file_get_string(parsed_preferences, "Choices", "Branch", NULL);
      g_assert_cmpstr(parsed_branch, ==, "stable");

      tracked_variant =
         _au_execute_manager("tracked-variant", NULL, FALSE, f->test_envp, NULL);
      g_assert_cmpstr(tracked_variant, ==, "steamdeck\n");
      tracked_branch =
         _au_execute_manager("tracked-branch", NULL, FALSE, f->test_envp, NULL);
      g_assert_cmpstr(tracked_branch, ==, "stable\n");
   }

   {
      g_autofree gchar *output = NULL;

      output = _au_execute_manager("check", NULL, FALSE, f->test_envp, &error);
      g_assert_no_error(error);
      g_assert_nonnull(strstr(output, "20220227.3"));
   }

   {
      g_autofree gchar *output = NULL;

      output = _au_execute_manager("check", "--penultimate-update", FALSE, f->test_envp,
                                   &error);
      g_assert_no_error(error);
      g_assert_nonnull(strstr(output, "20220227.3"));
   }

   {
      g_autofree gchar *output = NULL;
      g_autofree gchar *update_status = NULL;

      g_debug("Starting an update that is expected to complete in 1.5 seconds");
      output = _au_execute_manager("update", MOCK_SUCCESS, FALSE, f->test_envp, &error);
      g_assert_no_error(error);
      g_assert_nonnull(strstr(output, "Update completed"));
      update_status =
         _au_execute_manager("get-update-status", NULL, FALSE, f->test_envp, NULL);
      g_assert_cmpstr(update_status, ==, "successful\n");
   }

   au_tests_stop_daemon_service(daemon_proc);
}

static gboolean
get_daemon_debug_status(GDBusConnection *bus)
{
   g_autoptr(GVariant) reply = NULL;
   g_autoptr(GVariant) variant_reply = NULL;

   reply =
      send_atomupd_message(bus, "/org/gtk/Debugging", "org.freedesktop.DBus.Properties",
                           "Get", "(ss)", "org.gtk.Debugging", "DebugEnabled");

   g_variant_get(reply, "(v)", &variant_reply);

   return g_variant_get_boolean(variant_reply);
}

static void
test_verbose(Fixture *f, gconstpointer context)
{
   g_autoptr(GSubprocess) daemon_proc = NULL;
   g_autoptr(GDBusConnection) bus = NULL;
   g_autoptr(GError) error = NULL;
   g_autofree gchar *update_file_path = NULL;

   bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);

   _skip_if_daemon_is_running(bus, NULL);

   update_file_path = g_build_filename(f->srcdir, "data", "update_one_minor.json", NULL);
   f->test_envp =
      g_environ_setenv(f->test_envp, "G_TEST_UPDATE_JSON", update_file_path, TRUE);

   daemon_proc = au_tests_start_daemon_service(bus, f->manifest_path, f->conf_path,
                                               f->test_envp, FALSE);

   {
      g_autofree gchar *output = NULL;

      g_assert_false(get_daemon_debug_status(bus));
      output = _au_execute_manager("check", NULL, TRUE, f->test_envp, &error);
      g_assert_no_error(error);
      g_assert_nonnull(strstr(output, "20220227.3"));
      /* At the end of the execution the daemon debug flag should be set to false once
       * again */
      g_assert_false(get_daemon_debug_status(bus));
   }

   {
      g_autofree gchar *output = NULL;

      g_debug(
         "Starting an update with --verbose that is expected to complete in 1.5 seconds");
      output = _au_execute_manager("update", MOCK_SUCCESS, TRUE, f->test_envp, &error);
      g_assert_no_error(error);
      g_assert_nonnull(strstr(output, "Update completed"));
      /* At the end of the execution the daemon debug flag should be set to false once
       * again */
      g_assert_false(get_daemon_debug_status(bus));
   }

   send_atomupd_message(bus, "/org/gtk/Debugging", "org.gtk.Debugging", "SetDebugEnabled",
                        "(b)", TRUE);

   {
      g_autofree gchar *output = NULL;

      g_assert_true(get_daemon_debug_status(bus));
      output = _au_execute_manager("check", NULL, TRUE, f->test_envp, &error);
      g_assert_no_error(error);
      g_assert_nonnull(strstr(output, "20220227.3"));
      /* The debug option was already enabled, so it should not be changed to false */
      g_assert_true(get_daemon_debug_status(bus));
   }

   {
      g_autofree gchar *output = NULL;

      g_assert_true(get_daemon_debug_status(bus));
      g_debug(
         "Starting an update with --verbose that is expected to complete in 1.5 seconds");
      output = _au_execute_manager("update", MOCK_SUCCESS, TRUE, f->test_envp, &error);
      g_assert_no_error(error);
      g_assert_nonnull(strstr(output, "Update completed"));
      /* The debug option was already enabled, so it should not be changed to false */
      g_assert_true(get_daemon_debug_status(bus));
   }

   send_atomupd_message(bus, "/org/gtk/Debugging", "org.gtk.Debugging", "SetDebugEnabled",
                        "(b)", FALSE);

   {
      int multiplier = 1;
      const gchar *manager_argv[] = { "atomupd-manager", "--session",  "--verbose",
                                      "update",          MOCK_SUCCESS, NULL };

      /* Valgrind is really slow, so we start a mock update that takes longer to complete
       * and we wait longer */
      if (g_getenv("AU_TEST_VALGRIND") != NULL) {
         manager_argv[4] = MOCK_SLOW;
         multiplier = 6;
      }

      g_spawn_async(NULL, (gchar **)manager_argv, f->test_envp, G_SPAWN_SEARCH_PATH, NULL,
                    NULL, NULL, &error);

      /* Give it time to start the mock update */
      g_usleep(0.5 * G_USEC_PER_SEC * multiplier);
      /* While the update is in progress we expect the debug status to be turned on */
      g_assert_true(get_daemon_debug_status(bus));
      /* Wait for the update to complete */
      g_usleep(2 * G_USEC_PER_SEC * multiplier);
      g_assert_false(get_daemon_debug_status(bus));
   }

   au_tests_stop_daemon_service(daemon_proc);
}

int
main(int argc, char **argv)
{
   int ret;

   g_test_init(&argc, &argv, NULL);

#define test_add(_name, _test)                                                           \
   g_test_add(_name, Fixture, argv[0], au_tests_setup, _test, au_tests_teardown)

   test_add("/manager/check_updates", test_check_updates);
   test_add("/manager/multiple_method_calls", test_multiple_method_calls);
   test_add("/manager/verbose", test_verbose);

   ret = g_test_run();
   return ret;
}
