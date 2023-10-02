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

#pragma once

#include <stdio.h>
#include <unistd.h>

/**
 * AuUpdateStatus:
 * @AU_UPDATE_STATUS_IDLE: The update has not been launched yet
 * @AU_UPDATE_STATUS_IN_PROGRESS: The update is currently being applied
 * @AU_UPDATE_STATUS_PAUSED: The update has been paused
 * @AU_UPDATE_STATUS_SUCCESSFUL: The update process successfully completed
 * @AU_UPDATE_STATUS_FAILED: An Error occurred during the update
 * @AU_UPDATE_STATUS_CANCELLED: A special case of FAILED where the update attempt
 *  has been cancelled
 */
typedef enum {
   AU_UPDATE_STATUS_IDLE = 0,
   AU_UPDATE_STATUS_IN_PROGRESS = 1,
   AU_UPDATE_STATUS_PAUSED = 2,
   AU_UPDATE_STATUS_SUCCESSFUL = 3,
   AU_UPDATE_STATUS_FAILED = 4,
   AU_UPDATE_STATUS_CANCELLED = 5,
} AuUpdateStatus;

/**
 * AuUpdateType:
 * @AU_UPDATE_TYPE_MINOR: A minor update
 * @AU_UPDATE_TYPE_MAJOR: A major update
 */
typedef enum {
   AU_UPDATE_TYPE_MINOR = 0,
   AU_UPDATE_TYPE_MAJOR = 1,
} AuUpdateType;

extern guint ATOMUPD_VERSION;

extern const gchar *AU_DEFAULT_CONFIG;
extern const gchar *AU_DEFAULT_MANIFEST;
extern const gchar *AU_DEFAULT_UPDATE_JSON;
extern const gchar *AU_DEFAULT_BRANCH_PATH;

extern const gchar *AU_REBOOT_FOR_UPDATE;

gchar *_au_get_host_from_url(const gchar *url);

gboolean _au_ensure_urls_in_netrc(const gchar *netrc_path,
                                  const GList *urls,
                                  const gchar *username,
                                  const gchar *password,
                                  GError **error);

gboolean _au_ensure_url_in_desync_conf(const gchar *desync_conf_path,
                                       const gchar *url,
                                       const gchar *auth_encoded,
                                       GError **error);

gboolean au_throw_error(GError **error,
                        const char *format, ...) G_GNUC_PRINTF(2, 3);
