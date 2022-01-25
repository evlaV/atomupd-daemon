/*
 * Copyright © 2021-2022 Collabora Ltd.
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

#include <sysexits.h>

#include <glib.h>
#include <glib-unix.h>
#include <gio/gio.h>

#include "au-atomupd1-impl.h"

static GMainLoop *main_loop = NULL;

static void
name_acquired_cb (GDBusConnection *connection,
                  const gchar *name,
                  gpointer user_data)
{
  g_debug ("Acquired the name %s on the system message bus", name);
}

static void
name_lost_cb (GDBusConnection *connection,
              const gchar *name,
              gpointer user_data)
{
  g_debug ("Lost the name %s on the system message bus", name);
  g_main_loop_quit (main_loop);
}

static gboolean
on_sigint (gpointer user_data)
{
  g_debug ("Caught SIGINT. Initiating shutdown.");
  g_main_loop_quit (main_loop);
  return FALSE;
}

static gboolean opt_replace = FALSE;
static gboolean opt_verbose = FALSE;
static gboolean opt_version = FALSE;

static GOptionEntry options[] =
{
  { "replace", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &opt_replace,
    "Replace a previous instance with the same bus name.",
    NULL },
  { "verbose", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &opt_verbose,
    "Be more verbose.", NULL },
  { "version", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &opt_version,
    "Print version number and exit.", NULL },
  { NULL }
};

int
main (int argc,
      char *argv[])
{
  GBusNameOwnerFlags flags;
  g_autoptr(GError) local_error = NULL;
  g_autoptr(GOptionContext) option_context = NULL;
  g_autoptr(GDBusConnection) bus = NULL;
  g_autoptr(AuAtomupd1) atomupd = NULL;
  const gchar *atomupd1_path = "/com/steampowered/Atomupd1";
  const gchar *atomupd1_bus_name = "com.steampowered.Atomupd1";
  GError **error = &local_error;

  option_context = g_option_context_new ("");
  g_option_context_add_main_entries (option_context, options, NULL);

  if (!g_option_context_parse (option_context, &argc, &argv, &local_error))
    {
      return EX_USAGE;
    }

  if (opt_version)
    {
      g_print ("%s:\n"
               " Package: atomupd-daemon\n"
               " Version: %s\n",
               g_get_prgname (), VERSION);
      return EXIT_SUCCESS;
    }

  main_loop = g_main_loop_new (NULL, FALSE);
  g_unix_signal_add (SIGINT, on_sigint, NULL);

  flags = G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT;

  if (opt_replace)
    flags |= G_BUS_NAME_OWNER_FLAGS_REPLACE;

  atomupd = au_atomupd1_impl_new ();
  bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &local_error);

  if (bus == NULL)
    {
      g_warning ("An error occurred while connecting to the system bus: %s",
                 local_error->message);
      return EXIT_FAILURE;
    }

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (atomupd),
                                         bus, atomupd1_path, error))
    {
      g_warning ("An error occurred while registering the D-Bus object '%s': %s",
                 atomupd1_path, local_error->message);
      return EXIT_FAILURE;
    }

  g_bus_own_name_on_connection (bus,
                                atomupd1_bus_name,
                                flags,
                                name_acquired_cb,
                                name_lost_cb,
                                NULL,
                                NULL);

  g_debug ("Starting the main loop");

  g_main_loop_run (main_loop);

  return EXIT_SUCCESS;
}
