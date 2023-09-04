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

#include "atomupd-daemon/au-atomupd1-impl.h"

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
   const gchar *description;
   const gchar *config;
   const gchar *username;
   const gchar *password;
   const gchar *auth_encoded;
} ConfigAuthTest;

static const ConfigAuthTest config_auth_tests[] = {
   {
      .description = "Test config with authentication",
      .config = "[Server]\n"
                "QueryUrl = https://example.com\n"
                "Username = foo\n"
                "Password = hunter2\n"
                "Variants = steamdeck-test",
      .username = "foo",
      .password = "hunter2",
      .auth_encoded = "Basic Zm9vOmh1bnRlcjI=",
   },

   {
      .description = "Test config with additional sections",
      .config = "[Server]\n"
                "QueryUrl = https://example.com\n"
                "Username = foo\n"
                "Password = hunter2\n"
                "Variants = steamdeck-test\n"
                "[Host]\n"
                "Username = unrelated_thing",
      .username = "foo",
      .password = "hunter2",
      .auth_encoded = "Basic Zm9vOmh1bnRlcjI=",
   },

   {
      .description = "Test config with missing password",
      .config = "[Server]\n"
                "QueryUrl = https://example.com\n"
                "Username = foo\n",
   },

   {
      .description = "Test config without authentication",
      .config = "[Server]\n"
                "QueryUrl = https://example.com\n",
   },
};

static void
test_config_auth(Fixture *f, gconstpointer context)
{
   for (gsize i = 0; i < G_N_ELEMENTS(config_auth_tests); i++) {
      const ConfigAuthTest *test = &config_auth_tests[i];
      g_autoptr(GKeyFile) key_file = NULL;
      g_autofree gchar *username = NULL;
      g_autofree gchar *password = NULL;
      g_autofree gchar *auth_encoded = NULL;
      g_autoptr(GError) error = NULL;
      gboolean result;

      key_file = g_key_file_new();

      g_key_file_load_from_data(key_file, test->config, -1, G_KEY_FILE_NONE, &error);
      g_assert_no_error(error);

      result =
         _au_get_http_auth_from_config(key_file, &username, &password, &auth_encoded);

      if (test->username == NULL)
         g_assert_false(result);
      else
         g_assert_true(result);

      g_assert_cmpstr(username, ==, test->username);
      g_assert_cmpstr(password, ==, test->password);
      g_assert_cmpstr(auth_encoded, ==, test->auth_encoded);
   }
}

typedef struct {
   const gchar *buildid;
   const gint64 date;
   const gint64 increment;
   gboolean valid;
} BuildidCheckTest;

static const BuildidCheckTest buildid_check_tests[] = {
   {
      .buildid = "20230831.1",
      .date = 20230831,
      .increment = 1,
      .valid = TRUE,
   },

   {
      .buildid = "23001231.1000",
      .date = 23001231,
      .increment = 1000,
      .valid = TRUE,
   },

   {
      .buildid = "19700101",
      .date = 19700101,
      .valid = TRUE,
   },

   { .buildid = "20230832.10" },

   { .buildid = "20231331.1" },

   { .buildid = NULL },

   { .buildid = "" },

   { .buildid = " " },

   { .buildid = "20230831.1b" },

   { .buildid = "2023083a.1" },

   { .buildid = "202308311" },

   { .buildid = "20230831.-1" },

   { .buildid = "20230831.1.2" },

   { .buildid = "2023.100" },
};

static void
test_buildid_check(Fixture *f, gconstpointer context)
{
   for (gsize i = 0; i < G_N_ELEMENTS(buildid_check_tests); i++) {
      const BuildidCheckTest *test = &buildid_check_tests[i];
      g_autoptr(GError) error = NULL;
      gint64 date = 0;
      gint64 inc = 0;
      gboolean result;

      result = _is_buildid_valid(test->buildid, &date, &inc, &error);
      g_assert_true(result == test->valid);

      if (test->valid)
         g_assert_no_error(error);
      else
         g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);

      g_assert_cmpint(date, ==, test->date);
      g_assert_cmpint(inc, ==, test->increment);
   }
}

int
main(int argc, char **argv)
{
   g_test_init(&argc, &argv, NULL);

#define test_add(_name, _test) g_test_add(_name, Fixture, argv[0], setup, _test, teardown)

   test_add("/atomupd1/config_auth", test_config_auth);
   test_add("/atomupd1/buildid_check", test_buildid_check);

   return g_test_run();
}
