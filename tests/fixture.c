/*
 * Copyright © 2022-2025 Collabora Ltd.
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

#include <gio/gio.h>
#include <glib-unix.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "fixture.h"
#include "tests-utils.h"

static GPid
_start_mock_polkit(GDBusConnection *system_bus)
{
   g_autoptr(GVariant) ping_reply = NULL;
   g_autoptr(GError) error = NULL;
   gsize i = 0;
   gulong wait_for_polkit = 0.2 * G_USEC_PER_SEC;
   GPid polkit_pid = -1;

   /* When using Valgrind we increase the wait time 20x because the execution
    * is much slower, especially when running with the Gitlab CI. */
   if (g_getenv("AU_TEST_VALGRIND") != NULL)
      wait_for_polkit = 20 * wait_for_polkit;

   const gchar *polkit_argv[] = {
      "/usr/bin/python3", "-m", "dbusmock", "--template", "polkitd", NULL,
   };

   g_spawn_async(NULL, (gchar **)polkit_argv, NULL, G_SPAWN_DEFAULT, NULL, NULL,
                 &polkit_pid, &error);

   g_assert_no_error(error);

   /* Wait for the mock polkit D-Bus object to start */
   while (ping_reply == NULL && i < 15) {
      g_usleep(wait_for_polkit);

      if (i > 0)
         g_debug("Waiting for for the mock polkit to start: %li", i);

      ping_reply = g_dbus_connection_call_sync(
         system_bus, "org.freedesktop.PolicyKit1",
         "/org/freedesktop/PolicyKit1/Authority", "org.freedesktop.DBus.Peer", "Ping",
         g_variant_new("()"), /* consumed */
         NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, 1000, NULL, NULL);

      i++;
   }

   g_assert(ping_reply != NULL);

   g_debug("Mock Polkit started");

   return polkit_pid;
}

static void
_stop_mock_polkit(GPid polkit_pid)
{
   if (polkit_pid < 1)
      return;

   kill(polkit_pid, SIGTERM);
   g_usleep(0.5 * G_USEC_PER_SEC);
   /* Ensure that the polkit service is really dead */
   kill(polkit_pid, SIGKILL);
   waitpid(polkit_pid, NULL, 0);
}

void
mock_polkit_set_allowed(const gchar **allowed, gsize n_elements)
{
   g_autoptr(GDBusMessage) message = NULL;
   g_autoptr(GDBusMessage) reply = NULL;
   g_autoptr(GDBusConnection) bus = NULL;
   g_autoptr(GError) error = NULL;
   GVariant *elements_input = NULL; /* floating */

   bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
   g_assert_no_error(error);

   message = g_dbus_message_new_method_call("org.freedesktop.PolicyKit1",
                                            "/org/freedesktop/PolicyKit1/Authority",
                                            "org.freedesktop.DBus.Mock", "SetAllowed");

   elements_input = g_variant_new_strv(allowed, n_elements);

   g_dbus_message_set_body(message, g_variant_new_tuple(&elements_input, 1));

   reply = g_dbus_connection_send_message_with_reply_sync(
      bus, message, G_DBUS_SEND_MESSAGE_FLAGS_NONE, 3000, NULL, NULL, &error);
   g_assert_no_error(error);
}

void
au_tests_setup(Fixture *f, gconstpointer context)
{
   int fd;
   const char *argv0 = context;
   g_autoptr(GDBusConnection) system_bus = NULL;
   g_autoptr(GError) error = NULL;

   const gchar *polkit_allow_all[] = {
      "com.steampowered.atomupd1.check-for-updates",
      "com.steampowered.atomupd1.manage-pending-update",
      "com.steampowered.atomupd1.reload-configuration",
      "com.steampowered.atomupd1.start-custom-upgrade",
      "com.steampowered.atomupd1.start-downgrade",
      "com.steampowered.atomupd1.start-upgrade",
      "com.steampowered.atomupd1.switch-variant-or-branch",
      "com.steampowered.atomupd1.manage-http-proxy",
      "com.steampowered.atomupd1.manage-trusted-keys",
   };

   f->srcdir = g_strdup(g_getenv("G_TEST_SRCDIR"));
   f->builddir = g_strdup(g_getenv("G_TEST_BUILDDIR"));

   if (f->srcdir == NULL)
      f->srcdir = g_path_get_dirname(argv0);

   if (f->builddir == NULL)
      f->builddir = g_path_get_dirname(argv0);

   f->manifest_path = g_build_filename(f->srcdir, "data", "manifest.json", NULL);
   f->conf_dir = g_build_filename(f->srcdir, "data", NULL);

   fd = g_file_open_tmp("steamos-atomupd-XXXXXX.json", &f->updates_json, &error);
   g_assert_no_error(error);
   close(fd);
   /* Start with the update JSON not available */
   g_assert_cmpint(g_unlink(f->updates_json), ==, 0);

   fd = g_file_open_tmp("rauc-pid-XXXXXX", &f->rauc_pid_path, &error);
   g_assert_no_error(error);
   close(fd);
   /* Start with the mock RAUC service stopped */
   g_assert_cmpint(g_unlink(f->rauc_pid_path), ==, 0);

   fd = g_file_open_tmp("preferences-XXXXXX", &f->preferences_path, &error);
   g_assert_no_error(error);
   close(fd);
   /* Start with the preferences configuration file not available */
   g_assert_cmpint(g_unlink(f->preferences_path), ==, 0);

   fd = g_file_open_tmp("remote-info-XXXXXX", &f->remote_info_path, &error);
   g_assert_no_error(error);
   close(fd);
   /* Start with the remote info file not available */
   g_assert_cmpint(g_unlink(f->remote_info_path), ==, 0);

   fd = g_file_open_tmp("desync-conf-XXXXXX", &f->desync_conf_path, &error);
   g_assert_no_error(error);
   close(fd);
   /* Start with the desync config file not available */
   g_assert_cmpint(g_unlink(f->desync_conf_path), ==, 0);

   f->trusted_keys_dir = g_dir_make_tmp("atomupd-daemon-keys-XXXXXX", &error);
   g_assert_no_error(error);

   f->dev_keys_dir = g_dir_make_tmp("atomupd-daemon-dev-keys-XXXXXX", &error);
   g_assert_no_error(error);

   f->test_envp = g_get_environ();
   f->test_envp =
      g_environ_setenv(f->test_envp, "AU_UPDATES_JSON_FILE", f->updates_json, TRUE);
   f->test_envp = g_environ_setenv(f->test_envp, "G_TEST_MOCK_RAUC_SERVICE_PID",
                                   f->rauc_pid_path, TRUE);
   f->test_envp = g_environ_setenv(f->test_envp, "AU_USER_PREFERENCES_FILE",
                                   f->preferences_path, TRUE);
   f->test_envp =
      g_environ_setenv(f->test_envp, "AU_REMOTE_INFO_PATH", f->remote_info_path, TRUE);
   f->test_envp =
      g_environ_setenv(f->test_envp, "AU_DESYNC_CONFIG_PATH", f->desync_conf_path, TRUE);
   f->test_envp = g_environ_setenv(f->test_envp, "AU_DEFAULT_TRUSTED_KEYS", f->trusted_keys_dir, TRUE);
   f->test_envp =g_environ_setenv(f->test_envp, "AU_DEFAULT_DEV_KEYS", f->dev_keys_dir, TRUE);

   system_bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
   g_assert_no_error(error);
   f->polkit_pid = _start_mock_polkit(system_bus);
   mock_polkit_set_allowed(polkit_allow_all, G_N_ELEMENTS(polkit_allow_all));
}

void
au_tests_teardown(Fixture *f, gconstpointer context)
{
   g_free(f->srcdir);
   g_free(f->builddir);
   g_free(f->manifest_path);
   g_free(f->conf_dir);
   g_strfreev(f->test_envp);

   g_unlink(f->updates_json);
   g_free(f->updates_json);

   g_unlink(f->rauc_pid_path);
   g_free(f->rauc_pid_path);

   g_unlink(f->preferences_path);
   g_free(f->preferences_path);

   g_unlink(f->remote_info_path);
   g_free(f->remote_info_path);

   g_unlink(f->desync_conf_path);
   g_free(f->desync_conf_path);

   rm_rf(f->trusted_keys_dir);
   g_free(f->trusted_keys_dir);

   rm_rf(f->dev_keys_dir);
   g_free(f->dev_keys_dir);

   _stop_mock_polkit(f->polkit_pid);
}
