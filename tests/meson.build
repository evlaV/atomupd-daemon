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

if get_option('using_valgrind')
  # If we want to use valgrind we should not override the default PATH values,
  # otherwise valgrind could fail to start
  test_env.prepend('PATH', join_paths(meson.current_build_dir(), '..', 'atomupd-daemon'))
  test_env.prepend('PATH', meson.current_build_dir())

  test_env.set('AU_TEST_VALGRIND', '1')
else
  # Override PATH to ensure that we always use the mock steamos-atomupd-client
  test_env.set('PATH', join_paths(meson.current_build_dir(), '..', 'atomupd-daemon'))
  test_env.prepend('PATH', meson.current_build_dir())
endif

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

foreach test_name : ['au-atomupd1-impl', 'impl', 'manager', 'utils']
  exe = executable(
    'test-' + test_name,
    sources : [test_name + '.c', 'fixture.c', 'mock-defines.h', 'services.c', 'tests-utils.c', atomupd1],
    dependencies : [glib, gio, gio_unix, json_glib, libcurl, polkit_gobject, atomupd1_impl_dep],
    include_directories : project_include_dirs,
    install : get_option('install_tests'),
    install_dir : tests_dir,
  )

  if get_option('using_valgrind')
    # Increase the timeout because valgrind considerably slows down the execution time
    # Empirically, a 5x the normal timeout should be enough, but we triple that to
    # account for the very slow mock polkit start time
    test_timeout = 2250
  else
    test_timeout = 150
  endif

  test(test_name, exe, env : test_env, timeout : test_timeout, is_parallel: false)
endforeach

install_subdir('data', install_dir : tests_dir)
