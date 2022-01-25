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

#include <unistd.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <glib-unix.h>
#include <gio/gio.h>

#include "utils.h"
#include "au-atomupd1-impl.h"

struct _AuAtomupd1Impl
{
  AuAtomupd1Skeleton parent_instance;
};

static gboolean
au_atomupd1_impl_handle_check_for_updates (AuAtomupd1 *object,
                                           GDBusMethodInvocation *invocation,
                                           GVariant *arg_options)
{
  g_warning ("TODO: check for update is just a stub!");

  au_atomupd1_complete_check_for_updates (object,
                                          g_steal_pointer (&invocation),
                                          g_variant_new ("a{sa{sv}}", NULL),
                                          g_variant_new ("a{sa{sv}}", NULL));

  return TRUE;
}

static gboolean
au_atomupd1_impl_handle_cancel_update (AuAtomupd1 *object,
                                       GDBusMethodInvocation *invocation)
{
  g_warning ("TODO: cancel update is just a stub!");

  if (au_atomupd1_get_update_status (object) != AU_UPDATE_STATUS_IN_PROGRESS)
    {
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "There isn't an update in progress that can be cancelled");
      return TRUE;
    }

  // au_atomupd1_set_update_status (object, AU_UPDATE_STATUS_CANCELLED);

  au_atomupd1_complete_cancel_update (object, g_steal_pointer (&invocation));

  return TRUE;
}

static gboolean
au_atomupd1_impl_handle_pause_update (AuAtomupd1 *object,
                                      GDBusMethodInvocation *invocation)
{
  g_warning ("TODO: pause update is just a stub!");

  if (au_atomupd1_get_update_status (object) != AU_UPDATE_STATUS_IN_PROGRESS)
    {
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "There isn't an update in progress that can be paused");
      return TRUE;
    }

  // au_atomupd1_set_update_status (object, AU_UPDATE_STATUS_PAUSED);

  au_atomupd1_complete_pause_update (object, g_steal_pointer (&invocation));

  return TRUE;
}

static gboolean
au_atomupd1_impl_handle_start_update (AuAtomupd1 *object,
                                      GDBusMethodInvocation *invocation,
                                      const gchar *arg_id)
{
  g_warning ("TODO: start update is just a stub!");

  // au_atomupd1_set_update_status (object, AU_UPDATE_STATUS_IN_PROGRESS);

  au_atomupd1_complete_start_update (object, g_steal_pointer (&invocation));

  return TRUE;
}

static gboolean
au_atomupd1_impl_handle_resume_update (AuAtomupd1 *object,
                                       GDBusMethodInvocation *invocation)
{
  g_warning ("TODO: resume update is just a stub!");

  if (au_atomupd1_get_update_status (object) != AU_UPDATE_STATUS_PAUSED)
    {
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "There isn't a paused update that can be resumed");
      return TRUE;
    }

  // au_atomupd1_set_update_status (object, AU_UPDATE_STATUS_IN_PROGRESS);

  au_atomupd1_complete_resume_update (object, g_steal_pointer (&invocation));

  return TRUE;
}

static void
init_atomupd1_iface (AuAtomupd1Iface *iface)
{
  iface->handle_cancel_update = au_atomupd1_impl_handle_cancel_update;
  iface->handle_check_for_updates = au_atomupd1_impl_handle_check_for_updates;
  iface->handle_pause_update = au_atomupd1_impl_handle_pause_update;
  iface->handle_resume_update = au_atomupd1_impl_handle_resume_update;
  iface->handle_start_update = au_atomupd1_impl_handle_start_update;
}

G_DEFINE_TYPE_WITH_CODE (AuAtomupd1Impl, au_atomupd1_impl, AU_TYPE_ATOMUPD1_SKELETON,
                         G_IMPLEMENT_INTERFACE (AU_TYPE_ATOMUPD1, init_atomupd1_iface))

static void
au_atomupd1_impl_finalize (GObject *object)
{
  G_OBJECT_CLASS (au_atomupd1_impl_parent_class)->finalize (object);
}

static void
au_atomupd1_impl_class_init (AuAtomupd1ImplClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = au_atomupd1_impl_finalize;
}

static void
au_atomupd1_impl_init (AuAtomupd1Impl *self)
{
}

AuAtomupd1 *
au_atomupd1_impl_new (void)
{
  AuAtomupd1 *atomupd = g_object_new (AU_TYPE_ATOMUPD1_IMPL, NULL);

  /* We currently only have the version 1 of this interface */
  au_atomupd1_set_version (atomupd, 1);

  return atomupd;
}
