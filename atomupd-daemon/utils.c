/*
 * Copyright © 2023-2026 Collabora Ltd.
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

#include <curl/curl.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

#include "utils.h"

/*
 * Convenience function that sets @error, if not %NULL.
 *
 * Returns: %FALSE, to allow writing the compact `return au_throw_error()`
 */
gboolean
au_throw_error(GError **error, const gchar *format, ...)
{
   va_list args;

   g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
   g_return_val_if_fail(format != NULL, FALSE);

   if (error == NULL)
      return FALSE;

   va_start(args, format);
   *error = g_error_new_valist(G_IO_ERROR, G_IO_ERROR_FAILED, format, args);
   va_end(args);
   return FALSE;
}

void
download_data_free(DownloadData *data)
{
   if (data == NULL)
      return;

   g_free(data->target);
   g_free(data->url);
   g_free(data->proxy);
   g_free(data);
}

/*
 * _au_download_thread_func:
 * @task_data: (not nullable): DownloadData pointer
 *
 * Downloads the @task_data->url to the provided @task_data->target. If @task_data->target
 * already exists, it will be replaced. During the download, the temporary file is stored
 * at @task_data->target with the `.part` suffix.
 */
void
_au_download_thread_func(GTask *task,
                         gpointer source_object,
                         gpointer task_data,
                         GCancellable *cancellable)
{
   g_autofree gchar *tmp_file = NULL;
   g_autoptr(CURL) curl = NULL;
   CURLcode r;
   FILE *fp = NULL;
   g_autoptr(GError) error = NULL;
   const DownloadData *data = task_data;

   g_return_if_fail(data != NULL);
   g_return_if_fail(data->target != NULL);
   g_return_if_fail(data->url != NULL);

   tmp_file = g_strdup_printf("%s.part", data->target);

   curl = curl_easy_init();
   if (curl == NULL) {
      g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Libcurl failed to initialize");
      return g_task_return_error(task, g_steal_pointer(&error));
   }

   fp = fopen(tmp_file, "wb");
   if (fp == NULL) {
      g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "Failed opening the temporary file %s", tmp_file);
      return g_task_return_error(task, g_steal_pointer(&error));
   }

   curl_easy_setopt(curl, CURLOPT_URL, data->url);
   curl_easy_setopt(curl, CURLOPT_NETRC, CURL_NETRC_OPTIONAL);
   curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
   curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
   curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
   /* We don't have to be too aggressive with the timeout because the download
    * is done out of band and we are not blocking anything in the meantime. */
   curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);

   if (data->proxy != NULL)
      curl_easy_setopt(curl, CURLOPT_PROXY, data->proxy);

   r = curl_easy_perform(curl);
   fclose(fp);

   if (r != CURLE_OK) {
      g_unlink(tmp_file);
      g_set_error(&error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                  "The download from '%s' failed", data->url);
      return g_task_return_error(task, g_steal_pointer(&error));
   }

   if (g_rename(tmp_file, data->target) != 0) {
      g_unlink(tmp_file);
      g_set_error(&error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                  "Failed to move the temporary file to '%s'", data->target);
      return g_task_return_error(task, g_steal_pointer(&error));
   }

   g_task_return_boolean(task, TRUE);
}
