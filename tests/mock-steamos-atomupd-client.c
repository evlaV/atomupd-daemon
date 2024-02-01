/*
 * Copyright © 2022 Collabora Ltd.
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

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sysexits.h>

#include <glib.h>
#include <json-glib/json-glib.h>

#include "mock-defines.h"

static volatile sig_atomic_t stopped = FALSE;

static gchar *opt_config = NULL;
static gchar *opt_manifest = NULL;
static gchar *opt_update_file = NULL;
static gchar *opt_update_version = NULL;
static gchar *opt_variant = NULL;
static gchar *opt_branch = NULL;
static gboolean opt_query_only = FALSE;
static gboolean opt_estimate_download_size = FALSE;
static gboolean opt_debug = FALSE;

static GOptionEntry options[] = {
   { "config", '\0', G_OPTION_FLAG_NONE, G_OPTION_ARG_FILENAME, &opt_config, NULL,
     "PATH" },
   { "manifest-file", '\0', G_OPTION_FLAG_NONE, G_OPTION_ARG_FILENAME, &opt_manifest,
     NULL, "PATH" },
   { "update-file", '\0', G_OPTION_FLAG_NONE, G_OPTION_ARG_FILENAME, &opt_update_file,
     NULL, "PATH" },
   { "update-version", '\0', G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, &opt_update_version,
     NULL, NULL },
   { "variant", '\0', G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, &opt_variant, NULL, NULL },
   { "branch", '\0', G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, &opt_branch, NULL, NULL },
   { "query-only", '\0', G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &opt_query_only, NULL,
     NULL },
   { "estimate-download-size", '\0', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE,
     &opt_estimate_download_size, NULL, NULL },
   { "debug", '\0', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &opt_debug, NULL, NULL },
   { NULL }
};

static void
sig_handler(int _)
{
   stopped = TRUE;
}

int
main(int argc, char **argv)
{
   gulong delay = 0.5 * G_USEC_PER_SEC;
   g_autoptr(GOptionContext) option_context = NULL;
   g_autoptr(GError) error = NULL;

   signal(SIGTERM, sig_handler);

   option_context = g_option_context_new("");
   g_option_context_add_main_entries(option_context, options, NULL);

   if (!g_option_context_parse(option_context, &argc, &argv, &error)) {
      return EX_USAGE;
   }

   if (opt_query_only) {
      g_autofree gchar *update_json = NULL;
      const gchar *update_json_path = g_getenv("G_TEST_UPDATE_JSON");

      if (update_json_path == NULL) {
         printf("{}");
         return EXIT_SUCCESS;
      }

      if (!g_file_get_contents(update_json_path, &update_json, NULL, &error)) {
         g_warning("Failed to parse the update json file \"%s\": %s", update_json_path,
                   error->message);
         return EXIT_FAILURE;
      }

      printf("%s", update_json);

      return EXIT_SUCCESS;
   }

   if (opt_update_version == NULL)
      return EXIT_FAILURE;

   setbuf(stdout, NULL);

   if (g_str_equal(opt_update_version, MOCK_SUCCESS)) {
      /* Simulates an update that after 1.5 seconds successfully completes */
      printf("0.00%%\n");
      g_usleep(delay);
      printf("4.08%% 01m12s\n");
      g_usleep(delay);
      printf("54.42%% 00m13s\n");
      g_usleep(delay);
      printf("100%%\n");

      return EXIT_SUCCESS;
   } else if (g_str_equal(opt_update_version, MOCK_SLOW)) {
      /* Simulates an update that after 8 seconds successfully completes */
      printf("0.00%%\n");
      g_usleep(delay);
      printf("4.08%% 01m12s\n");
      g_usleep(delay);
      printf("54.42%% 00m13s\n");
      g_usleep(7 * G_USEC_PER_SEC);
      printf("100%%\n");

      return EXIT_SUCCESS;
   } else if (g_str_equal(opt_update_version, MOCK_INFINITE)) {
      /* Simulate a very long update. To make it consistent for the testing
       * it always prints the same progress percentage and estimation. */
      while (!stopped) {
         printf("16.08%% 06m35s\n");
         g_usleep(delay);
      }
      printf("17.50%% 05m50s\n");
      return EXIT_SUCCESS;
   } else if (g_str_equal(opt_update_version, MOCK_STUCK)) {
      /* Simulate an update that takes a very long time to start.
       * To make it consistent for the testing, we never print a single
       * progress update */
      while (!stopped)
         g_usleep(delay);

      return EXIT_SUCCESS;
   }

   return EXIT_FAILURE;
}
