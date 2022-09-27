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
#include <string.h>
#include <unistd.h>
#include <libelf.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <glib-unix.h>
#include <gio/gio.h>

#include "atomupd-daemon/utils.h"

#define _send_atomupd_message_with_null_reply(_bus, _method, _type, _content) \
  g_assert_null (_send_atomupd_message (_bus, _method, _type, _content))

#define _send_atomupd_message(_bus, _method, _format,  ...)  \
  _send_message(_bus, "com.steampowered.Atomupd1", _method, _format, __VA_ARGS__)

#define _send_properties_message(_bus, _method, _format,  ...)  \
  _send_message(_bus, "org.freedesktop.DBus.Properties", _method, _format, __VA_ARGS__)

static const char *argv0;

typedef struct
{
  gchar *srcdir;
  gchar *builddir;
  gchar *manifest_path;
  gchar *updates_json;
  /* Text file where we store the mock RAUC service pid */
  gchar *rauc_pid_path;
  GStrv test_envp;
} Fixture;

static void
setup (Fixture *f,
       gconstpointer context)
{
  int fd;
  g_autoptr(GError) error = NULL;

  f->srcdir = g_strdup (g_getenv ("G_TEST_SRCDIR"));
  f->builddir = g_strdup (g_getenv ("G_TEST_BUILDDIR"));

  if (f->srcdir == NULL)
    f->srcdir = g_path_get_dirname (argv0);

  if (f->builddir == NULL)
    f->builddir = g_path_get_dirname (argv0);

  f->manifest_path = g_build_filename (f->srcdir, "data", "manifest.json", NULL);

  fd = g_file_open_tmp ("steamos-atomupd-XXXXXX.json", &f->updates_json, &error);
  g_assert_no_error (error);
  close (fd);
  /* Start with the update JSON not available */
  g_assert_cmpint (g_unlink (f->updates_json), ==, 0);

  fd = g_file_open_tmp ("rauc-pid-XXXXXX", &f->rauc_pid_path, &error);
  g_assert_no_error (error);
  close (fd);
  /* Start with the mock RAUC service stopped */
  g_assert_cmpint (g_unlink (f->rauc_pid_path), ==, 0);

  f->test_envp = g_get_environ ();
  /* Override the daemon PATH to ensure that it will use the mock steamos-atomupd-client */
  f->test_envp = g_environ_setenv (f->test_envp, "PATH", f->builddir, TRUE);
  f->test_envp = g_environ_setenv (f->test_envp, "AU_UPDATES_JSON_FILE",
                                   f->updates_json, TRUE);
  f->test_envp = g_environ_setenv (f->test_envp, "G_TEST_MOCK_RAUC_SERVICE_PID",
                                   f->rauc_pid_path, TRUE);
}

static void
teardown (Fixture *f,
          gconstpointer context)
{
  g_free (f->srcdir);
  g_free (f->builddir);
  g_free (f->manifest_path);
  g_strfreev (f->test_envp);

  g_unlink (f->updates_json);
  g_free (f->updates_json);

  g_unlink (f->rauc_pid_path);
  g_free (f->rauc_pid_path);
}

typedef struct
{
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
} AtomupdProperties;

static void
atomupd_properties_free (AtomupdProperties *atomupd_properties)
{
  g_clear_pointer (&atomupd_properties->update_version, g_free);
  g_clear_pointer (&atomupd_properties->variant, g_free);
  g_clear_pointer (&atomupd_properties->failure_code, g_free);
  g_clear_pointer (&atomupd_properties->failure_message, g_free);
  g_clear_pointer (&atomupd_properties->current_version, g_free);

  g_free (atomupd_properties);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (AtomupdProperties, atomupd_properties_free)

typedef struct
{
  const gchar *buildid;
  const gchar *variant;
  guint64 estimated_size;
  AuUpdateType update_type; /* Defaults to AU_UPDATE_TYPE_MINOR */
  const gchar *requires_buildid;
} VersionsTest;

typedef struct
{
  const gchar *update_json;
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

/*
 * If @body is floating, this method will assume ownership of @body.
 */
static GVariant *
_send_message (GDBusConnection *bus,
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

  message = g_dbus_message_new_method_call ("com.steampowered.Atomupd1",
                                            "/com/steampowered/Atomupd1",
                                            interface,
                                            method);

  if (format_message != NULL)
    {
      va_start (ap, format_message);
      g_dbus_message_set_body (message, g_variant_new_va (format_message, NULL, &ap));
      va_end (ap);
    }

  reply = g_dbus_connection_send_message_with_reply_sync (bus,
                                                          message,
                                                          G_DBUS_SEND_MESSAGE_FLAGS_NONE,
                                                          3000,
                                                          NULL,
                                                          NULL,
                                                          &error);
  g_assert_no_error (error);

  reply_body = g_dbus_message_get_body (reply);

  if (reply_body == NULL)
    {
      g_debug ("The method \"%s\" didn't return anything", method);
      return NULL;
    }

  variant_string = g_variant_print (reply_body, TRUE);
  g_debug ("Reply body of \"%s\": %s", method, variant_string);

  return g_variant_ref_sink (reply_body);
}

/*
 * Returns: %TRUE if the atomic update daemon service is running. Otherwise
 * it will return %FALSE and will set @error (if provided).
 */
static gboolean
_is_daemon_service_running (GDBusConnection *bus,
                            GError **error)
{
  g_autoptr(GVariant) ping_reply = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  g_assert_nonnull (bus);

  ping_reply = g_dbus_connection_call_sync (bus,
                                            "com.steampowered.Atomupd1",
                                            "/com/steampowered/Atomupd1",
                                            "org.freedesktop.DBus.Peer",
                                            "Ping",
                                            g_variant_new ("()"), /* consumed */
                                            NULL,
                                            G_DBUS_CALL_FLAGS_NONE,
                                            1000,
                                            NULL,
                                            error);

  return ping_reply != NULL;
}

static void
_stop_daemon_service (GPid daemon_pid)
{
  g_return_if_fail (daemon_pid > 0);

  g_debug ("Stopping the daemon service");

  kill (daemon_pid, SIGTERM);
  g_usleep (0.5 * G_USEC_PER_SEC);
  /* Ensure that the daemon is really killed */
  kill (daemon_pid, SIGKILL);
}

/*
 * @daemon_pid: (out) (not optional)
 */
static void
_start_daemon_service (GDBusConnection *bus,
                       const gchar *manifest_path,
                       gchar **envp,
                       GPid *daemon_pid)
{
  g_return_if_fail (manifest_path != NULL);
  g_return_if_fail (daemon_pid != NULL);

  gulong wait = 0.5 * G_USEC_PER_SEC;
  gsize max_wait = 10;
  gsize i;
  g_autoptr(GError) error = NULL;

  const gchar *daemon_argv[] = {
    "atomupd-daemon",
    "--session",
    "--manifest-file",
    manifest_path,
    NULL,
  };

  g_spawn_async (NULL,
                 (gchar **) daemon_argv,
                 envp,
                 G_SPAWN_SEARCH_PATH,
                 NULL,
                 NULL,
                 daemon_pid,
                 &error);
  g_assert_no_error (error);

  g_debug ("Executed the D-Bus daemon service");

  g_usleep (wait);
  /* Wait up to 5 seconds (10 times 500ms) for the D-Bus service to start.
   * If after that time the service is still not up, it's safe to assume that
   * something went wrong. */
  for (i = 0; i < max_wait; i++)
    {
      if (_is_daemon_service_running (bus, &error))
        break;

      g_debug ("Atomupd service is not ready: %s", error->message);
      g_clear_error (&error);
      g_usleep (wait);
    }

  g_assert_cmpint (i, <, max_wait);

  g_debug ("The service successfully started");
}

static void
_check_available_versions (GVariantIter *available_iter,
                           const VersionsTest *versions_available)
{
  gchar *buildid; /* borrowed */
  GVariant *values = NULL; /* borrowed */
  g_autoptr(GVariantType) type_string = g_variant_type_new ("s");
  g_autoptr(GVariantType) type_uint64 = g_variant_type_new ("t");
  g_autoptr(GVariantType) type_uint32 = g_variant_type_new ("u");
  gsize i;

  for (i = 0; g_variant_iter_loop (available_iter, "{s@a{sv}}", &buildid, &values); i++)
    {
      g_autoptr(GVariant) variant = NULL;
      g_autoptr(GVariant) estimated_size = NULL;
      g_autoptr(GVariant) update_type = NULL;
      g_autoptr(GVariant) requires = NULL;
      const gchar *requires_str = NULL;
      const VersionsTest *expected_update = &versions_available[i];

      g_assert_cmpstr (expected_update->buildid, ==, buildid);

      variant = g_variant_lookup_value (values, "variant", type_string);
      g_assert_cmpstr (expected_update->variant, ==, g_variant_get_string (variant, NULL));

      estimated_size = g_variant_lookup_value (values, "estimated_size", type_uint64);
      g_assert_cmpuint (expected_update->estimated_size, ==,
                        g_variant_get_uint64 (estimated_size));

      update_type = g_variant_lookup_value (values, "update_type", type_uint32);
      g_assert_cmpuint (expected_update->update_type, ==,
                        g_variant_get_uint32 (update_type));

      requires = g_variant_lookup_value (values, "requires", type_string);
      requires_str = requires == NULL ? NULL : g_variant_get_string (requires, NULL);
      g_assert_cmpstr (expected_update->requires_buildid, ==, requires_str);
    }
  g_assert_cmpstr (versions_available[i].buildid, ==, NULL);
}

static GVariant *
_get_atomupd_property (GDBusConnection *bus,
                       const gchar *property)
{
  g_autoptr(GVariant) reply = NULL;
  g_autoptr(GVariant) reply_variant = NULL;

  g_debug ("Getting the \"%s\" property", property);
  reply = _send_properties_message (bus, "Get", "(ss)", "com.steampowered.Atomupd1",
                                    property);
  g_variant_get (reply, "(v)", &reply_variant);

  return g_steal_pointer (&reply_variant);
}

static void
_check_versions_property (GDBusConnection *bus,
                          const gchar *property,
                          const VersionsTest *versions_available)
{
  g_autoptr(GVariant) reply = NULL;
  g_autoptr(GVariantIter) versions_iter = NULL;

  reply = _get_atomupd_property (bus, property);
  g_variant_get (reply, "a{?*}", &versions_iter);

  _check_available_versions (versions_iter, versions_available);
}

static void
_call_check_for_updates (GDBusConnection *bus,
                         const VersionsTest *versions_available,
                         const VersionsTest *versions_available_later)
{
  g_autoptr(GVariant) reply = NULL;
  g_autoptr(GVariantIter) available_iter = NULL;
  g_autoptr(GVariantIter) available_later_iter = NULL;

  g_debug ("Calling the \"CheckForUpdates\" method");

  reply = _send_atomupd_message (bus, "CheckForUpdates", "(a{sv})", NULL);
  g_assert_nonnull (reply);

  g_variant_get (reply, "(a{?*}a{?*})", &available_iter, &available_later_iter);

  if (versions_available != NULL)
    _check_available_versions (available_iter, versions_available);

  if (versions_available_later != NULL)
    _check_available_versions (available_later_iter, versions_available_later);
}

static void
test_query_updates (Fixture *f,
                    gconstpointer context)
{
  gsize i;
  g_autoptr(GDBusConnection) bus = NULL;

  bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);

  if (_is_daemon_service_running (bus, NULL))
    {
      g_test_skip ("Can't run this test if another instance of the Atomupd "
                   "daemon service is already running");
      return;
    }

  for (i = 0; i < G_N_ELEMENTS (updates_test); i++)
    {
      GPid daemon_pid;
      const UpdatesTest *test = &updates_test[i];
      g_autofree gchar *update_file_path = NULL;

      update_file_path = g_build_filename (f->srcdir, "data", test->update_json, NULL);
      f->test_envp = g_environ_setenv (f->test_envp, "G_TEST_UPDATE_JSON",
                                       update_file_path, TRUE);

      _start_daemon_service (bus, f->manifest_path, f->test_envp, &daemon_pid);

      _call_check_for_updates (bus, test->versions_available,
                               test->versions_available_later);

      _check_versions_property (bus, "VersionsAvailable", test->versions_available);
      _check_versions_property (bus, "VersionsAvailableLater", test->versions_available_later);

      _stop_daemon_service (daemon_pid);
    }
}

static AtomupdProperties *
_get_atomupd_properties (GDBusConnection *bus)
{
  g_autoptr(GVariantIter) available_iter = NULL;
  g_autoptr(GVariantIter) available_later_iter = NULL;
  GVariantDict dict;
  g_autoptr(GVariant) reply = NULL;
  g_autoptr(GVariant) properties = NULL;
  g_autoptr(AtomupdProperties) atomupd_properties = g_new0 (AtomupdProperties, 1);

#define assert_variant(_variant, _type, _member) \
  g_assert_true (g_variant_dict_lookup (&dict, _variant, _type, \
                                        &atomupd_properties->_member))

#define assert_variant_dict(_variant, _variant_iter, _member) \
    g_assert_true (g_variant_dict_lookup (&dict, _variant, "a{?*}", &_variant_iter)); \
    atomupd_properties->_member = g_variant_iter_n_children (_variant_iter)

  reply = _send_properties_message (bus, "GetAll", "(s)", "com.steampowered.Atomupd1");
  g_variant_get (reply, "(@a{sv})", &properties);
  g_variant_dict_init (&dict, properties);

  assert_variant ("Version", "u", version);
  assert_variant ("ProgressPercentage", "d", progress_percentage);
  assert_variant ("EstimatedCompletionTime", "t", estimated_completion_time);
  assert_variant ("UpdateStatus", "u", status);
  assert_variant ("UpdateVersion", "s", update_version);
  assert_variant ("Variant", "s", variant);
  assert_variant ("FailureCode", "s", failure_code);
  assert_variant ("FailureMessage", "s", failure_message);
  assert_variant ("CurrentVersion", "s", current_version);

  assert_variant_dict ("VersionsAvailable", available_iter, versions_available_n);
  assert_variant_dict ("VersionsAvailableLater", available_later_iter,
                       versions_available_later_n);

  return g_steal_pointer (&atomupd_properties);
}

static void
test_default_properties (Fixture *f,
                         gconstpointer context)
{
  GPid daemon_pid;
  g_autoptr(GDBusConnection) bus = NULL;
  g_autoptr(AtomupdProperties) atomupd_properties = NULL;

  bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);

  if (_is_daemon_service_running (bus, NULL))
    {
      g_test_skip ("Can't run this test if another instance of the Atomupd "
                   "daemon service is already running");
      return;
    }

  _start_daemon_service (bus, f->manifest_path, f->test_envp, &daemon_pid);

  atomupd_properties = _get_atomupd_properties (bus);
  /* The version of this interface is the number 1 */
  g_assert_cmpuint (atomupd_properties->version, ==, 1);
  g_assert_true (atomupd_properties->progress_percentage == 0);
  g_assert_cmpuint (atomupd_properties->estimated_completion_time, ==, 0);
  g_assert_cmpuint (atomupd_properties->status, ==, AU_UPDATE_STATUS_IDLE);
  g_assert_cmpstr (atomupd_properties->update_version, ==, "");
  /* Variant parsed from "manifest.json" */
  g_assert_cmpstr (atomupd_properties->variant, ==, "steamdeck");
  g_assert_cmpstr (atomupd_properties->failure_code, ==, "");
  g_assert_cmpstr (atomupd_properties->failure_message, ==, "");
  g_assert_cmpuint (atomupd_properties->versions_available_n, ==, 0);
  g_assert_cmpuint (atomupd_properties->versions_available_later_n, ==, 0);
  /* Version buildid parsed from "manifest.json" */
  g_assert_cmpstr (atomupd_properties->current_version, ==, "20220205.2");

  _stop_daemon_service (daemon_pid);
}

static void
_check_message_reply (GDBusConnection *bus,
                      const gchar *method,
                      const gchar *message_type,
                      const gchar *message_content,
                      const gchar *expected_reply)
{
  g_autoptr(GVariant) reply = NULL;
  g_autofree gchar *reply_str = NULL;

  reply = _send_atomupd_message (bus, method, message_type, message_content);
  g_variant_get (reply, "(s)", &reply_str);

  g_assert_cmpstr (reply_str, ==, expected_reply);
}

static void
test_unexpected_methods (Fixture *f,
                         gconstpointer context)
{
  GPid daemon_pid;
  g_autoptr(GDBusConnection) bus = NULL;

  bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);

  if (_is_daemon_service_running (bus, NULL))
    {
      g_test_skip ("Can't run this test if another instance of the Atomupd "
                   "daemon service is already running");
      return;
    }

  _start_daemon_service (bus, f->manifest_path, f->test_envp, &daemon_pid);

  _check_message_reply (bus, "StartUpdate", "(s)", "20220120.1",
                        "It is not possible to start an update before calling \"CheckForUpdates\"");
  _check_message_reply (bus, "PauseUpdate", NULL, NULL,
                        "There isn't an update in progress that can be paused");
  _check_message_reply (bus, "ResumeUpdate", NULL, NULL,
                        "There isn't a paused update that can be resumed");
  _check_message_reply (bus, "CancelUpdate", NULL, NULL,
                        "There isn't an update in progress that can be cancelled");

  _stop_daemon_service (daemon_pid);
}

/*
 * @rauc_pid_path: Path to the file where the RAUC service pid will be stored
 * @envp: (nullable) (element-type filename) (transfer full):
 * @rauc_pid: (out):
 */
static void
_launch_rauc_service (const gchar *rauc_pid_path,
                      gchar **envp,
                      GPid *rauc_pid)
{
  GPid pid;
  gboolean spawn_ret;
  g_autofree gchar *pid_str = NULL;
  g_autoptr(GError) error = NULL;

  const char * const rauc_argv[] = { "mock-rauc-service", NULL };

  /* Launch a mock rauc service that does nothing until it receives
   * the SIGTERM signal. This will allow us to test the cancel and
   * pause methods. */
  spawn_ret = g_spawn_async (NULL,
                             (char **) rauc_argv,
                             envp,
                             G_SPAWN_SEARCH_PATH,
                             NULL,
                             NULL,
                             &pid,
                             &error);
  g_assert_no_error (error);
  g_assert_true (spawn_ret);

  g_debug ("Launched a mock rauc service with pid %i", pid);

  if (rauc_pid)
    *rauc_pid = pid;

  pid_str = g_strdup_printf ("%i", pid);
  g_file_set_contents (rauc_pid_path, pid_str, -1, &error);
  g_assert_no_error (error);
}

static void
test_start_pause_stop_update (Fixture *f,
                              gconstpointer context)
{
  GPid daemon_pid;
  GPid rauc_pid;
  AuUpdateStatus status;
  g_autofree gchar *update_file_path = NULL;
  g_autoptr(GVariant) reply = NULL;
  g_autoptr(GDBusConnection) bus = NULL;
  g_autoptr(AtomupdProperties) atomupd_properties = NULL;
  g_autoptr(GDateTime) time_now = NULL;

  bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);

  if (_is_daemon_service_running (bus, NULL))
    {
      g_test_skip ("Can't run this test if another instance of the Atomupd "
                   "daemon service is already running");
      return;
    }

  update_file_path = g_build_filename (f->srcdir, "data", "update_one_minor.json", NULL);
  f->test_envp = g_environ_setenv (f->test_envp, "G_TEST_UPDATE_JSON",
                                   update_file_path, TRUE);

  _launch_rauc_service (f->rauc_pid_path, f->test_envp, &rauc_pid);

  _start_daemon_service (bus, f->manifest_path, f->test_envp, &daemon_pid);

  _call_check_for_updates (bus, NULL, NULL);

  /* Restart the service. When starting an update we expect that it shouldn't
   * complain that we didn't check for updates, because we already did. */
  _stop_daemon_service (daemon_pid);
  _start_daemon_service (bus, f->manifest_path, f->test_envp, &daemon_pid);

  /* Assert that restarting the daemon successfully killed the old rauc service */
  g_assert_cmpint (kill (rauc_pid, 0) , !=, 0);

  _launch_rauc_service (f->rauc_pid_path, f->test_envp, &rauc_pid);

  g_debug ("Starting an update that is expected to complete in 1.5 seconds");
  _send_atomupd_message_with_null_reply (bus, "StartUpdate", "(s)", "mock-success");

  g_usleep (2 * G_USEC_PER_SEC);

  reply = _get_atomupd_property (bus, "UpdateStatus");
  g_variant_get (reply, "u", &status);
  g_assert_cmpuint (status, ==, AU_UPDATE_STATUS_SUCCESSFUL);

  /* With "mock-infinite" we simulate an update that is in progress.
   * To make it more predictable, it will always print a progress of
   * "16.08% 06m35s" until we cancel it with a SIGTERM. */
  g_debug ("Starting infinite update");
  _send_atomupd_message_with_null_reply (bus, "StartUpdate", "(s)", "mock-infinite");

  g_usleep (G_USEC_PER_SEC);
  time_now = g_date_time_new_now_utc ();
  atomupd_properties = _get_atomupd_properties (bus);
  g_assert_true (atomupd_properties->progress_percentage == 16.08);
  g_assert_cmpuint (atomupd_properties->estimated_completion_time, >,
                    g_date_time_to_unix (time_now));
  g_assert_cmpuint (atomupd_properties->status, ==, AU_UPDATE_STATUS_IN_PROGRESS);
  g_assert_cmpstr (atomupd_properties->update_version, ==, "mock-infinite");
  g_clear_pointer (&atomupd_properties, atomupd_properties_free);

  _send_atomupd_message_with_null_reply (bus, "PauseUpdate", NULL, NULL);
  atomupd_properties = _get_atomupd_properties (bus);
  g_assert_true (atomupd_properties->progress_percentage == 16.08);
  g_assert_cmpuint (atomupd_properties->status, ==, AU_UPDATE_STATUS_PAUSED);
  /* Assert that the mock rauc service has not been killed.
   * Because it is not our own child, we can't check for "WIFSTOPPED". */
  g_assert_cmpint (kill (rauc_pid, 0) , ==, 0);
  g_clear_pointer (&atomupd_properties, atomupd_properties_free);

  _send_atomupd_message_with_null_reply (bus, "ResumeUpdate", NULL, NULL);
  _send_atomupd_message_with_null_reply (bus, "CancelUpdate", NULL, NULL);
  g_usleep (G_USEC_PER_SEC);
  atomupd_properties = _get_atomupd_properties (bus);
  /* When receiving SIGTERM the mock steamos-atomupd-client will print
   * "17.50% 05m50s" and then quit */
  g_assert_true (atomupd_properties->progress_percentage == 17.50);
  g_assert_cmpuint (atomupd_properties->estimated_completion_time, >,
                    g_date_time_to_unix (time_now));
  g_assert_cmpuint (atomupd_properties->status, ==, AU_UPDATE_STATUS_CANCELLED);
  g_assert_cmpstr (atomupd_properties->update_version, ==, "mock-infinite");
  /* Assert that the CancelUpdate successfully killed the rauc service */
  g_assert_cmpint (kill (rauc_pid, 0) , !=, 0);

  _stop_daemon_service (daemon_pid);
}

static void
test_multiple_method_calls (Fixture *f,
                            gconstpointer context)
{
  GPid daemon_pid;
  GPid rauc_pid;
  g_autoptr(GVariant) reply = NULL;
  g_autoptr(GDBusConnection) bus = NULL;
  g_autoptr(AtomupdProperties) atomupd_properties = NULL;

  bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);

  if (_is_daemon_service_running (bus, NULL))
    {
      g_test_skip ("Can't run this test if another instance of the Atomupd "
                   "daemon service is already running");
      return;
    }

  _start_daemon_service (bus, f->manifest_path, f->test_envp, &daemon_pid);

  /* Launch the RAUC service after the atomupd daemon because in its start up
   * process it will kill any eventual RAUC processes that are already running */
  _launch_rauc_service (f->rauc_pid_path, f->test_envp, &rauc_pid);

  _call_check_for_updates (bus, NULL, NULL);
  reply = _send_atomupd_message (bus, "CheckForUpdates", "(a{sv})", NULL);
  g_assert_nonnull (reply);

  _send_atomupd_message_with_null_reply (bus, "StartUpdate", "(s)", "mock-infinite");
  _send_atomupd_message_with_null_reply (bus, "PauseUpdate", NULL, NULL);
  /* Pausing again should not be allowed */
  _check_message_reply (bus, "PauseUpdate", NULL, NULL,
                        "There isn't an update in progress that can be paused");
  /* It is expected to be possible to cancel a paused update */
  _send_atomupd_message_with_null_reply (bus, "CancelUpdate", NULL, NULL);
  g_usleep (G_USEC_PER_SEC);
  atomupd_properties = _get_atomupd_properties (bus);
  g_assert_cmpuint (atomupd_properties->status, ==, AU_UPDATE_STATUS_CANCELLED);
  g_assert_cmpint (kill (rauc_pid, 0) , !=, 0);

  _stop_daemon_service (daemon_pid);
}

typedef struct
{
  const gchar *file_content;
  const gchar *expected_update_version;
  AuUpdateStatus expected_status;
} RebootForUpdateTest;

static const RebootForUpdateTest reboot_for_update_test[] =
{
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
test_restarted_service (Fixture *f,
                        gconstpointer context)
{
  g_autoptr(GDBusConnection) bus = NULL;
  g_autoptr(GError) error = NULL;
  gsize i;

  bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);

  if (_is_daemon_service_running (bus, NULL))
    {
      g_test_skip ("Can't run this test if another instance of the Atomupd "
                   "daemon service is already running");
      return;
    }

  for (i = 0; i < G_N_ELEMENTS (reboot_for_update_test); i++)
    {
      GPid daemon_pid;
      gint fd;
      const RebootForUpdateTest *test = &reboot_for_update_test[i];
      g_autoptr(AtomupdProperties) atomupd_properties = NULL;
      g_autofree gchar *reboot_for_update = NULL;

      if (test->file_content != NULL)
        {
          fd = g_file_open_tmp ("reboot_for_update-XXXXXX", &reboot_for_update, &error);
          g_assert_no_error (error);
          g_assert_cmpint (fd, !=, -1);
          close (fd);

          g_file_set_contents (reboot_for_update, test->file_content, -1, &error);
          g_assert_no_error (error);

          f->test_envp = g_environ_setenv (f->test_envp, "AU_REBOOT_FOR_UPDATE",
                                           reboot_for_update, TRUE);
        }
      else
        {
          f->test_envp = g_environ_setenv (f->test_envp, "AU_REBOOT_FOR_UPDATE",
                                           "/missing_file", TRUE);
        }

      _start_daemon_service (bus, f->manifest_path, f->test_envp, &daemon_pid);

      atomupd_properties = _get_atomupd_properties (bus);
      g_assert_cmpstr (atomupd_properties->update_version, ==, test->expected_update_version);
      g_assert_cmpuint (atomupd_properties->status, ==, test->expected_status);

      _stop_daemon_service (daemon_pid);
      g_unlink (reboot_for_update);
    }
}

int
main (int argc,
      char **argv)
{
  int ret;

  argv0 = argv[0];

  g_test_init (&argc, &argv, NULL);

#define test_add(_name, _test) \
    g_test_add(_name, Fixture, NULL, setup, _test, teardown)

  test_add ("/daemon/updates", test_query_updates);
  test_add ("/daemon/default_properties", test_default_properties);
  test_add ("/daemon/unexpected_methods", test_unexpected_methods);
  test_add ("/daemon/start_pause_stop_update", test_start_pause_stop_update);
  test_add ("/daemon/multiple_method_calls", test_multiple_method_calls);
  test_add ("/daemon/restarted_service", test_restarted_service);

  ret = g_test_run ();
  return ret;
}
