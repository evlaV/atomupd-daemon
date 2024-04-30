/*
 * Copyright © 2022-2024 Collabora Ltd.
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

/*
 * If @body is floating, this method will assume ownership of @body.
 */

#include <ftw.h>
#include <stdio.h>

#include <gio/gio.h>
#include <glib.h>

#include "tests-utils.h"

GVariant *
send_atomupd_message(GDBusConnection *bus,
                     const gchar *path,
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

   message =
      g_dbus_message_new_method_call(AU_ATOMUPD1_BUS_NAME, path, interface, method);

   if (format_message != NULL) {
      va_start(ap, format_message);
      g_dbus_message_set_body(message, g_variant_new_va(format_message, NULL, &ap));
      va_end(ap);
   }

   reply = g_dbus_connection_send_message_with_reply_sync(
      bus, message, G_DBUS_SEND_MESSAGE_FLAGS_NONE, 3000, NULL, NULL, &error);
   g_assert_no_error(error);

   reply_body = g_dbus_message_get_body(reply);

   if (reply_body == NULL) {
      g_debug("The method \"%s\" didn't return anything", method);
      return NULL;
   }

   variant_string = g_variant_print(reply_body, TRUE);
   g_debug("Reply body of \"%s\": %s", method, variant_string);

   return g_variant_ref_sink(reply_body);
}

static gint
ftw_remove (const gchar *path,
            const struct stat *sb,
            gint typeflags,
            struct FTW *ftwbuf)
{
   if (remove (path) < 0) {
      g_debug ("Unable to remove %s: %s", path, g_strerror (errno));
      return -1;
   }

   return 0;
}

/*
 * Recursively delete @directory within the same file system and
 * without following symbolic links.
 */
gboolean
rm_rf (const char *directory)
{
   if (directory == NULL)
      return TRUE;

   if (nftw (directory, ftw_remove, 10, FTW_DEPTH|FTW_MOUNT|FTW_PHYS) < 0)
      return FALSE;

   return TRUE;
}
