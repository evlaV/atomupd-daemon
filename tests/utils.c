/*
 * Copyright © 2022-2023 Collabora Ltd.
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
#include <libelf.h>
#include <string.h>
#include <unistd.h>

#include <gio/gio.h>
#include <glib-unix.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "atomupd-daemon/utils.h"

typedef struct {
   int unused;
} Fixture;

typedef struct {
   int unused;
} Config;

static void
setup(Fixture *f, gconstpointer context)
{
   G_GNUC_UNUSED const Config *config = context;
}

static void
teardown(Fixture *f, gconstpointer context)
{
   G_GNUC_UNUSED const Config *config = context;
}

typedef struct {
   const gchar *full_url;
   const gchar *host;
} UrlTest;

static const UrlTest url_tests[] = {
   {
      .full_url = "https://example.com",
      .host = "example.com",
   },

   {
      .full_url = "https://example.com:123",
      .host = "example.com:123",
   },

   {
      .full_url = "https://example.com:123/foo/bar",
      .host = "example.com:123",
   },

   {
      .full_url = "https://example.com/foo/bar",
      .host = "example.com",
   },

   {
      .full_url = "http://example.com/foo/bar",
      .host = "example.com",
   },

   {
      .full_url = "example.com/foo/bar",
      .host = "example.com",
   },

   {
      .full_url = "example.com",
      .host = "example.com",
   },

   {
      .full_url = "https://example/abc",
      .host = "example",
   },

   {
      .full_url = "https://example.co.uk//abc",
      .host = "example.co.uk",
   },

   {
      .full_url = "https://example.co.uk",
      .host = "example.co.uk",
   },

   {
      .full_url = "ftp://example.com",
      .host = "example.com",
   },

   {
      .full_url = "https://",
      .host = "",
   },
};

static void
test_host_from_url(Fixture *f, gconstpointer context)
{
   for (gsize i = 0; i < G_N_ELEMENTS(url_tests); i++) {
      const UrlTest *test = &url_tests[i];
      g_autofree gchar *host = NULL;

      host = _au_get_host_from_url(test->full_url);
      g_assert_cmpstr(test->host, ==, host);
   }
}

typedef struct {
   const gchar *description;
   const gchar *content;
   const gchar *urls[5];
   const gchar *username;
   const gchar *password;
   const gchar *new_content;
} NetrcTest;

static const NetrcTest netrc_tests[] = {
   {
      .description = "netrc missing the new URLs logins",
      .content = "machine packages.example.com login foo password hunter2\n"
                 "machine images.example.com login foo password hunter2\n",
      .urls = { "https://ci.example.com/", "https://ci-a.example.com/meta", NULL },
      .username = "bar",
      .password = "secret",
      .new_content = "machine packages.example.com login foo password hunter2\n"
                     "machine images.example.com login foo password hunter2\n"
                     "machine ci-a.example.com login bar password secret\n"
                     "machine ci.example.com login bar password secret\n",
   },

   {
      .description = "netrc that already has one URL login info",
      .content = "machine packages.example.com login foo password hunter2\n"
                 "machine images.example.com login foo password hunter2\n",
      .urls = { "https://packages.example.com/updates",
                "https://atomupd.example.com/meta", NULL },
      .username = "foo",
      .password = "hunter2",
      .new_content = "machine packages.example.com login foo password hunter2\n"
                     "machine images.example.com login foo password hunter2\n"
                     "machine atomupd.example.com login foo password hunter2\n",
   },

   {
      .description = "netrc that doesn't end with a newline",
      .content = "machine packages.example.com login foo password hunter2\n"
                 "machine images.example.com login foo password hunter2",
      .urls = { "https://packages.example.com/updates",
                "https://atomupd.example.com/meta", NULL },
      .username = "foo",
      .password = "hunter2",
      .new_content = "machine packages.example.com login foo password hunter2\n"
                     "machine images.example.com login foo password hunter2\n"
                     "machine atomupd.example.com login foo password hunter2\n",
   },

   {
      .description = "Empty netrc",
      .content = "",
      .urls = { "https://packages.example.com", NULL },
      .username = "foo",
      .password = "hunter3!",
      .new_content = "machine packages.example.com login foo password hunter3!\n",
   },

   {
      .description = "Missing netrc",
      .content = NULL,
      .urls = { "https://packages.example.com", "example.com", NULL },
      .username = "foo",
      .password = "hunter2",
      .new_content = "machine example.com login foo password hunter2\n"
                     "machine packages.example.com login foo password hunter2\n",
   },

   {
      .description = "netrc that is already up to date",
      .content = "machine packages.example.com login foo password hunter2\n"
                 "machine images.example.com login foo password hunter2\n",
      .urls = { "https://packages.example.com/", NULL },
      .username = "foo",
      .password = "hunter2",
      .new_content = "machine packages.example.com login foo password hunter2\n"
                     "machine images.example.com login foo password hunter2\n",
   },

   {
      .description = "Update the password for a machine",
      .content = "machine packages.example.com login foo password hunter2\n"
                 "machine images.example.com login foo password hunter2\n",
      .urls = { "https://packages.example.com/", NULL },
      .username = "foo",
      .password = "HUNTER2",
      .new_content = "machine packages.example.com login foo password HUNTER2\n"
                     "machine images.example.com login foo password hunter2\n",
   },
};

// TODO add description for netrc tests

static void
test_netrc_update(Fixture *f, gconstpointer context)
{
   for (gsize i = 0; i < G_N_ELEMENTS(netrc_tests); i++) {
      const NetrcTest *test = &netrc_tests[i];
      g_autofree gchar *tmp_file = NULL;
      g_autofree char *tmp_content = NULL;
      g_autoptr(GList) urls = NULL;
      int fd;
      gsize j;
      gboolean result;
      g_autoptr(GError) error = NULL;

      fd = g_file_open_tmp("netrc-XXXXXX", &tmp_file, &error);
      g_assert_no_error(error);
      g_assert_cmpint(fd, !=, -1);
      close(fd);

      if (test->content != NULL) {
         g_file_set_contents(tmp_file, test->content, -1, &error);
         g_assert_no_error(error);
      } else {
         g_unlink(tmp_file);
      }

      for (j = 0; test->urls[j] != NULL; j++) {
         urls = g_list_append(urls, (gchar *)test->urls[j]);
      }

      result =
         _au_ensure_urls_in_netrc(tmp_file, urls, test->username, test->password, &error);
      g_assert_no_error(error);
      g_assert_true(result);

      g_file_get_contents(tmp_file, &tmp_content, NULL, &error);
      g_assert_no_error(error);
      g_assert_cmpstr(tmp_content, ==, test->new_content);

      g_unlink(tmp_file);
   }
}

typedef struct {
   const gchar *description;
   const gchar *content;
   const gchar *url;
   const gchar *auth_encoded;
   const gchar *new_content;
} DesyncConfTest;

static const DesyncConfTest desync_conf_tests[] = {
   {
      .description = "Add new URL",
      .content = "{\n"
                 "  \"store-options\": {\n"
                 "    \"https://images.example.com/*/*/*/\": {\n"
                 "      \"http-auth\": \"Basic abcabc==\",\n"
                 "      \"error-retry-base-interval\": 1000000000\n"
                 "    }\n"
                 "  }\n"
                 "}\n",
      .url = "https://ci.example.com/",
      .auth_encoded = "Basic foobar==",
      .new_content = "{\n"
                     "  \"store-options\" : {\n"
                     "    \"https://images.example.com/*/*/*/\" : {\n"
                     "      \"http-auth\" : \"Basic abcabc==\",\n"
                     "      \"error-retry-base-interval\" : 1000000000\n"
                     "    },\n"
                     "    \"https://ci.example.com/*/*/*/\" : {\n"
                     "      \"http-auth\" : \"Basic foobar==\",\n"
                     "      \"error-retry-base-interval\" : 1000000000\n"
                     "    }\n"
                     "  }\n"
                     "}",
   },

   {
      .description = "The URL is already in the Desync config",
      .content = "{\n"
                 "  \"store-options\": {\n"
                 "    \"https://images.example.com/*/*/*/\": {\n"
                 "      \"http-auth\": \"Basic abcabc==\",\n"
                 "      \"error-retry-base-interval\": 1000000000\n"
                 "    }\n"
                 "  }\n"
                 "}\n",
      .url = "https://images.example.com/",
      .auth_encoded = "Basic foobar==",
      .new_content = "{\n"
                     "  \"store-options\" : {\n"
                     "    \"https://images.example.com/*/*/*/\" : {\n"
                     "      \"http-auth\" : \"Basic foobar==\",\n"
                     "      \"error-retry-base-interval\" : 1000000000\n"
                     "    }\n"
                     "  }\n"
                     "}",
   },

   {
      .description = "Test URL without a trailing slash",
      .content = "{\n"
                 "  \"store-options\" : {\n"
                 "    \"ftp://example.com/*/*/*/\" : {\n"
                 "      \"http-auth\" : \"Basic abcabc==\",\n"
                 "      \"error-retry-base-interval\" : 1000000000\n"
                 "    }\n"
                 "  }\n"
                 "}",
      .url = "ftp://example.com",
      .auth_encoded = "Basic abcabc==",
      .new_content = "{\n"
                     "  \"store-options\" : {\n"
                     "    \"ftp://example.com/*/*/*/\" : {\n"
                     "      \"http-auth\" : \"Basic abcabc==\",\n"
                     "      \"error-retry-base-interval\" : 1000000000\n"
                     "    }\n"
                     "  }\n"
                     "}",
   },

   {
      .description = "Test missing Desync config",
      .url = "https://ci.example.com/",
      .auth_encoded = "Basic aabbccdd==",
      .new_content = "{\n"
                     "  \"store-options\" : {\n"
                     "    \"https://ci.example.com/*/*/*/\" : {\n"
                     "      \"http-auth\" : \"Basic aabbccdd==\",\n"
                     "      \"error-retry-base-interval\" : 1000000000\n"
                     "    }\n"
                     "  }\n"
                     "}",
   },
};

static void
test_desync_conf_update(Fixture *f, gconstpointer context)
{
   for (gsize i = 0; i < G_N_ELEMENTS(desync_conf_tests); i++) {
      const DesyncConfTest *test = &desync_conf_tests[i];
      g_autofree gchar *tmp_file = NULL;
      g_autofree char *tmp_content = NULL;
      int fd;
      gboolean result;
      g_autoptr(GError) error = NULL;

      fd = g_file_open_tmp("desync-conf-XXXXXX", &tmp_file, &error);
      g_assert_no_error(error);
      g_assert_cmpint(fd, !=, -1);
      close(fd);

      if (test->content != NULL) {
         g_file_set_contents(tmp_file, test->content, -1, &error);
         g_assert_no_error(error);
      } else {
         g_unlink(tmp_file);
      }

      result =
         _au_ensure_url_in_desync_conf(tmp_file, test->url, test->auth_encoded, &error);
      g_assert_no_error(error);
      g_assert_true(result);

      g_file_get_contents(tmp_file, &tmp_content, NULL, &error);
      g_assert_no_error(error);
      g_assert_cmpstr(tmp_content, ==, test->new_content);

      g_unlink(tmp_file);
   }
}

int
main(int argc, char **argv)
{
   g_test_init(&argc, &argv, NULL);

#define test_add(_name, _test) g_test_add(_name, Fixture, argv[0], setup, _test, teardown)

   test_add("/utils/host_from_url", test_host_from_url);
   test_add("/utils/netrc_update", test_netrc_update);
   test_add("/utils/desync_conf_update", test_desync_conf_update);

   return g_test_run();
}
