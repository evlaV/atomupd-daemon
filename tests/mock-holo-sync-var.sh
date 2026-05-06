#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright © 2026 Collabora Ltd.

mode="pass"
if [ -n "$G_TEST_HOLO_SYNC_VAR_MODE_FILE" ]; then
    mode=$(/usr/bin/cat "$G_TEST_HOLO_SYNC_VAR_MODE_FILE")
fi

if [ "$mode" = "progress" ]; then
    /usr/bin/sleep 1
fi

if [ "$mode" = "fail" ]; then
    exit 1
fi

if [ "$mode" = "no-etc" ]; then
    printf 'All user /etc changes in /var would be preserved after an update.\n' >&2
fi

if [ "$mode" = "pass" ]; then
    printf 'The following /etc files in /var would not be preserved after an update:\n/etc/foo.conf\n/etc/bar.conf\n' >&2
fi

exit 0
