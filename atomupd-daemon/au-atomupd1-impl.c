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

#include "au-atomupd1-impl.h"

struct _AuAtomupd1Impl
{
  AuAtomupd1Skeleton parent_instance;
};

static void
init_atomupd1_iface (AuAtomupd1Iface *iface)
{
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

  return atomupd;
}
