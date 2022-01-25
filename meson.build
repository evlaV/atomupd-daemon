# Copyright © 2021 Collabora Ltd.
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

project(
  'atomupd-daemon', 'c',
  version : '0.20211201.0',
  default_options: [
    'c_std=c99',
    'warning_level=2',
  ],
  meson_version : '>=0.49.0',
)

api_major = '0'
abi_major = '0'
abi_minor = '20211201.0'

version = meson.project_version()

gnome = import('gnome')

warning_flags = [
    '-Warray-bounds',
    '-Wcast-align',
    '-Wdouble-promotion',
    '-Wduplicated-branches',
    '-Wduplicated-cond',
    '-Wformat-nonliteral',
    '-Wformat-security',
    '-Wformat=2',
    '-Winit-self',
    '-Wlogical-op',
    '-Wmissing-declarations',
    '-Wmissing-format-attribute',
    '-Wmissing-include-dirs',
    '-Wmissing-noreturn',
    '-Wnull-dereference',
    '-Wpacked',
    '-Wpointer-arith',
    '-Wredundant-decls',
    '-Wrestrict',
    '-Wreturn-type',
    '-Wshadow',
    '-Wsign-compare',
    '-Wstrict-aliasing',
    '-Wswitch-default',
    '-Wswitch-enum',
    '-Wundef',
    '-Wunused-but-set-variable',
    '-Wwrite-strings',
]
warning_cflags = warning_flags + [
    '-Wimplicit-function-declaration',
    '-Wjump-misses-init',
    '-Wmissing-prototypes',
    '-Wnested-externs',
    '-Wold-style-definition',
    '-Wstrict-prototypes',
]

no_warning_flags = [
    'cast-align',
    'inline',
    'missing-field-initializers',
    'pedantic',
    'unused-local-typedefs',
    'unused-parameter',
    'variadic-macros',
]
no_warning_cflags = no_warning_flags + [
    'declaration-after-statement',
]

c_compiler = meson.get_compiler('c')
supported_warning_cflags = c_compiler.get_supported_arguments(warning_cflags)
add_project_arguments(supported_warning_cflags, language : 'c')

foreach flag : no_warning_cflags
  supported_no_warning_cflags = c_compiler.get_supported_arguments([
    '-Wno-error=' + flag,
    '-Wno-' + flag,
  ])
  add_global_arguments(supported_no_warning_cflags, language : 'c')
endforeach


conf_data = configuration_data()
conf_data.set_quoted('VERSION', version)
conf_data.set('project_name', meson.project_name())

configure_file(
  input : 'config.h.in',
  output : '_atomupd-daemon-config.h',
  configuration : conf_data,
)

add_project_arguments(
  ['-include', '_atomupd-daemon-config.h'],
  language : 'c',
)

glib = dependency(
  'glib-2.0',
  version : '>= 2.70',
)
gio = dependency(
  'gio-2.0',
  version : '>= 2.70',
)
gio_unix = dependency(
  'gio-unix-2.0',
  version : '>= 2.70',
)

subdir('atomupd-daemon')

#subdir('tests')
