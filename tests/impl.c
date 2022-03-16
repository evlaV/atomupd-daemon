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
#include <glib-unix.h>
#include <gio/gio.h>

#include "atomupd-daemon/utils.h"

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
} Fixture;

static void
setup (Fixture *f,
       gconstpointer context)
{
  f->srcdir = g_strdup (g_getenv ("G_TEST_SRCDIR"));
  f->builddir = g_strdup (g_getenv ("G_TEST_BUILDDIR"));

  if (f->srcdir == NULL)
    f->srcdir = g_path_get_dirname (argv0);

  if (f->builddir == NULL)
    f->builddir = g_path_get_dirname (argv0);

  f->manifest_path = g_build_filename (f->srcdir, "data", "manifest.json", NULL);
}

static void
teardown (Fixture *f,
          gconstpointer context)
{
  g_free (f->srcdir);
  g_free (f->builddir);
  g_free (f->manifest_path);
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
  g_auto(GStrv) test_envp = g_get_environ ();

  bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);

  if (_is_daemon_service_running (bus, NULL))
    {
      g_test_skip ("Can't run this test if another instance of the Atomupd "
                   "daemon service is already running");
      return;
    }

  /* Override the daemon PATH to ensure that it will use the mock steamos-atomupd-client */
  test_envp = g_environ_setenv (test_envp, "PATH", f->builddir, TRUE);

  for (i = 0; i < G_N_ELEMENTS (updates_test); i++)
    {
      GPid daemon_pid;
      const UpdatesTest *test = &updates_test[i];
      g_autofree gchar *update_file_path = NULL;

      update_file_path = g_build_filename (f->srcdir, "data", test->update_json, NULL);
      test_envp = g_environ_setenv (test_envp, "G_TEST_UPDATE_JSON",
                                    update_file_path, TRUE);

      _start_daemon_service (bus, f->manifest_path, test_envp, &daemon_pid);

      _call_check_for_updates (bus, test->versions_available,
                               test->versions_available_later);

      _check_versions_property (bus, "VersionsAvailable", test->versions_available);
      _check_versions_property (bus, "VersionsAvailableLater", test->versions_available_later);

      _stop_daemon_service (daemon_pid);
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

  ret = g_test_run ();
  return ret;
}
