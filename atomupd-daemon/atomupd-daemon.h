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

#pragma once

#include <stdio.h>
#include <unistd.h>

#include <glib.h>
#include <glib-object.h>
#include <gio/gio.h>

#include "atomupd1.h"

typedef struct _AtomupdDaemon AtomupdDaemon;
typedef struct _AtomupdDaemonClass AtomupdDaemonClass;

#define TYPE_ATOMUPD_DAEMON (atomupd_daemon_get_type ())
#define ATOMUPD_DAEMON(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), TYPE_ATOMUPD_DAEMON, AtomupdDaemon))
#define ATOMUPD_DAEMON_CLASS(cls) (G_TYPE_CHECK_CLASS_CAST ((cls), TYPE_ATOMUPD_DAEMON, AtomupdDaemonClass))
#define IS_ATOMUPD_DAEMON(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), TYPE_ATOMUPD_DAEMON))
#define IS_ATOMUPD_DAEMON_CLASS(cls) (G_TYPE_CHECK_CLASS_TYPE ((cls), TYPE_ATOMUPD_DAEMON))
#define ATOMUPD_DAEMON_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS((obj), TYPE_ATOMUPD_DAEMON, AtomupdDaemonClass)
GType atomupd_daemon_get_type (void);

struct _AtomupdDaemon
{

};

AtomupdDaemon *atomupd_daemon_new (GDBusConnection *connection);
