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

#include <glib.h>
#include <glib-unix.h>
#include <gio/gio.h>

#include "atomupd-daemon.h"

static AdAtomupd1 *launcher;

struct _AtomupdDaemonClass
{
  GObjectClass parent;
};

G_DEFINE_TYPE (AtomupdDaemon, atomupd_daemon, G_TYPE_OBJECT);

void
atomupd_daemon_init (AtomupdDaemon *self)
{

}

static void
atomupd_daemon_class_init (AtomupdDaemonClass *cls)
{
  // GObjectClass *object_class = G_OBJECT_CLASS (cls);
}

AtomupdDaemon *
atomupd_daemon_new (GDBusConnection *connection)
{
  g_return_val_if_fail (G_IS_DBUS_CONNECTION (connection), NULL);

  launcher = ad_atomupd1_skeleton_new ();

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (launcher),
                                         connection,
                                         "/com/steampowered/Atomupd1",
                                         NULL))
    return NULL;

  return NULL;
}
