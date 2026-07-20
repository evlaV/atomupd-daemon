/*
 * Copyright © 2022-2026 Collabora Ltd.
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

/* SHA256 of "foo:hunter2" */
#define HASH_HUNTER2 "66cd33a8f743a96c03cd87cd823b561963f6fca93703dc19d3d5595086557a53"

typedef struct {
   const gchar *description;
   const gchar *config;
   const gchar *hash;
} ConfigAuthTest;

static const ConfigAuthTest config_auth_hash_tests[] = {
   {
      .description = "Test config with authentication",
      .config = "[Server]\n"
                "QueryUrl = https://example.com\n"
                "Username = foo\n"
                "Password = hunter2\n"
                "Variants = steamdeck-test",
      .hash = HASH_HUNTER2,
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
      .hash = HASH_HUNTER2,
   },

   {
      .description = "Test config with missing password",
      .config = "[Server]\n"
                "QueryUrl = https://example.com\n"
                "Username = foo\n",
   },

   {
      .description = "Test config with missing username",
      .config = "[Server]\n"
                "QueryUrl = https://example.com\n"
                "Password = hunter2\n",
   },

   {
      .description = "Test config without authentication",
      .config = "[Server]\n"
                "QueryUrl = https://example.com\n",
   },
};

static void
test_config_auth_hash(Fixture *f, gconstpointer context)
{
   for (gsize i = 0; i < G_N_ELEMENTS(config_auth_hash_tests); i++) {
      const ConfigAuthTest *test = &config_auth_hash_tests[i];
      g_autoptr(GKeyFile) key_file = NULL;
      g_autofree gchar *hash = NULL;
      g_autoptr(GError) error = NULL;

      key_file = g_key_file_new();

      g_key_file_load_from_data(key_file, test->config, -1, G_KEY_FILE_NONE, &error);
      g_assert_no_error(error);

      hash = _au_get_secret_hash_from_config(key_file);

      g_assert_cmpstr(hash, ==, test->hash);
   }
}


typedef struct {
   const gchar *description;
   const gchar *secret_hash;
   const gchar *variant;
   const gchar *data;
   /* If @expected_with_hash is %NULL, we expect the result to be equal to @data */
   const gchar *expected_with_hash;
} SecretHashDataTest;

static const SecretHashDataTest secret_hash_data_tests[] = {
   {
      .description = "meta JSON with dev variant",
      .secret_hash = HASH_HUNTER2,
      .variant = "steamdeck-test1",
      .data = "{ \
  \"minor\": { \
    \"release\": \"holo\", \
    \"candidates\": [ \
      { \
        \"image\": { \
          \"product\": \"steamos\", \
          \"release\": \"holo\", \
          \"variant\": \"steamdeck-test1\", \
          \"arch\": \"amd64\", \
          \"version\": \"3.10.0\", \
          \"buildid\": \"20300101.100\", \
          \"checkpoint\": false, \
          \"estimated_size\": 60112233 \
        }, \
        \"update_path\": \"dev/steamdeck-test1/20300101.100/foo-3.10.0.raucb\" \
      } \
    ] \
  } \
}",
      .expected_with_hash = "{ \
  \"minor\": { \
    \"release\": \"holo\", \
    \"candidates\": [ \
      { \
        \"image\": { \
          \"product\": \"steamos\", \
          \"release\": \"holo\", \
          \"variant\": \"steamdeck-test1\", \
          \"arch\": \"amd64\", \
          \"version\": \"3.10.0\", \
          \"buildid\": \"20300101.100\", \
          \"checkpoint\": false, \
          \"estimated_size\": 60112233 \
        }, \
        \"update_path\": \"dev/steamdeck-test1_" HASH_HUNTER2
                            "_DO_NOT_SHARE_URL/20300101.100/foo-3.10.0.raucb\" \
      } \
    ] \
  } \
}",
   },

   {
      .description = "meta JSON without a dev variant",
      .secret_hash = HASH_HUNTER2,
      .variant = "steamdeck-test1",
      .data = "{ \
  \"minor\": { \
    \"release\": \"holo\", \
    \"candidates\": [ \
      { \
        \"image\": { \
          \"product\": \"steamos\", \
          \"release\": \"holo\", \
          \"variant\": \"steamdeck-test1\", \
          \"arch\": \"amd64\", \
          \"version\": \"3.10.0\", \
          \"buildid\": \"20300101.100\", \
          \"checkpoint\": false, \
          \"estimated_size\": 60112233 \
        }, \
        \"update_path\": \"steamdeck-test1/20300101.100/foo-3.10.0.raucb\" \
      } \
    ] \
  } \
}",
   },

   {
      .description = "Multiple dev URLs",
      .secret_hash = HASH_HUNTER2,
      .variant = "steamdeck-test1",
      .data = "{ \
  \"minor\": { \
    \"release\": \"holo\", \
    \"candidates\": [ \
      { \
        \"update_path\": \"dev/steamdeck-test1/20300101.100/foo-3.10.0.raucb\" \
      }, \
      { \
        \"update_path\": \"dev/steamdeck-test1/20302202.200/bar-3.11.0.raucb\" \
      } \
    ] \
  } \
}",
      .expected_with_hash = "{ \
  \"minor\": { \
    \"release\": \"holo\", \
    \"candidates\": [ \
      { \
        \"update_path\": \"dev/steamdeck-test1_" HASH_HUNTER2
                            "_DO_NOT_SHARE_URL/20300101.100/foo-3.10.0.raucb\" \
      }, \
      { \
        \"update_path\": \"dev/steamdeck-test1_" HASH_HUNTER2
                            "_DO_NOT_SHARE_URL/20302202.200/bar-3.11.0.raucb\" \
      } \
    ] \
  } \
}",
   },

   {
      .description = "Direct dev image URL",
      .secret_hash = HASH_HUNTER2,
      .variant = "steamdeck-test1",
      .data = "https://example.com/dev/steamdeck-test1/steamdeck-test1-20260324.raucb",
      .expected_with_hash = "https://example.com/dev/steamdeck-test1_" HASH_HUNTER2
                            "_DO_NOT_SHARE_URL/steamdeck-test1-20260324.raucb",
   },

   {
      .description = "dev appears in other fields but not in the URL",
      .secret_hash = HASH_HUNTER2,
      .variant = "steamdeck-dev",
      .data = "{ \"description\": \"developer build\", \"update_path\": "
              "\"steamdeck-dev/foo.raucb\" }",
   },

   {
      .description = "Hash missing",
      .secret_hash = NULL,
      .variant = "steamdeck-test1",
      .data = "https://example.com/dev/steamdeck-test1/steamdeck-test1-20260324.raucb",
   },

   {
      .description = "Direct dev image URL that already has the secret hash",
      .secret_hash = HASH_HUNTER2,
      .variant = "steamdeck-test1",
      .data = "https://example.com/dev/steamdeck-test1_" HASH_HUNTER2
              "_DO_NOT_SHARE_URL/steamdeck-test1-20260324.raucb",
   },

   {
      .description = "Direct dev image URL for an unexpected variant",
      .secret_hash = HASH_HUNTER2,
      .variant = "steamdeck-test1",
      .data = "https://example.com/dev/steamdeck-test2/steamdeck-test1-20260324.raucb",
   },
};

static void
test_include_secret_hash_data(Fixture *f, gconstpointer context)
{
   for (gsize i = 0; i < G_N_ELEMENTS(secret_hash_data_tests); i++) {
      const SecretHashDataTest *test = &secret_hash_data_tests[i];
      g_autofree gchar *result = NULL;

      g_test_message("[%zu] %s", i, test->description);

      result = _au_include_secret_hash_data(test->secret_hash, test->variant, test->data);
      if (test->expected_with_hash)
         g_assert_cmpstr(result, ==, test->expected_with_hash);
      else
         g_assert_cmpstr(result, ==, test->data);
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

typedef struct {
   const gchar *description;
   const gchar *url;
   const gchar *expected;
} RedactUrlTest;

static const RedactUrlTest redact_url_tests[] = {
   {
      .description = "URL with secret hash",
      .url = "https://example.com/dev/steamdeck-test1_" HASH_HUNTER2
             "_DO_NOT_SHARE_URL/steamdeck-test1-20260324.raucb",
      .expected = "https://example.com/dev/REDACTED/steamdeck-test1-20260324.raucb",
   },

   {
      .description = "URL without secret hash",
      .url = "https://example.com/steamdeck-test1/steamdeck-test1-20260324.raucb",
      .expected = "https://example.com/steamdeck-test1/steamdeck-test1-20260324.raucb",
   },

   {
      .description = "URL with only the notice suffix",
      .url = "https://example.com/dev/variant_DO_NOT_SHARE_URL/foo.raucb",
      .expected = "https://example.com/dev/REDACTED/foo.raucb",
   },

   {
      .description = "Unexpected empty URL",
      .url = "",
      .expected = "",
   },
};

static void
test_redact_url(Fixture *f, gconstpointer context)
{
   for (gsize i = 0; i < G_N_ELEMENTS(redact_url_tests); i++) {
      const RedactUrlTest *test = &redact_url_tests[i];
      g_autofree gchar *result = NULL;
      g_autoptr(GError) error = NULL;

      g_test_message("[%zu] %s", i, test->description);

      result = _au_redact_url(test->url, &error);
      g_assert_no_error(error);
      g_assert_cmpstr(result, ==, test->expected);
   }
}

static void
test_compute_secret_hash(Fixture *f, gconstpointer context)
{
   g_autofree gchar *hash = NULL;

   hash = _au_compute_secret_hash("foo", "hunter2");

   g_assert_cmpstr(hash, ==, HASH_HUNTER2);
}

int
main(int argc, char **argv)
{
   g_test_init(&argc, &argv, NULL);

#define test_add(_name, _test) g_test_add(_name, Fixture, argv[0], setup, _test, teardown)

   test_add("/atomupd1/config_auth_hash", test_config_auth_hash);
   test_add("/atomupd1/include_secret_hash_data", test_include_secret_hash_data);
   test_add("/atomupd1/buildid_check", test_buildid_check);
   test_add("/atomupd1/redact_url", test_redact_url);
   test_add("/atomupd1/compute_secret_hash", test_compute_secret_hash);

   return g_test_run();
}
