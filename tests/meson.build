# Copyright © 2022 Collabora Ltd.
#
# SPDX-License-Identifier: MIT
#
# Permission is hereby granted, free of charge, to any person obtaining
# a copy of this software and associated documentation files (the
# "Software"), to deal in the Software without restriction, including
# without limitation the rights to use, copy, modify, merge, publish,
# distribute, sublicense, and/or sell copies of the Software, and to
# permit persons to whom the Software is furnished to do so, subject to
# the following conditions:
#
# The above copyright notice and this permission notice shall be included
# in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
# CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
# TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
# SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

test_env = environment()
test_env.set('G_TEST_SRCDIR', meson.current_source_dir())
test_env.set('G_TEST_BUILDDIR', meson.current_build_dir())
# Override PATH to ensure that we always use the mock steamos-atomupd-client
test_env.set('PATH', join_paths(meson.current_build_dir(), '..', 'atomupd-daemon'))
test_env.prepend('PATH', meson.current_build_dir())

tests_dir = join_paths(
  get_option('prefix'),
  get_option('libexecdir'),
  'installed-tests',
  meson.project_name() + '-' + api_major,
)

executable(
  'steamos-atomupd-client',
  'mock-steamos-atomupd-client.c',
  dependencies : [glib, gio, json_glib],
  install: true,
  install_dir: tests_dir
)

executable(
  'systemctl',
  'mock-systemctl-show.c',
  dependencies : [glib],
  install: true,
  install_dir: tests_dir
)

executable(
  'mock-rauc-service',
  'mock-rauc-service.c',
  dependencies : [glib],
  install: true,
  install_dir: tests_dir
)

exe = executable(
  'test-impl',
  'impl.c',
  dependencies : [glib, gio, json_glib],
  include_directories : project_include_dirs,
  install : get_option('install_tests'),
  install_dir : tests_dir,
)

test('test-impl', exe, env : test_env, timeout : 60)

install_subdir('data', install_dir : tests_dir)
