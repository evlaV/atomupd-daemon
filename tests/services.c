/*
 * Copyright © 2022-2023 Collabora Ltd.
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

#include "atomupd-daemon/utils.h"
#include "services.h"

/*
 * Returns: %TRUE if the atomic update daemon service is running. Otherwise
 * it will return %FALSE and will set @error (if provided).
 */
gboolean
au_tests_is_daemon_service_running(GDBusConnection *bus, GError **error)
{
   g_autoptr(GVariant) ping_reply = NULL;

   g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

   g_assert_nonnull(bus);

   ping_reply = g_dbus_connection_call_sync(
      bus, AU_ATOMUPD1_BUS_NAME, AU_ATOMUPD1_PATH, "org.freedesktop.DBus.Peer", "Ping",
      g_variant_new("()"), /* consumed */
      NULL, G_DBUS_CALL_FLAGS_NONE, 1000, NULL, error);

   return ping_reply != NULL;
}

/*
 * @daemon_proc: Daemon process that needs to be stopped
 */
void
au_tests_stop_daemon_service(GSubprocess *daemon_proc)
{
   g_return_if_fail(daemon_proc != NULL);

   g_debug("Stopping the daemon service");

   g_subprocess_send_signal(daemon_proc, SIGTERM);
   g_usleep(0.5 * G_USEC_PER_SEC);
   /* Ensure that the daemon is really killed */
   g_subprocess_force_exit(daemon_proc);
}

/*
 * @manifest_path: (type filename) (not nullable): Path to the daemon manifest file
 * @conf_dir: (type filename) (nullable): Path to the daemon configuration directory
 */
GSubprocess *
au_tests_start_daemon_service(GDBusConnection *bus,
                              const gchar *manifest_path,
                              const gchar *conf_dir,
                              gchar **envp,
                              gboolean expected_to_fail)
{
   g_autoptr(GSubprocessLauncher) proc_launcher = NULL;
   g_autoptr(GSubprocess) proc = NULL;
   gulong wait = 0.5 * G_USEC_PER_SEC;
   gsize max_wait = 10;
   gsize i;
   g_autoptr(GError) error = NULL;

   g_return_val_if_fail(bus != NULL, NULL);
   g_return_val_if_fail(manifest_path != NULL, NULL);

   const gchar *daemon_argv[] = {
      "atomupd-daemon",
      "--session",
      "--manifest-file",
      manifest_path,
      conf_dir == NULL ? NULL : "--config-directory",
      conf_dir,
      NULL,
   };

   /* Valgrind is really slow, so we need to increase our default wait time */
   if (g_getenv("AU_TEST_VALGRIND") != NULL)
      wait = 4 * wait;

   proc_launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE);
   g_subprocess_launcher_set_environ(proc_launcher, envp);
   proc = g_subprocess_launcher_spawnv(proc_launcher, daemon_argv, &error);
   g_assert_no_error(error);

   g_debug("Executed the D-Bus daemon service");

   g_usleep(wait);
   /* Wait up to 5 seconds (10 times 500ms) for the D-Bus service to start.
    * If after that time the service is still not up, it's safe to assume that
    * something went wrong. */
   for (i = 0; i < max_wait; i++) {
      if (au_tests_is_daemon_service_running(bus, &error))
         break;

      g_debug("Atomupd service is not ready: %s", error->message);
      g_clear_error(&error);
      g_usleep(wait);
   }

   if (expected_to_fail) {
      g_assert_cmpint(i, ==, max_wait);
      g_debug("The service didn't start");
      return NULL;
   }

   g_assert_cmpint(i, <, max_wait);

   g_debug("The service successfully started");

   return g_steal_pointer(&proc);
}

/*
 * @rauc_pid_path: Path to the file where the RAUC service pid will be stored
 */
GSubprocess *
au_tests_launch_rauc_service(const gchar *rauc_pid_path)
{
   g_autoptr(GSubprocess) proc = NULL;
   g_autoptr(GError) error = NULL;

   /* Launch a mock rauc service that does nothing until it receives
    * the SIGTERM signal. This will allow us to test the cancel and
    * pause methods. */
   proc = g_subprocess_new(G_SUBPROCESS_FLAGS_SEARCH_PATH_FROM_ENVP, &error,
                           "mock-rauc-service", NULL);
   g_assert_no_error(error);

   g_debug("Launched a mock rauc service with pid %s", g_subprocess_get_identifier(proc));

   g_file_set_contents(rauc_pid_path, g_subprocess_get_identifier(proc), -1, &error);
   g_assert_no_error(error);

   return g_steal_pointer(&proc);
}
