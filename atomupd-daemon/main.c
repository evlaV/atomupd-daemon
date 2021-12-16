/*
 * Copyright © 2021 Collabora Ltd.
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

#include "atomupd-daemon.h"

static GMainLoop *main_loop = NULL;
static AtomupdDaemon *daemon = NULL;

static void
on_bus_acquired (GDBusConnection *connection,
                 const gchar *name,
                 gpointer user_data)
{
  daemon = atomupd_daemon_new (connection);
  g_debug ("Connected to the system bus");
}

static void
on_name_acquired (GDBusConnection *connection,
                  const gchar *name,
                  gpointer user_data)
{
  g_debug ("Acquired the name %s on the system message bus", name);
}

static void
on_name_lost (GDBusConnection *connection,
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
    "Replace a previous instance with the same bus name. "
    "Ignored if --bus-name is not used.",
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
  GError **error = &local_error;
  guint name_owner_id = 0;
  int ret = 1;

  option_context = g_option_context_new ("");
  g_option_context_add_main_entries (option_context, options, NULL);

  if (!g_option_context_parse (option_context, &argc, &argv, &local_error))
    {
      ret = EX_USAGE;
      goto out;
    }

  if (opt_version)
    {
      g_print ("%s:\n"
               " Package: atomupd-daemon\n"
               " Version: %s\n",
               g_get_prgname (), VERSION);
      ret = 0;
      goto out;
    }

  main_loop = g_main_loop_new (NULL, FALSE);
  g_unix_signal_add (SIGINT, on_sigint, NULL);

  flags = G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT;

  if (opt_replace)
    flags |= G_BUS_NAME_OWNER_FLAGS_REPLACE;

  // TODO we probably want "system" and not "session" here
  name_owner_id = g_bus_own_name (G_BUS_TYPE_SESSION,
                                  "com.steampowered.Atomupd1",
                                  flags,
                                  on_bus_acquired,
                                  on_name_acquired,
                                  on_name_lost,
                                  NULL,
                                  NULL);

  g_debug ("Starting the main loop");

  g_main_loop_run (main_loop);

out:
  return ret;
}
