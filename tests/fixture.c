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

#include <gio/gio.h>
#include <glib-unix.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "fixture.h"

void
au_tests_setup(Fixture *f, gconstpointer context)
{
   int fd;
   const char *argv0 = context;
   g_autoptr(GError) error = NULL;

   f->srcdir = g_strdup(g_getenv("G_TEST_SRCDIR"));
   f->builddir = g_strdup(g_getenv("G_TEST_BUILDDIR"));

   if (f->srcdir == NULL)
      f->srcdir = g_path_get_dirname(argv0);

   if (f->builddir == NULL)
      f->builddir = g_path_get_dirname(argv0);

   f->manifest_path = g_build_filename(f->srcdir, "data", "manifest.json", NULL);
   f->conf_path = g_build_filename(f->srcdir, "data", "client.conf", NULL);

   fd = g_file_open_tmp("steamos-atomupd-XXXXXX.json", &f->updates_json, &error);
   g_assert_no_error(error);
   close(fd);
   /* Start with the update JSON not available */
   g_assert_cmpint(g_unlink(f->updates_json), ==, 0);

   fd = g_file_open_tmp("rauc-pid-XXXXXX", &f->rauc_pid_path, &error);
   g_assert_no_error(error);
   close(fd);
   /* Start with the mock RAUC service stopped */
   g_assert_cmpint(g_unlink(f->rauc_pid_path), ==, 0);

   f->test_envp = g_get_environ();
   f->test_envp =
      g_environ_setenv(f->test_envp, "AU_UPDATES_JSON_FILE", f->updates_json, TRUE);
   f->test_envp = g_environ_setenv(f->test_envp, "G_TEST_MOCK_RAUC_SERVICE_PID",
                                   f->rauc_pid_path, TRUE);
}

void
au_tests_teardown(Fixture *f, gconstpointer context)
{
   g_free(f->srcdir);
   g_free(f->builddir);
   g_free(f->manifest_path);
   g_free(f->conf_path);
   g_strfreev(f->test_envp);

   g_unlink(f->updates_json);
   g_free(f->updates_json);

   g_unlink(f->rauc_pid_path);
   g_free(f->rauc_pid_path);
}
