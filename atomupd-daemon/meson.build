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

atomupd1 = gnome.gdbus_codegen(
  'atomupd1',
  sources : 'com.steampowered.Atomupd1.xml',
  interface_prefix : 'com.steampowered.',
  namespace : 'Ad',
)

install_data(
  'com.steampowered.Atomupd1.conf',
  install_dir : join_paths(get_option('datadir'), 'dbus-1/system.d')
)

executable(
  'atomupd-daemon',
  sources: [
    'atomupd-daemon.c',
    'main.c',
    atomupd1,
  ],
  dependencies : [
    glib,
    gio,
    gio_unix,
  ],
  install : true,
)
