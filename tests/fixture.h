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

#pragma once

#include <glib.h>

typedef struct {
   gchar *srcdir;
   gchar *builddir;
   gchar *desync_conf_path;
   gchar *manifest_path;
   gchar *conf_dir;
   gchar *preferences_path;
   gchar *remote_info_path;
   gchar *updates_json;
   /* Text file where we store the mock RAUC service pid */
   gchar *rauc_pid_path;
   GStrv test_envp;
   GPid polkit_pid;
} Fixture;

void
au_tests_setup(Fixture *f, gconstpointer context);
void
au_tests_teardown(Fixture *f, gconstpointer context);
void
mock_polkit_set_allowed(const gchar **allowed, gsize n_elements);
