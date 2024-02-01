# Copyright © 2021-2023 Collabora Ltd.
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

atomupd1 = gnome.gdbus_codegen(
  'au-atomupd1',
  sources : 'com.steampowered.Atomupd1.xml',
  interface_prefix : 'com.steampowered.',
  namespace : 'Au',
)

enums = gnome.mkenums_simple(
  'enums',
  sources: 'utils.h',
)

conf_data = configuration_data()
conf_data.set('libexecdir', get_option('prefix') / get_option('libexecdir'))
conf_data.set_quoted('authorized_user', get_option('authorized_user'))

systemdsystemunitdir = get_option('systemdsystemunitdir')
if systemdsystemunitdir == ''
  systemd_dep = dependency('systemd')
  systemdsystemunitdir = systemd_dep.get_variable('systemdsystemunitdir')
endif

configure_file(
  input : 'com.steampowered.Atomupd1.service.in',
  output : 'com.steampowered.Atomupd1.service',
  configuration : conf_data,
  install_dir : join_paths(get_option('prefix'), get_option('datadir'), 'dbus-1/system-services')
)

configure_file(
  input : 'atomupd.service.in',
  output : 'atomupd.service',
  configuration : conf_data,
  install_dir : systemdsystemunitdir,
)

configure_file(
  input : 'com.steampowered.Atomupd1.rules.in',
  output : 'com.steampowered.Atomupd1.rules',
  configuration : conf_data,
  install_dir : join_paths(get_option('prefix'), get_option('datadir'), 'polkit-1/rules.d')
)

install_data(
  'com.steampowered.Atomupd1.conf',
  install_dir : join_paths(get_option('prefix'), get_option('datadir'), 'dbus-1/system.d')
)

install_data(
  'com.steampowered.Atomupd1.policy',
  install_dir : join_paths(get_option('prefix'), get_option('datadir'), 'polkit-1/actions')
)

atomupd1_impl_dep = declare_dependency(
  sources : ['utils.c', 'au-atomupd1-impl.c'],
)

executable(
  'atomupd-daemon',
  sources: ['main.c', atomupd1],
  dependencies : [glib, gio, gio_unix, json_glib, polkit_gobject, atomupd1_impl_dep],
  install : true,
  install_dir : get_option('prefix') / get_option('libexecdir'),
)

executable(
  'atomupd-manager',
  sources: ['atomupd-manager.c', 'au-atomupd1-impl.c', atomupd1, enums],
  dependencies : [glib, gio, gio_unix, json_glib, polkit_gobject, atomupd1_impl_dep, systemd],
  install : true,
)
