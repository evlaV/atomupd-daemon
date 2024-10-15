/*
 * Copyright © 2023-2024 Collabora Ltd.
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

gchar *
_au_get_host_from_url(const gchar *url)
{
   g_autoptr(GString) host = NULL;
   const gchar *separator = "://";
   const char *pointer_separator = NULL;
   const char *host_end = NULL;

   g_return_val_if_fail(url != NULL, NULL);

   host = g_string_new(url);

   pointer_separator = strstr(host->str, separator);
   if (pointer_separator != NULL)
      g_string_erase(host, 0, (pointer_separator - host->str) + strlen(separator));

   host_end = strstr(host->str, "/");
   if (host_end != NULL)
      host = g_string_truncate(host, host_end - host->str);

   return g_string_free(g_steal_pointer (&host), FALSE);
}

/**
 * _au_ensure_urls_in_netrc:
 * @netrc_path: (not nullable): Path to the netrc config file
 * @urls: (not nullable): URLs that need to be available in @netrc_path
 * @username: (not nullable): username for the HTTP auth
 * @password: (not nullable): password for the HTTP auth
 * @error: Used to raise an error on failure
 *
 * Ensures that the @urls are available in @netrc_path, with the provided
 * @username and @password. If @netrc_path points to a non-existant location,
 * a new file will be created.
 *
 * Returns: %TRUE on success
 */
gboolean
_au_ensure_urls_in_netrc(const gchar *netrc_path,
                         const GList *urls,
                         const gchar *username,
                         const gchar *password,
                         GError **error)
{
   g_autoptr(GString) updated_netrc = NULL;
   g_autoptr(GList) new_hosts = NULL;
   g_autoptr(GHashTable) hosts = NULL;
   g_autofree gchar *login = NULL;
   FILE *fp = NULL;
   size_t len = 0;
   g_autofree gchar *line = NULL;
   gboolean netrc_updated = FALSE;

   g_return_val_if_fail(netrc_path != NULL, FALSE);
   g_return_val_if_fail(username != NULL, FALSE);
   g_return_val_if_fail(password != NULL, FALSE);
   g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

   login = g_strdup_printf("login %s password %s", username, password);

   hosts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

   for (const GList *u = urls; u != NULL; u = u->next) {
      g_hash_table_add(hosts, _au_get_host_from_url(u->data));
   }

   fp = fopen(netrc_path, "r");

   if (fp == NULL) {
      gint saved_errno = errno;

      if (errno == ENOENT) {
         g_debug ("There isn't a netrc file");
      } else {
         return au_throw_error(error, "Failed to open the netrc file: %s", g_strerror(saved_errno));
      }
   }

   updated_netrc = g_string_new("");

   if (fp != NULL) {
      ssize_t chars;
      while ((chars = getline(&line, &len, fp)) != -1) {
         g_auto(GStrv) parts = NULL;

         if (line[chars - 1] == '\n')
            line[chars - 1] = '\0';

         if (line[0] == '\0')
            continue;

         g_strstrip(line);
         parts = g_strsplit(line, " ", 3);

         if (!g_str_equal(parts[0], "machine") || parts[1] == NULL || parts[2] == NULL) {
            g_warning("netrc is possibly malformed, unexpected line: %s", line);
            continue;
         }

         if (g_hash_table_contains(hosts, parts[1])) {
            g_hash_table_remove(hosts, parts[1]);

            if (!g_str_equal(parts[2], login)) {
               g_debug("The login information for %s has been updated", parts[1]);
               netrc_updated = TRUE;
               g_string_append_printf(updated_netrc, "machine %s %s\n",
                                    parts[1], login);
               continue;
            }
         }

         /* This entry was either not edited or only available in the netrc,
         * keeping it as-is */
         g_string_append(updated_netrc, line);
         g_string_append_c(updated_netrc, '\n');
      }
   }

   /* Sort the hosts to get consistent values in output */
   new_hosts = g_list_sort (g_hash_table_get_keys (hosts), (GCompareFunc) strcmp);
   for (const GList *u = new_hosts; u != NULL; u = u->next) {
      netrc_updated = TRUE;
      g_string_append_printf(updated_netrc, "machine %s %s\n", (gchar *)u->data, login);
   }


   if (netrc_updated) {
      g_debug("Updating the netrc file...");
      if (!g_file_set_contents_full(netrc_path, updated_netrc->str, updated_netrc->len,
                                    G_FILE_SET_CONTENTS_CONSISTENT, 0600, error)) {
         g_clear_pointer (&fp, fclose);
         return FALSE;
      }
   }

   g_clear_pointer (&fp, fclose);
   return TRUE;
}

/*
 * _au_ensure_url_in_desync_conf:
 * @desync_conf_path: (not nullable): Path to the Desync config file
 * @url: (not nullable): URL that needs to be available in @desync_conf_path
 * @auth_encoded: (not nullable): HTTP authentication for @url
 * @error: Used to raise an error on failure
 *
 * Ensures that @url is available in @desync_conf_path, with the http-auth of
 * @auth_encoded. If @desync_conf_path points to a non-existant location, a new
 * file will be created.
 *
 * Returns: %TRUE on success
 */
gboolean
_au_ensure_url_in_desync_conf(const gchar *desync_conf_path,
                              const gchar *url,
                              const gchar *auth_encoded,
                              GError **error)
{
   g_autoptr(JsonParser) parser = NULL;
   JsonNode *root = NULL;
   JsonObject *object = NULL;
   JsonObject *store_options = NULL;
   const gchar *store_options_literal = "store-options";
   gboolean updated = FALSE;
   g_autoptr(GString) url_entry = g_string_new (NULL);
   gsize i;

   g_return_val_if_fail(desync_conf_path != NULL, FALSE);
   g_return_val_if_fail(url != NULL, FALSE);
   g_return_val_if_fail(auth_encoded != NULL, FALSE);
   g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

   parser = json_parser_new();

   if (!g_file_test(desync_conf_path, G_FILE_TEST_EXISTS)) {
      const gchar *conf_skeleton = "{ }";

      if (!json_parser_load_from_data(parser, conf_skeleton, -1, error))
         return FALSE;
   } else {
      if (!json_parser_load_from_file(parser, desync_conf_path, error))
         return FALSE;
   }

   root = json_parser_get_root(parser);

   if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root)) {
      if (error != NULL)
         g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                      "Expected to find a JSON object in \"%s\"", desync_conf_path);
      return FALSE;
   }

   object = json_node_get_object(root);

   if (!json_object_has_member(object, store_options_literal))
      json_object_set_object_member(object, store_options_literal, json_object_new());

   store_options = json_object_get_object_member(object, store_options_literal);

   /* Use three `*` because the the first element is the image name, usually
    * "steamdeck", then the version and finally the "castr" directory.
    * We only add two `*` here, because the third will be added in the for loop. */
   g_string_printf (url_entry,
                    "%s%s%s",
                    url,
                    g_str_has_suffix (url, "/") ? "" : "/",
                    "*/*/");

   /* The server isn't too strict on the paths used. In order to cover any reasonable
    * additional sub directories that the server might add in the future, we iterate
    * a couple additional times to reach up to five `*` in the URL. */
   for (i = 0; i < 3; i++) {
      g_string_append (url_entry, "*/");

      if (json_object_has_member (store_options, url_entry->str)) {
         const gchar *old_auth = NULL;
         JsonObject *url_object = NULL;

         url_object = json_object_get_object_member(store_options, url_entry->str);

         old_auth = json_object_get_string_member_with_default(url_object, "http-auth", NULL);

         if (g_strcmp0(old_auth, auth_encoded) != 0) {
            g_debug("The auth token for %s has been updated", url_entry->str);

            json_object_set_string_member(url_object, "http-auth", auth_encoded);
            updated = TRUE;
         }
      }

      if (!json_object_has_member (store_options, url_entry->str)) {
         g_autoptr(JsonObject) url_object = json_object_new();

         json_object_set_string_member(url_object, "http-auth", auth_encoded);
         /* Set the error retry base interval to 1 second to let Desync wait a sane
         * amount of time before re-trying a failed HTTP request */
         json_object_set_int_member(url_object, "error-retry-base-interval", 1000000000);
         json_object_set_object_member(store_options, url_entry->str, g_steal_pointer(&url_object));

         updated = TRUE;
      }
   }

   if (updated) {
      g_autoptr(JsonGenerator) generator = NULL;
      g_autofree gchar *json_output = NULL;

      g_debug("Updating the Desync config file...");
      generator = json_generator_new();
      json_generator_set_root (generator, root);
      json_generator_set_pretty (generator, TRUE);
      json_output = json_generator_to_data (generator, NULL);

      if (!g_file_set_contents_full(desync_conf_path, json_output, -1,
                                    G_FILE_SET_CONTENTS_CONSISTENT, 0600, error))
         return FALSE;
   }

   return TRUE;
}

/*
 * _au_download_file:
 * @target: (not nullable): Path where to store the downloaded file
 * @url: (not nullable): URL that needs to be downloaded
 * @error: Used to raise an error on failure
 *
 * Downloads the @url to the provided @target. If @target already exists, it will be
 * replaced. During the download, the temporary file is stored at @target with the `.part`
 * suffix.
 *
 * Returns: %TRUE on success
 */
gboolean
_au_download_file(const gchar *target, const gchar *url, GError **error)
{
   g_autofree gchar *tmp_file = NULL;
   g_autoptr(CURL) curl = NULL;
   CURLcode r;
   FILE *fp = NULL;

   g_return_val_if_fail(target != NULL, FALSE);
   g_return_val_if_fail(url != NULL, FALSE);
   g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

   tmp_file = g_strdup_printf("%s.part", target);

   curl = curl_easy_init();
   if (curl == NULL)
      return au_throw_error(error, "Libcurl failed to initialize");

   fp = fopen(tmp_file, "wb");
   if (fp == NULL)
      return au_throw_error(error, "Failed opening the temporary file %s", tmp_file);

   curl_easy_setopt(curl, CURLOPT_URL, url);
   curl_easy_setopt(curl, CURLOPT_NETRC, CURL_NETRC_OPTIONAL);
   curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
   curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
   curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
   /* We are aggressive with the timeout because at the moment this is only used to
    * download very small text files. Additionally, if the download fails, it is not
    * a fatal error and we can continue regardless. */
   curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

   r = curl_easy_perform(curl);
   fclose(fp);

   if (r != CURLE_OK) {
      g_unlink(tmp_file);
      return au_throw_error(error, "The download from '%s' failed", url);
   }

   if (g_rename(tmp_file, target) != 0) {
      g_unlink(tmp_file);
      return au_throw_error(error, "Failed to move the temporary file to '%s'", target);
   }

   return TRUE;
}
