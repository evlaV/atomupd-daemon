/*
 * Copyright © 2021-2024 Collabora Ltd.
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
#include <unistd.h>

#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <glib-unix.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <polkit/polkit.h>

#include "au-atomupd1-impl.h"
#include "utils.h"

#include <json-glib/json-glib.h>

/* The version of this interface, exposed in the "Version" property */
guint ATOMUPD_VERSION = 6;

const gchar *AU_CONFIG = "client.conf";
const gchar *AU_DEV_CONFIG = "client-dev.conf";
const gchar *AU_REMOTE_INFO = "remote-info.conf";
const gchar *AU_DEFAULT_MANIFEST = "/etc/steamos-atomupd/manifest.json";
const gchar *AU_DEFAULT_UPDATE_JSON = "/run/atomupd-daemon/atomupd-updates.json";

/* Please keep this in sync with steamos-select-branch */
const gchar *AU_DEFAULT_BRANCH_PATH = "/var/lib/steamos-branch";

const gchar *AU_FALLBACK_CONFIG_PATH = "/usr/lib/steamos-atomupd";

const gchar *AU_USER_PREFERENCES = "/etc/steamos-atomupd/preferences.conf";

/* This file is not expected to be preserved when applying a system update.
 * It is not a problem if this happens to be preserved across updates, e.g.
 * if a user adds `/etc/steamos-atomupd/\*` to `/etc/atomic-update.conf.d/`,
 * because when atomupd-daemon starts up it always tries to replace the local
 * remote-info.conf file with the latest version from the server. */
const gchar *AU_REMOTE_INFO_PATH = "/etc/steamos-atomupd/remote-info.conf";

/* Please keep this in sync with steamos-customizations common.mk */
const gchar *AU_REBOOT_FOR_UPDATE = "/run/steamos-atomupd/reboot_for_update";

/* Please keep this in sync with steamos-customizations rauc/system.conf */
const gchar *AU_DESYNC_CONFIG_PATH = "/etc/desync/config.json";

const gchar *AU_NETRC_PATH = "/root/.netrc";

struct _AuAtomupd1Impl {
   AuAtomupd1Skeleton parent_instance;

   GDebugController *debug_controller;
   gulong debug_controller_id;
   PolkitAuthority *authority;
   GPid install_pid;
   guint install_event_source;
   gchar *config_path;
   gchar *config_directory;
   gchar *manifest_path;
   GFile *updates_json_file;
   GFile *updates_json_copy;
   GDataInputStream *start_update_stdout_stream;
   gint64 buildid_date;
   gint64 buildid_increment;
};

typedef struct {
   AuAtomupd1 *object;
   GDBusMethodInvocation *invocation;
} RequestData;

typedef struct {
   RequestData *req;
   gint standard_output;
} QueryData;

typedef struct {
   const gchar *expanded;
   const gchar *contracted;
} VariantConversion;

/* This is the same contracted->expanded relation that steamos-update uses */
static const VariantConversion variant_conversions[] = {
   {
      .expanded = "steamdeck",
      .contracted = "rel",
   },
   {
      .expanded = "steamdeck-rc",
      .contracted = "rc",
   },
   {
      .expanded = "steamdeck-beta",
      .contracted = "beta",
   },
   {
      .expanded = "steamdeck-bc",
      .contracted = "bc",
   },
   {
      .expanded = "steamdeck-main",
      .contracted = "main",
   },
   {
      .expanded = "steamdeck-staging",
      .contracted = "staging",
   },
};

static void
_request_data_free(RequestData *self)
{
   if (self->invocation != NULL)
      g_dbus_method_invocation_return_error(g_steal_pointer(&self->invocation),
                                            G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                            "Request was freed without being handled");

   g_clear_object(&self->object);

   g_slice_free(RequestData, self);
}

static void
_query_data_free(QueryData *self)
{
   _request_data_free(self->req);

   if (self->standard_output > -1)
      g_close(self->standard_output, NULL);

   g_slice_free(QueryData, self);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(RequestData, _request_data_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(QueryData, _query_data_free)

static QueryData *
au_query_data_new(void)
{
   QueryData *data = g_slice_new0(QueryData);
   data->standard_output = -1;

   data->req = g_slice_new0(RequestData);

   return data;
}

/*
 * _au_get_expanded_variant:
 * @variant: (not nullable): Variant to expand
 *
 * In Jupiter we historically stored the chosen variant in a contracted
 * form. This method will convert the contracted variant into the
 * expanded version that could be used in `steamos-atomupd-client`.
 *
 * If the variant is not a legacy contracted version, a copy of @variant
 * will be returned.
 *
 * Returns: (transfer full): The expanded version of @variant, or a copy of
 *  @variant, if it doesn't need to be expanded.
 */
static gchar *
_au_get_expanded_variant(const gchar *variant)
{
   gsize i;

   g_return_val_if_fail(variant != NULL, NULL);

   for (i = 0; i < G_N_ELEMENTS(variant_conversions); i++) {
      if (g_strcmp0(variant, variant_conversions[i].contracted) == 0)
         return g_strdup(variant_conversions[i].expanded);
   }

   g_debug("The variant %s doesn't need to be expanded", variant);
   return g_strdup(variant);
}

static const gchar *
_au_get_legacy_branch_file_path(void)
{
   static const gchar *branch_file = NULL;

   if (branch_file == NULL) {
      /* This environment variable is used for debugging and automated tests */
      branch_file = g_getenv("AU_CHOSEN_BRANCH_FILE");

      if (branch_file == NULL)
         branch_file = AU_DEFAULT_BRANCH_PATH;
   }

   return branch_file;
}

static const gchar *
_au_get_fallback_config_path(void)
{
   static const gchar *config_path = NULL;

   if (config_path == NULL) {
      /* This environment variable is used for debugging and automated tests */
      config_path = g_getenv("AU_FALLBACK_CONFIG_PATH");

      if (config_path == NULL)
         config_path = AU_FALLBACK_CONFIG_PATH;
   }

   return config_path;
}

static const gchar *
_au_get_user_preferences_file_path(void)
{
   static const gchar *user_preferences_file = NULL;

   if (user_preferences_file == NULL) {
      /* This environment variable is used for debugging and automated tests */
      user_preferences_file = g_getenv("AU_USER_PREFERENCES_FILE");

      if (user_preferences_file == NULL)
         user_preferences_file = AU_USER_PREFERENCES;
   }

   return user_preferences_file;
}

static const gchar *
_au_get_remote_info_path(void)
{
   static const gchar *remote_info = NULL;

   if (remote_info == NULL) {
      /* This environment variable is used for debugging and automated tests */
      remote_info = g_getenv("AU_REMOTE_INFO_PATH");

      if (remote_info == NULL)
         remote_info = AU_REMOTE_INFO_PATH;
   }

   return remote_info;
}

/*
 * _au_update_user_preferences:
 * @variant: Which variant to track
 * @branch: Which branch to track
 * @http_proxy: (nullable): Which HTTP/HTTPS proxy to use, if any
 * @error: (out) (optional): Used to return an error on failure
 *
 * Returns: %TRUE if the user preferences were successfully written to a file
 */
static gboolean
_au_update_user_preferences(const gchar *variant,
                            const gchar *branch,
                            GVariant *http_proxy,
                            GError **error)
{
   const gchar *user_prefs_path = _au_get_user_preferences_file_path();
   g_autoptr(GKeyFile) preferences = g_key_file_new();
   g_autoptr(GError) local_error = NULL;

   if (!g_key_file_load_from_file(preferences, user_prefs_path, G_KEY_FILE_KEEP_COMMENTS,
                                  &local_error)) {
      if (g_error_matches(local_error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
         g_debug("'%s' is missing, creating a new one...", user_prefs_path);
      } else {
         g_warning("An error occurred while attempting to open the preferences file '%s'",
                   user_prefs_path);
         g_propagate_error(error, g_steal_pointer(&local_error));
         return FALSE;
      }
   }

   g_key_file_set_string(preferences, "Choices", "Variant", variant);
   g_key_file_set_string(preferences, "Choices", "Branch", branch);

   /* Remove the old HTTP proxy values, if present */
   g_key_file_remove_group(preferences, "Proxy", NULL);

   if (http_proxy != NULL) {
      const gchar *address = NULL;
      gint port;

      g_variant_get(http_proxy, "(&si)", &address, &port);

      if (g_strcmp0(address, "") != 0) {
         g_key_file_set_string(preferences, "Proxy", "Address", address);
         g_key_file_set_integer(preferences, "Proxy", "Port", port);
      }
   }

   return g_key_file_save_to_file(preferences, user_prefs_path, error);
}

/*
 * _au_convert_from_legacy_variant:
 * @legacy_variant: Legacy variant to convert
 * @variant_out: (out): Used to return the variant
 * @branch_out: (out): Used to return the branch
 * @error: (out) (optional): Used to return an error on failure
 *
 * Convert the legacy variant into the new variant and branch values.
 * Adapted from steamos-atomupd `convert_from_legacy_variant()`
 *
 * Returns: %TRUE if it was possible to convert @legacy_variant.
 */
static gboolean
_au_convert_from_legacy_variant(const gchar *legacy_variant,
                                gchar **variant_out,
                                gchar **branch_out)
{
   g_autofree gchar *expanded_variant = NULL;

   expanded_variant = _au_get_expanded_variant(legacy_variant);

   if (g_str_equal(expanded_variant, "steamdeck")) {
      *variant_out = g_strdup("steamdeck");
      *branch_out = g_strdup("stable");
   } else if (g_str_has_prefix(expanded_variant, "steamdeck-")) {
      *variant_out = g_strdup("steamdeck");
      *branch_out = g_strdup(expanded_variant + strlen("steamdeck-"));
   } else {
      g_warning("The legacy variant '%s' is unexpected", expanded_variant);
      return FALSE;
   }

   return TRUE;
}

/*
 * _au_load_legacy_preferences:
 * @branch_file_path: (not nullable): Path to the legacy steamos-branch file
 * @variant_out: (out): Used to return the tracked variant
 * @branch_out: (out): Used to return the tracked branch
 * @error: Used to raise an error on failure
 *
 * Retrieve the variant and branch that are currently being tracked by parsing the
 * old legacy "steamos-branch" file.
 *
 * Returns: %TRUE if the preferences are correctly retrieved
 */
static gboolean
_au_load_legacy_preferences(const gchar *branch_file_path,
                            gchar **variant_out,
                            gchar **branch_out,
                            GError **error)
{
   g_autofree gchar *variant = NULL;
   g_autofree gchar *branch = NULL;
   g_autofree gchar *legacy_variant = NULL;
   gsize len;
   const gchar *user_prefs_path = _au_get_user_preferences_file_path();

   g_return_val_if_fail(branch_file_path != NULL, FALSE);
   g_return_val_if_fail(variant_out != NULL && *variant_out == NULL, FALSE);
   g_return_val_if_fail(branch_out != NULL && *branch_out == NULL, FALSE);
   g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

   if (!g_file_test(branch_file_path, G_FILE_TEST_EXISTS)) {
      return au_throw_error(error,
                            "The legacy config file '%s' is not present. Skipping it...",
                            branch_file_path);
   }

   g_debug("Parsing the legacy steamos-branch file '%s'", branch_file_path);

   if (!g_file_get_contents(branch_file_path, &legacy_variant, &len, error)) {
      g_warning("The legacy config file '%s' is probably malformed", branch_file_path);
      g_unlink(branch_file_path);
      return FALSE;
   }

   if (len != 0) {
      const char *search;

      /* Remove eventual trailing newline that could have been added by
       * steamos-select-branch */
      if (legacy_variant[len - 1] == '\n')
         legacy_variant[len - 1] = '\0';

      search = strstr(legacy_variant, "\n");
      if (search != NULL) {
         /* If we have multiple newlines the file is likely malformed */
         g_warning(
            "The legacy config file '%s' has multiple lines, seems to be malformed",
            branch_file_path);
         g_unlink(branch_file_path);
         return au_throw_error(error, "Failed to parse the legacy config file '%s'",
                               branch_file_path);
      }
   }

   /* Extrapolate the variant and branch from the legacy variant value, if valid */
   if (!_au_convert_from_legacy_variant(legacy_variant, &variant, &branch)) {
      g_warning("Unparsable legacy branch file variant '%s', removing '%s'",
                legacy_variant, branch_file_path);
      g_unlink(branch_file_path);
      return au_throw_error(error, "Failed to convert the legacy config file '%s'",
                            branch_file_path);
   }

   if (!_au_update_user_preferences(variant, branch, NULL, error)) {
      g_warning("An error occurred while migrating to the new '%s' file: %s",
                user_prefs_path, (*error)->message);
      return FALSE;
   }

   g_debug("The user preferences have been migrated to the new '%s' file",
           user_prefs_path);

   /* After migrating the preferences we can remove the deprecated old branch file */
   g_unlink(branch_file_path);

   *variant_out = g_steal_pointer(&variant);
   *branch_out = g_steal_pointer(&branch);
   return TRUE;
}

/*
 * _au_load_preferences_file:
 * @user_prefs_path: (not nullable): Path to the preferences file
 * @variant_out: (out): Used to return the tracked variant
 * @branch_out: (out): Used to return the tracked branch
 * @error: Used to raise an error on failure
 *
 * Parses the user preference file and retrieves the variant and branch that are
 * currently being tracked.
 *
 * Returns: %TRUE if the preferences are correctly retrieved
 */
static gboolean
_au_load_user_preferences_file(const gchar *user_prefs_path,
                               gchar **variant_out,
                               gchar **branch_out,
                               GVariant **http_proxy_out,
                               GError **error)
{
   g_autofree gchar *variant = NULL;
   g_autofree gchar *branch = NULL;
   g_autofree gchar *proxy_address = NULL;
   int proxy_port = -1;
   g_autoptr(GVariant) http_proxy = NULL;
   g_autoptr(GKeyFile) user_prefs = g_key_file_new();
   g_autoptr(GError) local_error = NULL;

   g_return_val_if_fail(user_prefs_path != NULL, FALSE);
   g_return_val_if_fail(variant_out != NULL && *variant_out == NULL, FALSE);
   g_return_val_if_fail(branch_out != NULL && *branch_out == NULL, FALSE);
   g_return_val_if_fail(http_proxy_out != NULL && *http_proxy_out == NULL, FALSE);
   g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

   if (!g_file_test(user_prefs_path, G_FILE_TEST_EXISTS)) {
      return au_throw_error(
         error, "The user preferences config file '%s' is not present. Skipping it...",
         user_prefs_path);
   }

   g_debug("Parsing the preferences.conf file '%s'", user_prefs_path);

   if (!g_key_file_load_from_file(user_prefs, user_prefs_path, G_KEY_FILE_NONE, error)) {
      g_warning("The user preferences config file '%s' is probably malformed",
                user_prefs_path);
      return FALSE;
   }

   variant = g_key_file_get_string(user_prefs, "Choices", "Variant", error);
   if (variant == NULL) {
      g_warning("Failed to parse the chosen Variant from '%s'", user_prefs_path);
      return FALSE;
   }

   branch = g_key_file_get_string(user_prefs, "Choices", "Branch", error);
   if (branch == NULL) {
      g_warning("Failed to parse the chosen Branch from '%s'", user_prefs_path);
      return FALSE;
   }

   proxy_address = g_key_file_get_string(user_prefs, "Proxy", "Address", NULL);
   if (proxy_address == NULL) {
      g_debug("The user preferences config file doesn't have an HTTP proxy configured");
   } else {
      proxy_port = g_key_file_get_integer(user_prefs, "Proxy", "Port", &local_error);
      if (local_error == NULL) {
         http_proxy =
            g_variant_ref_sink(g_variant_new("(si)", proxy_address, proxy_port));
      } else {
         g_warning("Failed to parse the configured Proxy Port from '%s': %s, trying to "
                   "continue...",
                   user_prefs_path, local_error->message);
         g_clear_error(&local_error);
      }
   }

   *variant_out = g_steal_pointer(&variant);
   *branch_out = g_steal_pointer(&branch);
   *http_proxy_out = g_steal_pointer(&http_proxy);
   return TRUE;
}

static gchar *
_au_get_default_variant(const gchar *manifest, GError **error);

static gchar *
_au_get_default_branch(const gchar *manifest);

/*
 * _au_load_preferences_from_manifest:
 * @manifest_path: (not nullable): Path to the image manifest file
 * @variant_out: (out): Used to return the tracked variant
 * @branch_out: (out): Used to return the tracked branch
 * @error: Used to raise an error on failure
 *
 * Retrieve the default variant and branch by parsing the image JSON manifest file.
 *
 * Returns: %TRUE if the values are correctly retrieved
 */
static gboolean
_au_load_preferences_from_manifest(const gchar *manifest_path,
                                   gchar **variant_out,
                                   gchar **branch_out,
                                   GError **error)
{
   g_autofree gchar *variant = NULL;
   g_autofree gchar *branch = NULL;

   g_return_val_if_fail(manifest_path != NULL, FALSE);
   g_return_val_if_fail(variant_out != NULL && *variant_out == NULL, FALSE);
   g_return_val_if_fail(branch_out != NULL && *branch_out == NULL, FALSE);
   g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

   g_debug("Parsing the image manifest '%s' to grab the variant and branch",
           manifest_path);

   variant = _au_get_default_variant(manifest_path, error);
   if (variant == NULL) {
      g_warning("Failed to parse the default variant from the image manifest");
      return FALSE;
   }

   branch = _au_get_default_branch(manifest_path);

   if (!_au_update_user_preferences(variant, branch, NULL, error))
      return FALSE;

   *variant_out = g_steal_pointer(&variant);
   *branch_out = g_steal_pointer(&branch);
   return TRUE;
}

/*
 * _au_clear_available_updates:
 *
 * Clear the eventual available updates list.
 */
static void
_au_clear_available_updates(AuAtomupd1 *object)
{
   g_autoptr(GVariant) available = NULL;
   g_autoptr(GVariant) available_later = NULL;

   available = g_variant_new("a{sa{sv}}", NULL);
   available_later = g_variant_new("a{sa{sv}}", NULL);
   au_atomupd1_set_updates_available(object, g_steal_pointer(&available));
   au_atomupd1_set_updates_available_later(object, g_steal_pointer(&available_later));
}

/*
 * _au_get_http_auth_from_config:
 * @client_config: (not nullable): Object that holds the configuration key file
 * @username_out: (out): Used to return the required username
 * @password_out: (out): Used to return the required password
 * @encoded_out: (out): Used to return the auth type, followed by the base64
 *  encoded `@username_out:@password_out`
 *
 * Returns: %TRUE if the configuration has the HTTP authentication info.
 */
gboolean
_au_get_http_auth_from_config(GKeyFile *client_config,
                              gchar **username_out,
                              gchar **password_out,
                              gchar **encoded_out)
{
   g_autofree gchar *username = NULL;
   g_autofree gchar *password = NULL;
   g_autoptr(GError) local_error = NULL;

   g_return_val_if_fail(client_config != NULL, FALSE);
   g_return_val_if_fail(username_out == NULL || *username_out == NULL, FALSE);
   g_return_val_if_fail(password_out == NULL || *password_out == NULL, FALSE);
   g_return_val_if_fail(encoded_out == NULL || *encoded_out == NULL, FALSE);

   username = g_key_file_get_string(client_config, "Server", "Username", &local_error);
   if (username == NULL) {
      g_debug("Assuming no authentication required for this config: %s",
              local_error->message);
      return FALSE;
   }

   password = g_key_file_get_string(client_config, "Server", "Password", &local_error);
   if (password == NULL) {
      g_debug("Assuming no authentication required for this config: %s",
              local_error->message);
      return FALSE;
   }

   if (encoded_out != NULL) {
      g_autofree gchar *user_pass = NULL;
      g_autofree gchar *user_pass_base64 = NULL;

      user_pass = g_strdup_printf("%s:%s", username, password);
      user_pass_base64 = g_base64_encode((guchar *)user_pass, strlen(user_pass));
      *encoded_out = g_strdup_printf("Basic %s", user_pass_base64);
   }

   if (username_out != NULL)
      *username_out = g_steal_pointer(&username);

   if (password_out != NULL)
      *password_out = g_steal_pointer(&password);

   return TRUE;
}

/*
 * _au_get_urls_from_config:
 * @client_config: (not nullable): Object that holds the configuration key file
 * @error: Used to raise an error on failure
 *
 * Get the keys from the "Server" section of @client_config that specify the
 * URLs to use.
 *
 * Returns: A GHashTable, free with `g_hash_table_unref()`
 */
static GHashTable *
_au_get_urls_from_config(GKeyFile *client_config, GError **error)
{
   g_autoptr(GHashTable) urls = NULL;
   g_auto(GStrv) keys = NULL;
   gsize i;

   g_return_val_if_fail(client_config != NULL, FALSE);

   urls = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
   keys = g_key_file_get_keys(client_config, "Server", NULL, error);

   if (keys == NULL)
      return NULL;

   for (i = 0; keys[i] != NULL; i++) {
      if (g_str_has_suffix(keys[i], "Url")) {
         g_autofree gchar *url_value = NULL;
         url_value = g_key_file_get_string(client_config, "Server", keys[i], error);

         if (url_value == NULL)
            return NULL;

         g_hash_table_insert(urls, g_strdup(keys[i]), g_steal_pointer(&url_value));
      }
   }

   return g_steal_pointer(&urls);
}

/*
 * _au_get_list_from_config:
 * @client_config: (not nullable): Object that holds the configuration key file
 * @key: (not nullable): The key to retrieve
 * @error: Used to raise an error on failure
 *
 * The only allowed symbols are lowercase and uppercase word characters,
 * numbers, underscore, hyphen and the semicolon as a separator.
 * Eventual values that have symbols outside those allowed, will be
 * skipped.
 *
 * Returns: (array zero-terminated=1) (transfer full) (nullable): The values
 *  list, or %NULL if the configuration doesn't have the @key field.
 */
static gchar **
_au_get_list_from_config(GKeyFile *client_config, const gchar *key, GError **error)
{
   g_autoptr(GRegex) regex;
   g_autoptr(GPtrArray) valid_entries = NULL;
   g_auto(GStrv) entries = NULL;

   g_return_val_if_fail(client_config != NULL, NULL);
   g_return_val_if_fail(key != NULL, NULL);

   regex = g_regex_new("^[a-zA-Z0-9_-]+$", 0, 0, NULL);
   g_assert(regex != NULL); /* known to be valid at compile-time */

   valid_entries = g_ptr_array_new_with_free_func(g_free);

   entries = g_key_file_get_string_list(client_config, "Server", key, NULL, error);
   if (entries == NULL)
      return NULL;

   /* Sanitize the input. This helps us to skip improper/unexpected user inputs */
   for (gsize i = 0; entries[i] != NULL; i++) {
      if (g_regex_match(regex, entries[i], 0, NULL))
         g_ptr_array_add(valid_entries, g_strdup(entries[i]));
      else
         g_warning(
            "The config value \"%s\" has characters that are not allowed, skipping...",
            entries[i]);
   }

   g_ptr_array_add(valid_entries, NULL);

   return (gchar **)g_ptr_array_free(g_steal_pointer(&valid_entries), FALSE);
}

/*
 * _au_get_known_variants_from_config:
 *
 * Returns: (array zero-terminated=1) (transfer full) (nullable): The list
 *  of known variants, or %NULL if the configuration doesn't have the
 *  "Variants" field.
 */
static gchar **
_au_get_known_variants_from_config(GKeyFile *client_config, GError **error)
{
   return _au_get_list_from_config(client_config, "Variants", error);
}

/*
 * _au_get_known_branches_from_config:
 *
 * Returns: (array zero-terminated=1) (transfer full) (nullable): The list
 *  of known branches, or %NULL if the configuration doesn't have the
 *  "Branches" field.
 */
static gchar **
_au_get_known_branches_from_config(GKeyFile *client_config, GError **error)
{
   return _au_get_list_from_config(client_config, "Branches", error);
}

/*
 * _au_get_meta_url_from_default_config:
 * @atomupd: (not nullable): An AuAtomupd1Impl object
 *
 * Get the meta URL value from the default AU_CONFIG file located in the @atomupd config
 * directory. I.e. this skips the client-dev.conf file, even if it is present.
 * If the default configuration file is malformed, also the fallback config path will be
 * used as a last resort attempt.
 *
 * Returns: (type filename) (transfer full): The MetaUrl value from the configuration.
 */
static gchar *
_au_get_meta_url_from_default_config(const AuAtomupd1Impl *atomupd, GError **error)
{
   g_autofree gchar *default_config_path = NULL;
   g_autofree gchar *fallback_config_path = NULL;
   g_autoptr(GError) local_error = NULL;
   gsize i;

   g_return_val_if_fail(atomupd != NULL, NULL);

   default_config_path = g_build_filename(atomupd->config_directory, AU_CONFIG, NULL);
   fallback_config_path =
      g_build_filename(_au_get_fallback_config_path(), AU_CONFIG, NULL);

   const gchar *config_path[] = { default_config_path, fallback_config_path, NULL };

   for (i = 0; config_path[i] != NULL; i++) {
      g_autofree gchar *meta_url = NULL;
      g_autoptr(GKeyFile) client_config = g_key_file_new();

      g_clear_error(&local_error);

      if (g_key_file_load_from_file(client_config, config_path[i], G_KEY_FILE_NONE,
                                    &local_error)) {
         meta_url =
            g_key_file_get_string(client_config, "Server", "MetaUrl", &local_error);
         if (meta_url != NULL)
            return g_steal_pointer(&meta_url);
      }

      g_info("Failed to load the MetaUrl property from '%s': %s", config_path[i],
             local_error->message);
   }

   g_propagate_error(error, g_steal_pointer(&local_error));
   return NULL;
}

/*
 * _au_get_string_from_manifest:
 * @manifest: (not nullable): Path to the JSON manifest file
 * @key: (not nullable): Key whose value will be returned
 * @error: Used to raise an error on failure
 *
 * Returns: (type filename) (transfer full): The value of @key, taken from the
 *  manifest file.
 */
static gchar *
_au_get_string_from_manifest(const gchar *manifest, const gchar *key, GError **error)
{
   const gchar *value = NULL;
   JsonNode *json_node = NULL;     /* borrowed */
   JsonObject *json_object = NULL; /* borrowed */
   g_autoptr(JsonParser) parser = NULL;

   g_return_val_if_fail(manifest != NULL, NULL);
   g_return_val_if_fail(key != NULL, NULL);
   g_return_val_if_fail(error == NULL || *error == NULL, NULL);

   parser = json_parser_new();
   if (!json_parser_load_from_file(parser, manifest, error))
      return NULL;

   json_node = json_parser_get_root(parser);
   if (json_node == NULL) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "failed to parse the manifest JSON \"%s\"", manifest);
      return NULL;
   }

   json_object = json_node_get_object(json_node);
   value = json_object_get_string_member_with_default(json_object, key, NULL);

   if (value == NULL)
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "the parsed manifest JSON \"%s\" doesn't have the expected \"%s\" key",
                  manifest, key);

   return g_strdup(value);
}

/*
 * _au_get_default_variant:
 * @manifest: (not nullable): Path to the JSON manifest file
 * @error: Used to raise an error on failure
 *
 * Returns: (type filename) (transfer full): The variant value taken from the
 *  manifest file, or %NULL on failure
 */
static gchar *
_au_get_default_variant(const gchar *manifest, GError **error)
{
   return _au_get_string_from_manifest(manifest, "variant", error);
}

/*
 * _au_get_default_branch:
 * @manifest: (not nullable): Path to the JSON manifest file
 *
 * Returns: (type filename) (transfer full): The branch value taken from the
 *  manifest file, or an hardcoded `stable` on failure
 */
static gchar *
_au_get_default_branch(const gchar *manifest)
{
   g_autofree gchar *branch = NULL;
   g_autoptr(GError) local_error = NULL;

   branch = _au_get_string_from_manifest(manifest, "default_update_branch", &local_error);

   if (branch == NULL) {
      g_warning("Failed to parse the default branch from the image manifest. Using "
                "`stable` as a last resort attempt: %s",
                local_error->message);
      branch = g_strdup("stable");
   }

   return g_steal_pointer(&branch);
}

/*
 * _au_get_current_system_build_id:
 * @manifest: (not nullable): Path to the JSON manifest file
 * @error: Used to raise an error on failure
 *
 * Returns: (type filename) (transfer full): The system build ID, taken
 *  from the manifest file.
 */
static gchar *
_au_get_current_system_build_id(const gchar *manifest, GError **error)
{
   return _au_get_string_from_manifest(manifest, "buildid", error);
}

typedef void (*AuthorizedCallback)(AuAtomupd1 *object,
                                   GDBusMethodInvocation *invocation,
                                   gpointer data);

typedef struct {
   AuAtomupd1 *object;
   AuthorizedCallback authorized_cb;
   GDBusMethodInvocation *invocation;
   gpointer data;
   GDestroyNotify destroy_notify;
} CheckAuthData;

static CheckAuthData *
_au_check_auth_data_new(AuAtomupd1 *object,
                        AuthorizedCallback authorized_cb,
                        GDBusMethodInvocation *invocation,
                        gpointer authorized_cb_data,
                        GDestroyNotify destroy_notify)
{
   CheckAuthData *data = g_slice_new0(CheckAuthData);
   data->object = g_object_ref(object);
   data->invocation = invocation;
   data->authorized_cb = authorized_cb;
   data->data = authorized_cb_data;
   data->destroy_notify = destroy_notify;

   return data;
}

static void
_au_check_auth_data_free(CheckAuthData *self)
{
   g_object_unref(self->object);

   if (self->invocation != NULL)
      g_dbus_method_invocation_return_error(g_steal_pointer(&self->invocation),
                                            G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                            "Request was freed without being handled");

   if (self->destroy_notify)
      (*self->destroy_notify)(self->data);

   g_slice_free(CheckAuthData, self);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(CheckAuthData, _au_check_auth_data_free)

static void
_check_auth_cb(PolkitAuthority *authority, GAsyncResult *res, gpointer data)
{
   g_autoptr(CheckAuthData) check_auth_data = data;
   g_autoptr(GError) error = NULL;
   PolkitAuthorizationResult *authorization;

   authorization = polkit_authority_check_authorization_finish(authority, res, &error);

   if (authorization == NULL) {
      g_dbus_method_invocation_return_error(
         g_steal_pointer(&check_auth_data->invocation), G_DBUS_ERROR,
         G_DBUS_ERROR_ACCESS_DENIED,
         "An error occurred while checking for authorizations: %s", error->message);
      return;
   }

   if (!polkit_authorization_result_get_is_authorized(authorization)) {
      g_dbus_method_invocation_return_error(g_steal_pointer(&check_auth_data->invocation),
                                            G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED,
                                            "User is not allowed to execute this method");
      return;
   }

   (*check_auth_data->authorized_cb)(check_auth_data->object,
                                     g_steal_pointer(&check_auth_data->invocation),
                                     check_auth_data->data);
}

static void
_au_check_auth(AuAtomupd1 *object,
               const gchar *action_id,
               AuthorizedCallback authorized_cb,
               GDBusMethodInvocation *invocation,
               gpointer authorized_cb_data,
               GDestroyNotify destroy_notify)
{
   GDBusMessage *message = NULL; /* borrowed */
   GDBusMessageFlags message_flags;
   PolkitCheckAuthorizationFlags flags = POLKIT_CHECK_AUTHORIZATION_FLAGS_NONE;
   g_autoptr(PolkitSubject) subject = NULL;
   g_autoptr(CheckAuthData) data = NULL;
   AuAtomupd1Impl *self = AU_ATOMUPD1_IMPL(object);

   data = _au_check_auth_data_new(object, authorized_cb, invocation, authorized_cb_data,
                                  destroy_notify);

   subject = polkit_system_bus_name_new(g_dbus_method_invocation_get_sender(invocation));

   message = g_dbus_method_invocation_get_message(invocation);
   message_flags = g_dbus_message_get_flags(message);

   if (message_flags & G_DBUS_MESSAGE_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION)
      flags |= POLKIT_CHECK_AUTHORIZATION_FLAGS_ALLOW_USER_INTERACTION;

   polkit_authority_check_authorization(self->authority, subject, action_id, NULL, flags,
                                        NULL, (GAsyncReadyCallback)_check_auth_cb,
                                        g_steal_pointer(&data));
}

/*
 * _au_get_current_system_version:
 * @manifest: (not nullable): Path to the JSON manifest file
 * @error: Used to raise an error on failure
 *
 * Returns: (type filename) (transfer full): The system version, taken
 *  from the manifest file.
 */
static gchar *
_au_get_current_system_version(const gchar *manifest, GError **error)
{
   return _au_get_string_from_manifest(manifest, "version", error);
}

/*
 * @candidate_obj: (not nullable): A JSON object representing the image update
 * @id: (out) (not optional):
 * @version: (out) (not optional):
 * @variant: (out) (not optional):
 * @size: (out) (not optional):
 * @error: Used to raise an error on failure
 */
static gboolean
_au_parse_image(JsonObject *candidate_obj,
                gchar **id,
                gchar **version,
                gchar **variant,
                gint64 *size,
                GError **error)
{
   JsonObject *img_obj = NULL; /* borrowed */
   JsonNode *img_node = NULL;  /* borrowed */
   const gchar *local_id = NULL;
   const gchar *local_version = NULL;
   const gchar *local_variant = NULL;
   gint64 local_size;

   g_return_val_if_fail(candidate_obj != NULL, FALSE);
   g_return_val_if_fail(id != NULL, FALSE);
   g_return_val_if_fail(version != NULL, FALSE);
   g_return_val_if_fail(variant != NULL, FALSE);
   g_return_val_if_fail(size != NULL, FALSE);
   g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

   img_node = json_object_get_member(candidate_obj, "image");
   img_obj = json_node_get_object(img_node);

   local_size = json_object_get_int_member_with_default(img_obj, "estimated_size", 0);
   local_id = json_object_get_string_member_with_default(img_obj, "buildid", NULL);
   local_version = json_object_get_string_member_with_default(img_obj, "version", NULL);
   local_variant = json_object_get_string_member_with_default(img_obj, "variant", NULL);
   if (local_id == NULL || local_version == NULL || local_variant == NULL) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "The \"image\" JSON object doesn't have the expected members");
      return FALSE;
   }

   *id = g_strdup(local_id);
   *version = g_strdup(local_version);
   *variant = g_strdup(local_variant);
   *size = local_size;

   return TRUE;
}

/*
 * @json_node: (not nullable): The JsonNode of the steamos-atomupd-client output
 * @updated_build_id: Update build ID that has been already installed and is
 *  waiting a reboot, or %NULL if there isn't such update
 * @replacement_eol_variant: (out) (not optional): If the requested variant is EOL,
 *  this is set to its replacement, or %NULL otherwise.
 * @available: (out) (not optional): Map of available updates that can be installed
 * @available_later: (out) (not optional): Map of available updates that require
 *  a newer system version
 * @error: Used to raise an error on failure
 */
static gboolean
_au_parse_candidates(JsonNode *json_node,
                     const gchar *updated_build_id,
                     GVariant **available,
                     GVariant **available_later,
                     gchar **replacement_eol_variant,
                     GError **error)
{
   g_auto(GVariantBuilder) available_builder =
      G_VARIANT_BUILDER_INIT(G_VARIANT_TYPE("a{sa{sv}}"));
   g_auto(GVariantBuilder) available_later_builder =
      G_VARIANT_BUILDER_INIT(G_VARIANT_TYPE("a{sa{sv}}"));
   /* We expect the update candidates to be under the "minor" key for legacy reasons */
   const gchar *type_string = "minor";
   g_autofree gchar *
      requires
   = NULL;
   JsonObject *json_object = NULL; /* borrowed */
   JsonObject *sub_obj = NULL;     /* borrowed */
   JsonNode *sub_node = NULL;      /* borrowed */
   JsonArray *array = NULL;        /* borrowed */
   guint array_size;
   gsize i;

   g_return_val_if_fail(json_node != NULL, FALSE);
   g_return_val_if_fail(available != NULL, FALSE);
   g_return_val_if_fail(*available == NULL, FALSE);
   g_return_val_if_fail(available_later != NULL, FALSE);
   g_return_val_if_fail(*available_later == NULL, FALSE);
   g_return_val_if_fail(replacement_eol_variant != NULL, FALSE);
   g_return_val_if_fail(*replacement_eol_variant == NULL, FALSE);
   g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

   json_object = json_node_get_object(json_node);

   if (!json_object_has_member(json_object, type_string))
      goto success;

   sub_node = json_object_get_member(json_object, type_string);
   sub_obj = json_node_get_object(sub_node);

   if (!json_object_has_member(sub_obj, "candidates")) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "The JSON doesn't have the expected \"candidates\" member");
      return FALSE;
   }

   /* Note that despite its name, the `candidates` member does not
    * actually list multiple possible updates that can be applied
    * immediately. Instead, it lists a single update that can be
    * applied immediately, followed by 0 or more updates that can
    * only be applied after passing through earlier checkpoints. */
   array = json_object_get_array_member(sub_obj, "candidates");

   array_size = json_array_get_length(array);

   for (i = 0; i < array_size; i++) {
      g_autofree gchar *id = NULL;
      g_autofree gchar *variant = NULL;
      g_autofree gchar *version = NULL;
      gint64 size;
      GVariantBuilder builder;

      if (!_au_parse_image(json_array_get_object_element(array, i), &id, &version,
                           &variant, &size, error))
         return FALSE;

      if (i == 0 && g_strcmp0(id, updated_build_id) == 0) {
         /* If the first proposed update matches the version already
          * applied (and is pending a reboot), there's nothing left for
          * us to do. Otherwise, we would be applying the same update
          * twice - something which we want avoid. */
         g_debug("The proposed update to version '%s' has already been applied. Reboot "
                 "to start using it.",
                 id);
         goto success;
      }

      g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));

      g_variant_builder_add(&builder, "{sv}", "version", g_variant_new_string(version));
      g_variant_builder_add(&builder, "{sv}", "variant", g_variant_new_string(variant));
      g_variant_builder_add(&builder, "{sv}", "estimated_size",
                            g_variant_new_uint64(size));
      if (requires != NULL)
         g_variant_builder_add(&builder, "{sv}", "requires",
                               g_variant_new_string(requires));

      g_variant_builder_add(i == 0 ? &available_builder : &available_later_builder,
                            "{sa{sv}}", id, &builder);

      g_clear_pointer(&requires, g_free);
      requires = g_steal_pointer(&id);
   }

success:
   *available = g_variant_ref_sink(g_variant_builder_end(&available_builder));
   *available_later = g_variant_ref_sink(g_variant_builder_end(&available_later_builder));
   if (sub_obj != NULL)
      /* If the requested variant was EOL, save the new variant that the server
       * is proposing as its alternative */
      *replacement_eol_variant = g_strdup(json_object_get_string_member_with_default(
         sub_obj, "replacement_eol_variant", NULL));

   return TRUE;
}

static gboolean
_au_switch_to_variant(AuAtomupd1 *object,
                      gchar *variant,
                      gboolean clear_available_updates,
                      GError **error);

static gboolean
_au_switch_to_branch(AuAtomupd1 *object, gchar *branch, GError **error);

static void
on_query_completed(GPid pid, gint wait_status, gpointer user_data)
{
   g_autoptr(QueryData) data = user_data;
   g_autoptr(GIOChannel) stdout_channel = NULL;
   g_autoptr(GVariant) available = NULL;
   g_autoptr(GVariant) available_later = NULL;
   g_autofree gchar *replacement_eol_variant = NULL;
   g_autoptr(JsonNode) json_node = NULL;
   g_autoptr(GError) error = NULL;
   g_autofree gchar *output = NULL;
   const gchar *updated_build_id = NULL;
   gsize out_length;
   AuUpdateStatus current_status;
   AuAtomupd1Impl *self = AU_ATOMUPD1_IMPL(data->req->object);

   if (!g_spawn_check_wait_status(wait_status, &error)) {
      if (error->domain == G_SPAWN_EXIT_ERROR && error->code == 2) {
         /* The query server returned an HTTP error in the 4xx range */

         g_autofree gchar *variant = NULL;
         g_autofree gchar *branch = NULL;
         const gchar *initial_variant = NULL;
         const gchar *initial_branch = NULL;
         g_autoptr(GError) local_error = NULL;

         variant = _au_get_default_variant(self->manifest_path, &local_error);
         if (variant == NULL) {
            g_dbus_method_invocation_return_error(
               g_steal_pointer(&data->req->invocation), G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
               "The server query returned HTTP 4xx and parsing the default variant from "
               "the image manifest failed: %s",
               local_error->message);
            return;
         }

         branch = _au_get_default_branch(self->manifest_path);

         initial_variant = au_atomupd1_get_variant(data->req->object);
         initial_branch = au_atomupd1_get_branch(data->req->object);

         if (g_strcmp0(initial_variant, variant) == 0 &&
             g_strcmp0(initial_branch, branch) == 0) {
            g_dbus_method_invocation_return_error(
               g_steal_pointer(&data->req->invocation), G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
               "The server query returned HTTP 4xx. We are already following the default "
               "variant and branch, nothing else we can do...");
            return;
         }

         g_warning(
            "The server query returned HTTP 4xx. Reverting the variant and branch to "
            "the default values: %s, %s",
            variant, branch);

         if (!_au_switch_to_variant(data->req->object, variant, TRUE, &local_error)) {
            g_dbus_method_invocation_return_error(
               g_steal_pointer(&data->req->invocation), G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
               "An error occurred while switching to the default variant '%s': %s",
               variant, local_error->message);
            return;
         }

         if (!_au_switch_to_branch(data->req->object, branch, &local_error)) {
            g_dbus_method_invocation_return_error(
               g_steal_pointer(&data->req->invocation), G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
               "An error occurred while switching to the default branch '%s': %s",
               variant, local_error->message);
            return;
         }

         g_dbus_method_invocation_return_error(
            g_steal_pointer(&data->req->invocation), G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
            "The server query returned HTTP 4xx. The tracked variant and branch have "
            "been reverted to the default values: '%s', '%s'",
            variant, branch);
         return;
      }

      g_dbus_method_invocation_return_error(
         g_steal_pointer(&data->req->invocation), G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
         "An error occurred calling the 'steamos-atomupd-client' helper: %s",
         error->message);
      return;
   }

   stdout_channel = g_io_channel_unix_new(data->standard_output);
   if (g_io_channel_read_to_end(stdout_channel, &output, &out_length, &error) !=
       G_IO_STATUS_NORMAL) {
      g_dbus_method_invocation_return_error(
         g_steal_pointer(&data->req->invocation), G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
         "An error occurred reading the output of 'steamos-atomupd-client' helper: %s",
         error->message);
      return;
   }

   if (out_length == 0 || output[0] == '\0') {
      /* In theory when no updates are available we should receive an empty
       * JSON object (i.e. {}). Is it okay to assume no updates or should we
       * throw an error here? */
      available = g_variant_ref_sink(g_variant_new("a{sa{sv}}", NULL));
      available_later = g_variant_ref_sink(g_variant_new("a{sa{sv}}", NULL));
      goto success;
   }

   if (out_length != strlen(output)) {
      /* This might happen if there is the terminating null byte '\0' followed
       * by some other data */
      g_dbus_method_invocation_return_error(
         g_steal_pointer(&data->req->invocation), G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
         "Helper output is not valid JSON: contains \\0");
      return;
   }

   json_node = json_from_string(output, &error);
   if (json_node == NULL) {
      if (error == NULL) {
         /* The helper returned an empty JSON, there are no available updates */
         available = g_variant_ref_sink(g_variant_new("a{sa{sv}}", NULL));
         available_later = g_variant_ref_sink(g_variant_new("a{sa{sv}}", NULL));
         goto success;
      } else {
         g_dbus_method_invocation_return_error(
            g_steal_pointer(&data->req->invocation), G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
            "The helper output is not a valid JSON: %s", error->message);
         return;
      }
   }

   current_status = au_atomupd1_get_update_status(data->req->object);
   if (current_status == AU_UPDATE_STATUS_SUCCESSFUL)
      updated_build_id = au_atomupd1_get_update_build_id(data->req->object);

   if (!_au_parse_candidates(json_node, updated_build_id, &available, &available_later,
                             &replacement_eol_variant, &error)) {
      g_dbus_method_invocation_return_error(
         g_steal_pointer(&data->req->invocation), G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
         "An error occurred while parsing the helper output JSON: %s", error->message);
      return;
   }

   if (!g_file_replace_contents(self->updates_json_file, output, out_length, NULL, FALSE,
                                G_FILE_CREATE_NONE, NULL, NULL, &error)) {
      g_dbus_method_invocation_return_error(
         g_steal_pointer(&data->req->invocation), G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
         "An error occurred while storing the helper output JSON: %s", error->message);
      return;
   }

   if (replacement_eol_variant != NULL) {
      g_debug("Switching from the EOL variant %s to its replacement %s",
              au_atomupd1_get_variant(data->req->object), replacement_eol_variant);

      if (!_au_switch_to_variant(data->req->object, replacement_eol_variant, FALSE,
                                 &error)) {
         g_dbus_method_invocation_return_error(
            g_steal_pointer(&data->req->invocation), G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
            "An error occurred while switching to the new variant '%s': %s",
            replacement_eol_variant, error->message);
         return;
      }
   }

success:
   au_atomupd1_set_updates_available(data->req->object, available);
   au_atomupd1_set_updates_available_later(data->req->object, available_later);
   au_atomupd1_complete_check_for_updates(data->req->object,
                                          g_steal_pointer(&data->req->invocation),
                                          available, available_later);
}

static gboolean
_au_switch_to_variant(AuAtomupd1 *object,
                      gchar *variant,
                      gboolean clear_available_updates,
                      GError **error)
{
   const gchar *branch = au_atomupd1_get_branch(object);
   GVariant *http_proxy = au_atomupd1_get_http_proxy(object); /* borrowed */

   g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

   if (g_strcmp0(variant, au_atomupd1_get_variant(object)) == 0) {
      g_debug("We are already tracking the variant %s, nothing to do", variant);
      return TRUE;
   }

   if (!_au_update_user_preferences(variant, branch, http_proxy, error))
      return FALSE;

   /* When changing variant in theory we could re-download the remote-info.conf file.
    * However, the chances of being different from the one we were already supposed to
    * have are very slim. So, in practice there is no real need to do it. */

   if (clear_available_updates)
      _au_clear_available_updates(object);

   au_atomupd1_set_variant(object, variant);

   return TRUE;
}

static void
au_switch_variant_authorized_cb(AuAtomupd1 *object,
                                GDBusMethodInvocation *invocation,
                                gpointer arg_variant_pointer)
{
   g_autoptr(GError) error = NULL;

   if (!_au_switch_to_variant(object, arg_variant_pointer, TRUE, &error)) {
      g_dbus_method_invocation_return_error(
         g_steal_pointer(&invocation), G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
         "An error occurred while switching to the chosen variant: %s", error->message);
      return;
   }

   au_atomupd1_complete_switch_to_variant(object, g_steal_pointer(&invocation));
}

static gboolean
au_atomupd1_impl_handle_switch_to_variant(AuAtomupd1 *object,
                                          GDBusMethodInvocation *invocation,
                                          const gchar *arg_variant)
{
   _au_check_auth(object, "com.steampowered.atomupd1.switch-variant-or-branch",
                  au_switch_variant_authorized_cb, invocation, g_strdup(arg_variant),
                  g_free);

   return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
_au_switch_to_branch(AuAtomupd1 *object, gchar *branch, GError **error)
{
   const gchar *variant = au_atomupd1_get_variant(object);
   GVariant *http_proxy = au_atomupd1_get_http_proxy(object); /* borrowed */

   g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

   if (g_str_equal(branch, au_atomupd1_get_branch(object))) {
      g_debug("We are already tracking the branch %s, nothing to do", branch);
      return TRUE;
   }

   if (!_au_update_user_preferences(variant, branch, http_proxy, error))
      return FALSE;

   _au_clear_available_updates(object);
   au_atomupd1_set_branch(object, branch);

   return TRUE;
}

static void
au_switch_branch_authorized_cb(AuAtomupd1 *object,
                               GDBusMethodInvocation *invocation,
                               gpointer arg_branch_pointer)
{
   g_autoptr(GError) error = NULL;

   if (!_au_switch_to_branch(object, arg_branch_pointer, &error)) {
      g_dbus_method_invocation_return_error(
         g_steal_pointer(&invocation), G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
         "An error occurred while switching to the chosen branch: %s", error->message);
      return;
   }

   au_atomupd1_complete_switch_to_branch(object, g_steal_pointer(&invocation));
}

static gboolean
au_atomupd1_impl_handle_switch_to_branch(AuAtomupd1 *object,
                                         GDBusMethodInvocation *invocation,
                                         const gchar *arg_branch)
{
   _au_check_auth(object, "com.steampowered.atomupd1.switch-variant-or-branch",
                  au_switch_branch_authorized_cb, invocation, g_strdup(arg_branch),
                  g_free);

   return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gchar *
_au_get_http_proxy_address_and_port(AuAtomupd1 *object)
{
   const gchar *address = NULL;
   gint port;
   GVariant *http_proxy = au_atomupd1_get_http_proxy(object); /* borrowed */

   if (http_proxy == NULL)
      return NULL;

   g_variant_get(http_proxy, "(&si)", &address, &port);

   if (g_strcmp0(address, "") == 0)
      return NULL;

   return g_strdup_printf("%s:%i", address, port);
}

static gboolean
_au_download_remote_info(const AuAtomupd1Impl *atomupd, GError **error)
{
   g_autofree gchar *meta_url = NULL;
   g_autofree gchar *remote_info_url = NULL;
   g_autofree gchar *http_proxy = NULL;
   const gchar *release = NULL;
   const gchar *product = NULL;
   const gchar *architecture = NULL;
   const gchar *variant = NULL;
   JsonNode *json_node = NULL;     /* borrowed */
   JsonObject *json_object = NULL; /* borrowed */
   g_autoptr(JsonParser) parser = NULL;

   g_return_val_if_fail(atomupd != NULL, FALSE);
   g_return_val_if_fail(atomupd->manifest_path != NULL, FALSE);
   g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

   parser = json_parser_new();
   if (!json_parser_load_from_file(parser, atomupd->manifest_path, error))
      return FALSE;

   json_node = json_parser_get_root(parser);
   if (json_node == NULL)
      return au_throw_error(error, "failed to parse the manifest JSON \"%s\"",
                            atomupd->manifest_path);

   json_object = json_node_get_object(json_node);

   release = json_object_get_string_member_with_default(json_object, "release", NULL);
   product = json_object_get_string_member_with_default(json_object, "product", NULL);
   architecture = json_object_get_string_member_with_default(json_object, "arch", NULL);

   if (release == NULL || product == NULL || architecture == NULL)
      return au_throw_error(error,
                            "the manifest JSON \"%s\" does not have the expected keys",
                            atomupd->manifest_path);

   variant = au_atomupd1_get_variant((AuAtomupd1 *)atomupd);
   meta_url = _au_get_meta_url_from_default_config(atomupd, error);

   if (meta_url == NULL)
      return FALSE;

   remote_info_url = g_build_filename(meta_url, release, product, architecture, variant,
                                      AU_REMOTE_INFO, NULL);

   http_proxy = _au_get_http_proxy_address_and_port((AuAtomupd1 *)atomupd);

   if (!_au_download_file(_au_get_remote_info_path(), remote_info_url, http_proxy, error))
      return FALSE;

   return TRUE;
}

static gboolean
_au_select_and_load_configuration(AuAtomupd1Impl *atomupd, GError **error);

static void
au_check_for_updates_authorized_cb(AuAtomupd1 *object,
                                   GDBusMethodInvocation *invocation,
                                   gpointer arg_options_pointer)
{
   GVariant *arg_options = arg_options_pointer;
   const gchar *variant = NULL;
   const gchar *branch = NULL;
   g_autofree gchar *http_proxy = NULL;
   const gchar *key = NULL;
   GVariant *value = NULL;
   gboolean penultimate = FALSE;
   GVariantIter iter;
   GPid child_pid;
   g_autoptr(QueryData) data = au_query_data_new();
   g_auto(GStrv) launch_environ = g_get_environ();
   g_autoptr(GPtrArray) argv = NULL;
   g_autoptr(GError) error = NULL;
   AuAtomupd1Impl *self = AU_ATOMUPD1_IMPL(object);

   g_return_if_fail(self->config_path != NULL);
   g_return_if_fail(self->manifest_path != NULL);

   if (!g_file_test(_au_get_remote_info_path(), G_FILE_TEST_EXISTS)) {
      g_debug("We don't have a remote info file, trying to download it again...");
      if (_au_download_remote_info(self, &error)) {
         if (!_au_select_and_load_configuration(self, &error)) {
            g_dbus_method_invocation_return_error(
               g_steal_pointer(&invocation), G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
               "An error occurred while reloading the configuration, please fix your "
               "conf file and retry: %s",
               error->message);
            return;
         }
      } else {
         g_debug("Failed to download the remote info: %s", error->message);
         g_clear_error(&error);
      }
   }

   g_variant_iter_init(&iter, arg_options);

   while (g_variant_iter_loop(&iter, "{sv}", &key, &value)) {
      if (g_str_equal(key, "penultimate")) {
         if (g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN)) {
            penultimate = g_variant_get_boolean(value);
         } else {
            g_dbus_method_invocation_return_error(
               g_steal_pointer(&invocation), G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
               "The argument '%s' must have a boolean value", key);
         }
         continue;
      }

      g_dbus_method_invocation_return_error(
         g_steal_pointer(&invocation), G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
         "The argument '%s' is not a valid option", key);
      return;
   }

   http_proxy = _au_get_http_proxy_address_and_port(object);
   if (http_proxy != NULL) {
      launch_environ = g_environ_setenv(launch_environ, "https_proxy", http_proxy, TRUE);
      launch_environ = g_environ_setenv(launch_environ, "http_proxy", http_proxy, TRUE);
   }

   variant = au_atomupd1_get_variant(object);
   branch = au_atomupd1_get_branch(object);

   argv = g_ptr_array_new_with_free_func(g_free);
   g_ptr_array_add(argv, g_strdup("steamos-atomupd-client"));
   g_ptr_array_add(argv, g_strdup("--config"));
   g_ptr_array_add(argv, g_strdup(self->config_path));
   g_ptr_array_add(argv, g_strdup("--manifest-file"));
   g_ptr_array_add(argv, g_strdup(self->manifest_path));
   g_ptr_array_add(argv, g_strdup("--variant"));
   g_ptr_array_add(argv, g_strdup(variant));
   g_ptr_array_add(argv, g_strdup("--branch"));
   g_ptr_array_add(argv, g_strdup(branch));
   g_ptr_array_add(argv, g_strdup("--query-only"));
   g_ptr_array_add(argv, g_strdup("--estimate-download-size"));

   if (penultimate)
      g_ptr_array_add(argv, g_strdup("--penultimate-update"));

   if (g_debug_controller_get_debug_enabled(self->debug_controller))
      g_ptr_array_add(argv, g_strdup("--debug"));

   g_ptr_array_add(argv, NULL);

   if (!g_spawn_async_with_pipes(NULL, /* working directory */
                                 (gchar **)argv->pdata, launch_environ,
                                 G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                                 NULL,                         /* child setup */
                                 NULL,                         /* user data */
                                 &child_pid, NULL,             /* standard input */
                                 &data->standard_output, NULL, /* standard error */
                                 &error)) {
      g_dbus_method_invocation_return_error(
         g_steal_pointer(&invocation), G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
         "An error occurred calling the 'steamos-atomupd-client' helper: %s",
         error->message);
      return;
   }

   data->req->invocation = g_steal_pointer(&invocation);
   data->req->object = g_object_ref(object);
   g_child_watch_add(child_pid, on_query_completed, g_steal_pointer(&data));
}

static gboolean
au_atomupd1_impl_handle_check_for_updates(AuAtomupd1 *object,
                                          GDBusMethodInvocation *invocation,
                                          GVariant *arg_options)
{
   _au_check_auth(object, "com.steampowered.atomupd1.check-for-updates",
                  au_check_for_updates_authorized_cb, invocation,
                  g_variant_ref(arg_options), (GDestroyNotify)g_variant_unref);

   return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
_au_atomupd1_set_update_status_and_error(AuAtomupd1 *object,
                                         guint status,
                                         const gchar *error_code,
                                         const gchar *error_message)
{
   g_return_if_fail(object != NULL);

   au_atomupd1_set_update_status(object, status);
   au_atomupd1_set_failure_code(object, error_code);
   au_atomupd1_set_failure_message(object, error_message);
   g_dbus_interface_skeleton_flush(G_DBUS_INTERFACE_SKELETON(object));
}

/*
 * Returns: The RAUC service PID or -1 if an error occurred.
 */
static gint64
_au_get_rauc_service_pid(GError **error)
{
   g_autofree gchar *output = NULL;
   gchar *endptr = NULL;
   gint wait_status = 0;
   gint64 rauc_pid;

   const gchar *systemctl_argv[] = {
      "systemctl", "show", "--property", "MainPID", "rauc", NULL,
   };

   g_return_val_if_fail(error == NULL || *error == NULL, -1);

   if (!g_spawn_sync(NULL,                           /* working directory */
                     (gchar **)systemctl_argv, NULL, /* envp */
                     G_SPAWN_SEARCH_PATH, NULL,      /* child setup */
                     NULL,                           /* user data */
                     &output, NULL,                  /* stderr */
                     &wait_status, error)) {
      return -1;
   }

   if (!g_spawn_check_wait_status(wait_status, error))
      return -1;

   if (!g_str_has_prefix(output, "MainPID=")) {
      g_debug("Systemctl output is '%s' instead of the expected 'MainPID=X'", output);
      g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                  "An error occurred while trying to gather the RAUC PID");
      return -1;
   }

   rauc_pid = g_ascii_strtoll(output + strlen("MainPID="), &endptr, 10);
   if (endptr == NULL || output + strlen("MainPID=") == (const char *)endptr) {
      g_debug("Unable to parse Systemctl output: %s", output);
      g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                  "An error occurred while trying to gather the RAUC PID");
      return -1;
   }

   return rauc_pid;
}

/*
 * Returns: The @process PID or -1 if it's not available or an error occurred.
 */
static gint64
_au_get_process_pid(const gchar *process, GError **error)
{
   g_autofree gchar *output = NULL;
   gchar *endptr = NULL;
   gint wait_status = 0;
   gint64 pid;

   g_return_val_if_fail(process != NULL, -1);
   g_return_val_if_fail(error == NULL || *error == NULL, -1);

   const gchar *argv[] = {
      "pidof", "--single-shot", "-x", process, NULL,
   };

   if (!g_spawn_sync(NULL,                      /* working directory */
                     (gchar **)argv, NULL,      /* envp */
                     G_SPAWN_SEARCH_PATH, NULL, /* child setup */
                     NULL,                      /* user data */
                     &output, NULL,             /* stderr */
                     &wait_status, error))
      return -1;

   if (WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 1) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "There isn't a running process for %s", process);
      return -1;
   }

   if (!g_spawn_check_wait_status(wait_status, error))
      return -1;

   pid = g_ascii_strtoll(output, &endptr, 10);
   if (endptr == NULL || output == (const char *)endptr) {
      g_debug("Unable to parse pidof output: %s", output);
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "An error occurred while trying to gather the %s PID", process);
      return -1;
   }

   return pid;
}

static void
_au_ensure_pid_is_killed(GPid pid)
{
   gsize i;
   int status;
   int pgid = getpgid(pid);

   if (pid < 1)
      return;

   g_debug("Sending SIGTERM to PID %i", pid);

   if (kill(pid, SIGTERM) == 0) {
      /* The PIDs we are trying to stop usually do it in less than a second.
       * We wait up to 2s and, if they are still running, we will send a
       * SIGKILL. */
      for (i = 0; i < 4; i++) {
         if (waitpid(pid, &status, WNOHANG | WUNTRACED) > 0) {
            if (WIFEXITED(status))
               goto success;

            if (WIFSTOPPED(status)) {
               g_debug("PID %i is currently paused, sending SIGCONT to the group %i", pid,
                       pgid);
               killpg(pgid, SIGCONT);
            }
         } else {
            int saved_errno = errno;

            if (saved_errno == ESRCH)
               goto success;

            if (saved_errno == ECHILD) {
               /* The PID may not be our child, i.e. the rauc service.
                * It is still safe to kill it, because it is the process
                * responsible for applying an update and will gracefully
                * handle the kill(). When we'll try to apply another update
                * this service will be automatically executed again. */
               if (kill(pid, 0) != 0)
                  goto success;

               /* If this process is not our child, we can't use waitpid() and
                * WIFSTOPPED(). Instead we send a SIGCONT regardless of the status of the
                * process. */
               g_debug("Sending SIGCONT to the group %i to ensure that the PIDs are not "
                       "paused",
                       pgid);
               killpg(pgid, SIGCONT);
            }
         }

         g_debug("PID %i is still running", pid);
         g_usleep(0.5 * G_USEC_PER_SEC);
      }
   }

   g_debug("Sending SIGKILL to PID %i", pid);
   kill(pid, SIGKILL);
   waitpid(pid, NULL, 0);

success:
   g_debug("PID %i terminated successfully", pid);
}

static void
_au_cancel_async(GTask *task,
                 gpointer source_object,
                 gpointer data,
                 GCancellable *cancellable)
{
   gint64 rauc_pid;
   g_autoptr(GError) error = NULL;
   GPid pid = GPOINTER_TO_INT(data);

   /* The first thing to kill is the install helper. Otherwise, if we kill
    * RAUC while the helper is still running, the helper might execute RAUC
    * again before we are able to send the termination signal to the helper. */
   _au_ensure_pid_is_killed(pid);

   /* At the moment a RAUC operation can't be cancelled using its D-Bus API.
    * For this reason we get its PID number and send a SIGTERM/SIGKILL to it. */
   rauc_pid = _au_get_rauc_service_pid(&error);

   if (rauc_pid < 0) {
      g_task_return_error(task, g_steal_pointer(&error));
      return;
   }

   _au_ensure_pid_is_killed(rauc_pid);

   g_task_return_boolean(task, TRUE);
}

static void
cancel_callback(GObject *source_object, GAsyncResult *result, gpointer user_data)
{
   g_autoptr(GError) error = NULL;
   g_autoptr(RequestData) data = user_data;

   if (g_task_propagate_boolean(G_TASK(result), &error)) {
      au_atomupd1_set_update_status(data->object, AU_UPDATE_STATUS_CANCELLED);
      au_atomupd1_complete_cancel_update(data->object,
                                         g_steal_pointer(&data->invocation));
   } else {
      /* We failed to cancel a running update, probably the update is still
       * running, but we don't know for certain. */
      g_dbus_method_invocation_return_error(
         g_steal_pointer(&data->invocation), G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
         "Failed to cancel an update: %s", error->message);
   }
}

static void
au_cancel_update_authorized_cb(AuAtomupd1 *object,
                               GDBusMethodInvocation *invocation,
                               gpointer data_pointer)
{
   g_autoptr(RequestData) data = g_slice_new0(RequestData);
   g_autoptr(GTask) task = NULL;
   AuAtomupd1Impl *self = AU_ATOMUPD1_IMPL(object);

   if (au_atomupd1_get_update_status(object) != AU_UPDATE_STATUS_IN_PROGRESS &&
       au_atomupd1_get_update_status(object) != AU_UPDATE_STATUS_PAUSED) {
      g_dbus_method_invocation_return_error(
         g_steal_pointer(&invocation), G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
         "There isn't an update in progress that can be cancelled");
      return;
   }

   /* Remove the previous child watch callback */
   if (self->install_event_source != 0)
      g_source_remove(self->install_event_source);

   self->install_event_source = 0;

   data->invocation = g_steal_pointer(&invocation);
   data->object = g_object_ref(object);

   task = g_task_new(NULL, NULL, cancel_callback, g_steal_pointer(&data));

   g_task_set_task_data(task, GINT_TO_POINTER(self->install_pid), NULL);
   g_task_run_in_thread(task, _au_cancel_async);
}

static gboolean
au_atomupd1_impl_handle_cancel_update(AuAtomupd1 *object,
                                      GDBusMethodInvocation *invocation)
{
   _au_check_auth(object, "com.steampowered.atomupd1.manage-pending-update",
                  au_cancel_update_authorized_cb, invocation, NULL, NULL);

   return G_DBUS_METHOD_INVOCATION_HANDLED;
}

/*
 * _au_send_signal_to_install_procs:
 * @self: A AuAtomupd1Impl object
 * @sig: Signal that will be sent to the processes
 * @error: Used to raise an error on failure
 *
 * Send @sig to the install helper PID and to the RAUC service process group ID
 *
 * Returns: %TRUE if the signal was successfully sent
 */
static gboolean
_au_send_signal_to_install_procs(AuAtomupd1Impl *self, int sig, GError **error)
{
   gint64 rauc_pid;
   int saved_errno;

   g_return_val_if_fail(self != NULL, FALSE);
   g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

   if (self->install_pid == 0) {
      g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                  "Unexpectedly the PID of the install helper is not set");
      return FALSE;
   }

   g_debug("Sending signal %i to the install helper with PID %i", sig, self->install_pid);

   if (kill(self->install_pid, sig) < 0) {
      saved_errno = errno;
      g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                  "Unable to send signal %i to the update helper: %s", sig,
                  g_strerror(saved_errno));
      return FALSE;
   }

   rauc_pid = _au_get_rauc_service_pid(error);
   if (rauc_pid < 0)
      return FALSE;

   if (rauc_pid > 0) {
      /* Send the signal to the entire PGID, to include the eventual Desync process */
      int rauc_pgid = getpgid(rauc_pid);
      g_debug("Sending signal %i to the RAUC service PGID %i", sig, rauc_pgid);

      if (killpg(rauc_pgid, sig) < 0) {
         saved_errno = errno;
         g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                     "Unable to send signal %i to the RAUC service: %s", sig,
                     g_strerror(saved_errno));
         return FALSE;
      }
   }
   return TRUE;
}

static void
au_pause_update_authorized_cb(AuAtomupd1 *object,
                              GDBusMethodInvocation *invocation,
                              gpointer data_pointer)
{
   g_autoptr(GError) error = NULL;
   AuAtomupd1Impl *self = AU_ATOMUPD1_IMPL(object);

   if (au_atomupd1_get_update_status(object) != AU_UPDATE_STATUS_IN_PROGRESS) {
      g_dbus_method_invocation_return_error(
         g_steal_pointer(&invocation), G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
         "There isn't an update in progress that can be paused");
      return;
   }

   if (!_au_send_signal_to_install_procs(self, SIGSTOP, &error)) {
      g_dbus_method_invocation_return_error(
         g_steal_pointer(&invocation), error->domain, error->code,
         "An error occurred while attempting to pause the installation process: %s",
         error->message);
      return;
   }

   au_atomupd1_set_update_status(object, AU_UPDATE_STATUS_PAUSED);
   au_atomupd1_complete_pause_update(object, g_steal_pointer(&invocation));
}

static gboolean
au_atomupd1_impl_handle_pause_update(AuAtomupd1 *object,
                                     GDBusMethodInvocation *invocation)
{
   _au_check_auth(object, "com.steampowered.atomupd1.manage-pending-update",
                  au_pause_update_authorized_cb, invocation, NULL, NULL);

   return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
_au_client_stdout_update_cb(GObject *object_stream,
                            GAsyncResult *result,
                            gpointer user_data)
{
   GDataInputStream *stream = (GDataInputStream *)object_stream;
   g_autoptr(AuAtomupd1) object = user_data;
   AuAtomupd1Impl *self = AU_ATOMUPD1_IMPL(object);
   size_t len;
   const gchar *cursor = NULL;
   gchar *endptr = NULL;
   g_autoptr(GError) error = NULL;
   g_autofree gchar *line = NULL;
   g_auto(GStrv) parts = NULL;
   g_autoptr(GDateTime) time_estimation = g_date_time_new_now_utc();

   if (self->start_update_stdout_stream != stream)
      return;

   /* steamos-atomupd-client will periodically print updates regarding the
    * upgrade process. These updates are formatted as "XX.XX% DdHhMMmSSs".
    * The estimated remaining time may be missing if we are either using Casync
    * or if it is currently unknown.
    * Examples of valid values include: "15.85% 08m44s", "0.00%", "4.31% 00m56s",
    * "47.00% 1h12m05s" and "100%". */

   line = g_data_input_stream_read_line_finish(stream, result, NULL, &error);

   if (error != NULL) {
      g_debug("Unable to read the update progress: %s", error->message);
      return;
   }

   /* If there is nothing more to read, just return */
   if (line == NULL)
      return;

   parts = g_strsplit(g_strstrip(line), " ", 2);

   len = strlen(parts[0]);
   if (len < 2 || parts[0][len - 1] != '%') {
      g_debug("Unable to parse the completed percentage: %s", parts[0]);
      return;
   }

   /* Remove the percentage sign */
   parts[0][len - 1] = '\0';

   /* The percentage here is not locale dependent, we don't have to worry
    * about comma vs period for the decimals. */
   au_atomupd1_set_progress_percentage(object, g_ascii_strtod(parts[0], NULL));

   g_data_input_stream_read_line_async(stream, G_PRIORITY_DEFAULT, NULL,
                                       _au_client_stdout_update_cb, g_object_ref(object));

   if (parts[1] == NULL) {
      au_atomupd1_set_estimated_completion_time(object, 0);
      g_dbus_interface_skeleton_flush(G_DBUS_INTERFACE_SKELETON(object));
      return;
   }

   cursor = parts[1];
   while (*cursor != '\0') {
      g_autoptr(GDateTime) updated_estimation = NULL;
      guint64 value;

      endptr = NULL;
      value = g_ascii_strtoull(cursor, &endptr, 10);

      if (endptr == NULL || cursor == (const char *)endptr) {
         g_debug("Unable to parse the expected remaining time: %s", parts[1]);
         au_atomupd1_set_estimated_completion_time(object, 0);
         g_dbus_interface_skeleton_flush(G_DBUS_INTERFACE_SKELETON(object));
         return;
      }

      cursor = endptr + 1;

      switch (endptr[0]) {
      case 'd':
         updated_estimation = g_date_time_add_days(time_estimation, value);
         break;

      case 'h':
         updated_estimation = g_date_time_add_hours(time_estimation, value);
         break;

      case 'm':
         updated_estimation = g_date_time_add_minutes(time_estimation, value);
         break;

      case 's':
         updated_estimation = g_date_time_add_seconds(time_estimation, value);
         break;

      default:
         g_debug("Unable to parse the expected remaining time: %s", parts[1]);
         au_atomupd1_set_estimated_completion_time(object, 0);
         g_dbus_interface_skeleton_flush(G_DBUS_INTERFACE_SKELETON(object));
         return;
      }

      g_date_time_unref(time_estimation);
      time_estimation = g_steal_pointer(&updated_estimation);
   }

   au_atomupd1_set_estimated_completion_time(object,
                                             g_date_time_to_unix(time_estimation));
   g_dbus_interface_skeleton_flush(G_DBUS_INTERFACE_SKELETON(object));
}

static void
au_start_update_clear(AuAtomupd1Impl *self)
{
   g_clear_object(&self->start_update_stdout_stream);
   self->install_event_source = 0;
   self->install_pid = 0;
}

static void
child_watch_cb(GPid pid, gint wait_status, gpointer user_data)
{
   g_autoptr(AuAtomupd1) object = user_data;
   g_autoptr(GError) error = NULL;

   if (g_spawn_check_wait_status(wait_status, &error)) {
      g_debug("The update has been successfully applied");
      _au_atomupd1_set_update_status_and_error(object, AU_UPDATE_STATUS_SUCCESSFUL, NULL,
                                               NULL);
   } else {
      g_debug("'steamos-atomupd-client' helper returned an error: %s", error->message);
      _au_atomupd1_set_update_status_and_error(
         object, AU_UPDATE_STATUS_FAILED, "org.freedesktop.DBus.Error", error->message);
   }

   au_start_update_clear((AuAtomupd1Impl *)object);
}

/*
 * @buildid: The build ID to validate
 * @date_out: (out) (optional): Used to return the date of @buildid
 * @inc_out: (out) (optional): Used to return the incremental number of @buildid
 * @error: (out) (optional): Used to return an error on failure
 *
 * Parse @buildid and perform some sanity checks.
 * @buildid is expected to have a "date" part that follows the ISO-8601
 * standard, without hyphens. Optionally, after the date, there is a dot and
 * an increment. I.e. YYYYMMDD[.N]
 *
 * If this standard will ever change, please keep it in sync with the
 * BuildId class in `steamos-atomupd.git`
 *
 * Returns: %TRUE if the @buildid has the expected form
 */
gboolean
_is_buildid_valid(const gchar *buildid, gint64 *date_out, gint64 *inc_out, GError **error)
{
   gint64 month = 0;
   gint64 day = 0;
   gint64 inc = 0;
   gint64 date = 0;
   char *endptr;
   g_auto(GStrv) requested_parts = NULL;
   const gchar *date_str;
   gsize i;

   if (buildid == NULL || g_strcmp0(buildid, "") == 0)
      return au_throw_error(error, "The provided Buildid is either NULL or empty");

   requested_parts = g_strsplit(buildid, ".", 2);
   date_str = requested_parts[0];

   /* The date is expected to be in the form of YYYYMMDD, which
    * g_date_time_new_from_iso8601() doesn't cover because without the time part.
    * Instead, we parse it ourselves. */

   date = g_ascii_strtoll(date_str, &endptr, 10);
   if (date < 0 || date > G_MAXINT || endptr == date_str || *endptr != '\0') {
      return au_throw_error(
         error, "Buildid '%s' doesn't follow the expected YYYYMMDD[.N] format", buildid);
   }

   if (strlen(date_str) != 8) {
      return au_throw_error(
         error, "Buildid '%s' doesn't follow the expected YYYYMMDD[.N] format", buildid);
   }
   for (i = 4; i < 6; i++) {
      month = month * 10 + (date_str[i] - '0');
   }
   for (i = 6; i < 8; i++) {
      day = day * 10 + (date_str[i] - '0');
   }

   if (month > 12 || day > 31)
      return au_throw_error(error, "The date in the buildid '%s' is not valid", buildid);

   if (requested_parts[1] != NULL) {
      inc = g_ascii_strtoll(requested_parts[1], &endptr, 10);

      if (inc < 0 || inc > G_MAXINT || endptr == requested_parts[1] || *endptr != '\0') {
         return au_throw_error(
            error, "The increment part of the buildid is unexpected: '%s'", buildid);
      }
   }

   if (date_out != NULL)
      *date_out = date;
   if (inc_out != NULL)
      *inc_out = inc;

   return TRUE;
}

static void
au_start_update_authorized_cb(AuAtomupd1 *object,
                              GDBusMethodInvocation *invocation,
                              gpointer arg_id_pointer)
{
   const gchar *arg_id = arg_id_pointer;
   AuAtomupd1Impl *self = (AuAtomupd1Impl *)object;
   g_autoptr(GPtrArray) argv = NULL;
   g_autofree gchar *http_proxy = NULL;
   g_auto(GStrv) launch_environ = g_get_environ();
   g_autoptr(GFileIOStream) stream = NULL;
   g_autoptr(GInputStream) unix_stream = NULL;
   GVariant *updates_available = NULL; /* borrowed */
   g_autoptr(GVariantIter) updates_iter = NULL;
   gboolean found_buildid = FALSE;
   g_autoptr(GError) error = NULL;
   AuUpdateStatus current_status;
   gint client_stdout;

   current_status = au_atomupd1_get_update_status(object);
   if (current_status == AU_UPDATE_STATUS_IN_PROGRESS ||
       current_status == AU_UPDATE_STATUS_PAUSED) {
      g_dbus_method_invocation_return_error(g_steal_pointer(&invocation), G_DBUS_ERROR,
                                            G_DBUS_ERROR_FAILED,
                                            "Failed to start a new update because one "
                                            "is already in progress");
      return;
   }

   if (!g_file_query_exists(self->updates_json_file, NULL)) {
      g_dbus_method_invocation_return_error(g_steal_pointer(&invocation), G_DBUS_ERROR,
                                            G_DBUS_ERROR_FAILED,
                                            "It is not possible to start an update "
                                            "before calling \"CheckForUpdates\"");
      return;
   }

   au_atomupd1_set_update_build_id(object, arg_id);

   updates_available = au_atomupd1_get_updates_available(object);
   if (updates_available != NULL) {
      gchar *buildid = NULL;   /* borrowed */
      GVariant *values = NULL; /* borrowed */

      g_variant_get(updates_available, "a{?*}", &updates_iter);
      while (g_variant_iter_loop(updates_iter, "{s@a{sv}}", &buildid, &values)) {
         if (g_str_equal(arg_id, buildid)) {
            g_autoptr(GVariant) version = NULL;

            version = g_variant_lookup_value(values, "version", G_VARIANT_TYPE_STRING);
            au_atomupd1_set_update_version(object, g_variant_get_string(version, NULL));
            found_buildid = TRUE;
            break;
         }
      }
   }

   if (!found_buildid) {
      g_warning("The chosen buildid '%s' doesn't seem to be available, the update "
                "is expected to fail",
                arg_id);

      /* Clear any previous value we might have */
      au_atomupd1_set_update_version(object, NULL);
   }

   http_proxy = _au_get_http_proxy_address_and_port(object);
   if (http_proxy != NULL) {
      launch_environ = g_environ_setenv(launch_environ, "https_proxy", http_proxy, TRUE);
      launch_environ = g_environ_setenv(launch_environ, "http_proxy", http_proxy, TRUE);
   }

   /* Create a copy of the json file because we will pass that to the
    * 'steamos-atomupd-client' helper and, if in the meantime we check again
    * for updates, we may replace that file with a newer version. */

   if (self->updates_json_copy != NULL) {
      g_file_delete(self->updates_json_copy, NULL, NULL);
      g_clear_object(&self->updates_json_copy);
   }

   self->updates_json_copy =
      g_file_new_tmp("steamos-atomupd-XXXXXX.json", &stream, &error);
   if (self->updates_json_copy == NULL) {
      g_dbus_method_invocation_return_error(
         g_steal_pointer(&invocation), G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
         "Failed to create a copy of the JSON update file %s", error->message);
      return;
   }
   if (!g_file_copy(self->updates_json_file, self->updates_json_copy,
                    G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &error)) {
      g_dbus_method_invocation_return_error(
         g_steal_pointer(&invocation), G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
         "Failed to create a copy of the JSON update file %s", error->message);
      return;
   }

   argv = g_ptr_array_new_with_free_func(g_free);
   g_ptr_array_add(argv, g_strdup("steamos-atomupd-client"));
   g_ptr_array_add(argv, g_strdup("--config"));
   g_ptr_array_add(argv, g_strdup(self->config_path));
   g_ptr_array_add(argv, g_strdup("--update-file"));
   g_ptr_array_add(argv, g_file_get_path(self->updates_json_copy));
   g_ptr_array_add(argv, g_strdup("--update-version"));
   g_ptr_array_add(argv, g_strdup(arg_id));

   if (g_debug_controller_get_debug_enabled(self->debug_controller))
      g_ptr_array_add(argv, g_strdup("--debug"));

   g_ptr_array_add(argv, NULL);

   au_start_update_clear(self);
   if (!g_spawn_async_with_pipes(NULL, /* working directory */
                                 (gchar **)argv->pdata, launch_environ,
                                 G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                                 NULL,                     /* child setup */
                                 NULL,                     /* user data */
                                 &self->install_pid, NULL, /* standard input */
                                 &client_stdout, NULL,     /* standard error */
                                 &error)) {
      g_dbus_method_invocation_return_error(
         g_steal_pointer(&invocation), G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
         "Failed to launch the \"steamos-atomupd-client\" helper: %s", error->message);
      return;
   }

   unix_stream = g_unix_input_stream_new(client_stdout, TRUE);
   self->start_update_stdout_stream = g_data_input_stream_new(unix_stream);

   g_data_input_stream_read_line_async(self->start_update_stdout_stream,
                                       G_PRIORITY_DEFAULT, NULL,
                                       _au_client_stdout_update_cb, g_object_ref(object));

   self->install_event_source = g_child_watch_add(
      self->install_pid, (GChildWatchFunc)child_watch_cb, g_object_ref(object));

   au_atomupd1_set_progress_percentage(object, 0);
   _au_atomupd1_set_update_status_and_error(object, AU_UPDATE_STATUS_IN_PROGRESS, NULL,
                                            NULL);

   au_atomupd1_complete_start_update(object, g_steal_pointer(&invocation));
}

static gboolean
au_atomupd1_impl_handle_start_update(AuAtomupd1 *object,
                                     GDBusMethodInvocation *invocation,
                                     const gchar *arg_id)
{
   AuAtomupd1Impl *self = (AuAtomupd1Impl *)object;
   gint64 request_buildid_date;
   gint64 request_buildid_increment;
   const gchar *action_id = "com.steampowered.atomupd1.start-upgrade";
   g_autoptr(GError) error = NULL;

   if (!_is_buildid_valid(arg_id, &request_buildid_date, &request_buildid_increment,
                          &error)) {
      g_dbus_method_invocation_return_error_literal(
         g_steal_pointer(&invocation), G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
         error->message);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
   }

   if (request_buildid_date < self->buildid_date ||
       (request_buildid_date == self->buildid_date &&
        request_buildid_increment < self->buildid_increment)) {
      action_id = "com.steampowered.atomupd1.start-downgrade";
   }

   _au_check_auth(object, action_id, au_start_update_authorized_cb, invocation,
                  g_strdup(arg_id), g_free);

   return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
au_resume_update_authorized_cb(AuAtomupd1 *object,
                               GDBusMethodInvocation *invocation,
                               gpointer data_pointer)
{
   g_autoptr(GError) error = NULL;
   AuAtomupd1Impl *self = AU_ATOMUPD1_IMPL(object);

   if (au_atomupd1_get_update_status(object) != AU_UPDATE_STATUS_PAUSED) {
      g_dbus_method_invocation_return_error(
         g_steal_pointer(&invocation), G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
         "There isn't a paused update that can be resumed");
      return;
   }

   if (!_au_send_signal_to_install_procs(self, SIGCONT, &error)) {
      g_dbus_method_invocation_return_error(
         g_steal_pointer(&invocation), error->domain, error->code,
         "An error occurred while attempting to resume the installation process: %s",
         error->message);
      return;
   }

   au_atomupd1_set_update_status(object, AU_UPDATE_STATUS_IN_PROGRESS);
   au_atomupd1_complete_resume_update(object, g_steal_pointer(&invocation));
}

static gboolean
au_atomupd1_impl_handle_resume_update(AuAtomupd1 *object,
                                      GDBusMethodInvocation *invocation)
{
   _au_check_auth(object, "com.steampowered.atomupd1.manage-pending-update",
                  au_resume_update_authorized_cb, invocation, NULL, NULL);

   return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
debug_controller_authorize_cb(GDebugControllerDBus *debug_controller,
                              GDBusMethodInvocation *invocation,
                              gpointer user_data)
{
   /* Do not perform any additional check before authorizing this operation.
    * We don't print sensitive information in the debug output, so there is no
    * need to gate this behind polkit. */
   return TRUE;
}

static gboolean
_au_parse_preferences(AuAtomupd1Impl *atomupd, GError **error)
{
   g_autofree gchar *variant = NULL;
   g_autofree gchar *branch = NULL;
   g_autoptr(GVariant) http_proxy = NULL;
   const gchar *branch_file_path = _au_get_legacy_branch_file_path();
   const gchar *user_prefs_path = _au_get_user_preferences_file_path();
   g_autoptr(GError) local_error = NULL;

   g_return_val_if_fail(atomupd != NULL, FALSE);
   g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

   /* If we still have a legacy "steamos-branch" file, we try to load it first and
    * then convert it to the new preferences.conf */
   if (!_au_load_legacy_preferences(branch_file_path, &variant, &branch, &local_error)) {
      g_debug("%s", local_error->message);
      g_clear_error(&local_error);
   }

   /* Try preferences.conf if we couldn't load the legacy config */
   if (!variant && !_au_load_user_preferences_file(user_prefs_path, &variant, &branch,
                                                   &http_proxy, &local_error)) {
      g_debug("%s", local_error->message);
      g_clear_error(&local_error);
   }

   /* As our last resort we try to parse the image manifest file */
   if (!variant) {
      if (!_au_load_preferences_from_manifest(atomupd->manifest_path, &variant, &branch,
                                              error)) {
         return FALSE;
      }
   }

   if (http_proxy == NULL)
      http_proxy = g_variant_ref_sink(g_variant_new("(si)", "", 0));

   g_debug("Tracking the variant %s and branch %s", variant, branch);

   au_atomupd1_set_variant((AuAtomupd1 *)atomupd, variant);
   au_atomupd1_set_branch((AuAtomupd1 *)atomupd, branch);
   au_atomupd1_set_http_proxy((AuAtomupd1 *)atomupd, http_proxy);

   return TRUE;
}

static gboolean
_au_parse_manifest(AuAtomupd1Impl *atomupd, GError **error)
{
   g_autofree gchar *system_build_id = NULL;
   g_autofree gchar *system_version = NULL;

   g_return_val_if_fail(atomupd != NULL, FALSE);
   g_return_val_if_fail(atomupd->manifest_path != NULL, FALSE);
   g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

   system_build_id = _au_get_current_system_build_id(atomupd->manifest_path, error);
   if (system_build_id == NULL)
      return FALSE;
   system_version = _au_get_current_system_version(atomupd->manifest_path, error);
   if (system_version == NULL)
      return FALSE;

   if (!_is_buildid_valid(system_build_id, &atomupd->buildid_date,
                          &atomupd->buildid_increment, error))
      return FALSE;

   au_atomupd1_set_current_build_id((AuAtomupd1 *)atomupd, system_build_id);
   au_atomupd1_set_current_version((AuAtomupd1 *)atomupd, system_version);

   return TRUE;
}

static gboolean
_au_parse_config(AuAtomupd1Impl *atomupd, gboolean include_remote_info, GError **error)
{
   const gchar *server_mandatory_key[] = { "ImagesUrl", "MetaUrl", NULL };
   g_autofree gchar *username = NULL;
   g_autofree gchar *password = NULL;
   g_autofree gchar *auth_encoded = NULL;
   g_autofree gchar *default_variant = NULL;
   g_autofree gchar *default_branch = NULL;
   g_auto(GStrv) known_variants = NULL;
   g_auto(GStrv) known_branches = NULL;
   g_autoptr(GKeyFile) client_config = g_key_file_new();
   g_autoptr(GKeyFile) remote_info = g_key_file_new();
   g_autoptr(GError) local_error = NULL;
   gsize i;

   g_return_val_if_fail(atomupd != NULL, FALSE);
   g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

   if (!g_key_file_load_from_file(client_config, atomupd->config_path, G_KEY_FILE_NONE,
                                  error))
      return FALSE;

   /* Ensure that the configuration has all the mandatory fields */
   for (i = 0; server_mandatory_key[i] != NULL; i++) {
      if (!g_key_file_has_key(client_config, "Server", server_mandatory_key[i], NULL)) {
         return au_throw_error(
            error, "The config file \"%s\" doesn't have the expected \"%s\" entry",
            atomupd->config_path, server_mandatory_key[i]);
      }
   }

   if (include_remote_info &&
       !g_key_file_load_from_file(remote_info, _au_get_remote_info_path(),
                                  G_KEY_FILE_NONE, &local_error)) {
      /* This could happen if for example the Steam Deck is in offline mode, or the server
       * doesn't have a remote info file at all. In those cases we simply continue to use
       * the local info */
      g_debug("Failed to use the additional remote info: %s", local_error->message);
      g_debug("Continuing anyway...");
      g_clear_error(&local_error);
   }

   g_debug("Getting the list of known variants and branches");

   if (include_remote_info) {
      known_variants = _au_get_known_variants_from_config(remote_info, NULL);
      if (known_variants != NULL)
         g_debug("Using the list of known variants from the remote info file");
   }

   if (known_variants == NULL) {
      known_variants = _au_get_known_variants_from_config(client_config, error);
      if (known_variants == NULL)
         return FALSE;
   }

   /* As a security measure against misconfigurations, we ensure that in the list of
    * known variants there is at least the default variant */
   default_variant = _au_get_default_variant(atomupd->manifest_path, NULL);
   if (default_variant != NULL &&
       !g_strv_contains((const gchar *const *)known_variants, default_variant)) {
      gsize length = g_strv_length(known_variants);

      known_variants = g_realloc(known_variants, (length + 2) * sizeof(gchar *));
      known_variants[length] = g_steal_pointer(&default_variant);
      known_variants[length + 1] = NULL;
   }

   au_atomupd1_set_known_variants((AuAtomupd1 *)atomupd,
                                  (const gchar *const *)known_variants);

   if (include_remote_info) {
      known_branches = _au_get_known_branches_from_config(remote_info, NULL);
      if (known_branches != NULL)
         g_debug("Using the list of known branches from the remote info file");
   }

   if (known_branches == NULL) {
      known_branches = _au_get_known_branches_from_config(client_config, error);
      if (known_branches == NULL)
         return FALSE;
   }

   /* As a security measure against misconfigurations, we ensure that in the list of
    * known branches there is at least the default branch */
   default_branch = _au_get_default_branch(atomupd->manifest_path);
   if (!g_strv_contains((const gchar *const *)known_branches, default_branch)) {
      gsize length = g_strv_length(known_branches);

      known_branches = g_realloc(known_branches, (length + 2) * sizeof(gchar *));
      known_branches[length] = g_steal_pointer(&default_branch);
      known_branches[length + 1] = NULL;
   }

   au_atomupd1_set_known_branches((AuAtomupd1 *)atomupd,
                                  (const gchar *const *)known_branches);

   /* If the config has an HTTP auth, we need to ensure that netrc and Desync
    * have it too */
   if (_au_get_http_auth_from_config(client_config, &username, &password,
                                     &auth_encoded)) {
      g_autoptr(GHashTable) url_table = NULL;
      g_autoptr(GList) urls = NULL;
      const gchar *images_url;

      url_table = _au_get_urls_from_config(client_config, error);
      if (url_table == NULL) {
         /* The config file is malformed, bail out */
         g_warning("Failed to get the list of URLs from %s", atomupd->config_path);
         return FALSE;
      }
      urls = g_hash_table_get_values(url_table);

      if (!_au_ensure_urls_in_netrc(AU_NETRC_PATH, urls, username, password, error))
         return FALSE;

      images_url = g_hash_table_lookup(url_table, "ImagesUrl");
      if (images_url == NULL) {
         /* The ImagesUrl entry is mandatory for a valid config file */
         au_throw_error(
            error, "The config file \"%s\" doesn't have the expected \"ImagesUrl\" entry",
            atomupd->config_path);
         return FALSE;
      }

      if (!_au_ensure_url_in_desync_conf(AU_DESYNC_CONFIG_PATH, images_url, auth_encoded,
                                         error))
         return FALSE;
   }

   return TRUE;
}

static gboolean
_au_select_and_load_configuration(AuAtomupd1Impl *atomupd, GError **error)
{
   g_autofree gchar *dev_config_path = NULL;
   g_autoptr(GError) local_error = NULL;

   g_return_val_if_fail(atomupd != NULL, FALSE);
   g_return_val_if_fail(atomupd->config_directory != NULL, FALSE);
   g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

   g_clear_pointer(&atomupd->config_path, g_free);

   dev_config_path = g_build_filename(atomupd->config_directory, AU_DEV_CONFIG, NULL);
   if (g_file_test(dev_config_path, G_FILE_TEST_EXISTS)) {
      atomupd->config_path = g_steal_pointer(&dev_config_path);

      /* We don't load the remote info file when using a development configuration.
       * We could have additional custom variants/branches, and we don't want to replace
       * them with the ones from the server side. */
      if (_au_parse_config(atomupd, FALSE, &local_error)) {
         g_debug("Loaded the configuration file '%s'", atomupd->config_path);
         return TRUE;
      }

      g_warning("Failed to load '%s': %s\nUsing '%s' as a fallback.", AU_DEV_CONFIG,
                local_error->message, AU_CONFIG);
      g_clear_error(&local_error);
      g_clear_pointer(&atomupd->config_path, g_free);
   }

   atomupd->config_path = g_build_filename(atomupd->config_directory, AU_CONFIG, NULL);
   if (_au_parse_config(atomupd, TRUE, &local_error)) {
      g_debug("Loaded the configuration file '%s'", atomupd->config_path);
      return TRUE;
   }

   /* If we can't load the configuration file, as a last resort, we try the hardcoded
    * AU_FALLBACK_CONFIG_PATH. This is a last attempt to avoid breaking the atomic
    * updates in use cases where we have an invalid configuration file in the canonical
    * path. */
   g_warning(
      "Failed to load '%s': %s\n Using the hardcoded path '%s' as a last resort attempt.",
      atomupd->config_path, local_error->message, _au_get_fallback_config_path());
   g_clear_error(&local_error);
   g_clear_pointer(&atomupd->config_path, g_free);

   atomupd->config_path =
      g_build_filename(_au_get_fallback_config_path(), AU_CONFIG, NULL);

   return _au_parse_config(atomupd, TRUE, error);
}

static void
au_reload_configuration_authorized_cb(AuAtomupd1 *object,
                                      GDBusMethodInvocation *invocation,
                                      gpointer arg_options_pointer)
{
   g_autoptr(GError) error = NULL;
   AuAtomupd1Impl *self = AU_ATOMUPD1_IMPL(object);

   _au_clear_available_updates(object);

   if (!_au_select_and_load_configuration(self, &error)) {
      g_dbus_method_invocation_return_error(
         g_steal_pointer(&invocation), G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
         "An error occurred while reloading the configuration, please fix your conf file "
         "and retry: %s",
         error->message);
      return;
   }

   au_atomupd1_complete_reload_configuration(object, g_steal_pointer(&invocation));
}

static gboolean
au_atomupd1_impl_handle_reload_configuration(AuAtomupd1 *object,
                                             GDBusMethodInvocation *invocation,
                                             GVariant *arg_options)
{
   _au_check_auth(object, "com.steampowered.atomupd1.reload-configuration",
                  au_reload_configuration_authorized_cb, invocation,
                  g_variant_ref(arg_options), (GDestroyNotify)g_variant_unref);

   return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
au_enable_http_proxy_authorized_cb(AuAtomupd1 *object,
                                   GDBusMethodInvocation *invocation,
                                   gpointer arg_proxy_data_pointer)
{
   GVariant *arg_proxy_data = arg_proxy_data_pointer;
   const gchar *variant = au_atomupd1_get_variant(object);
   const gchar *branch = au_atomupd1_get_branch(object);
   g_autoptr(GError) error = NULL;

   if (!_au_update_user_preferences(variant, branch, arg_proxy_data, &error)) {
      g_dbus_method_invocation_return_error(
         g_steal_pointer(&invocation), G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
         "An error occurred while enabling the HTTP proxy : %s", error->message);
      return;
   }

   au_atomupd1_set_http_proxy(object, arg_proxy_data);

   au_atomupd1_complete_enable_http_proxy(object, g_steal_pointer(&invocation));
}

static gboolean
au_atomupd1_impl_handle_enable_http_proxy(AuAtomupd1 *object,
                                          GDBusMethodInvocation *invocation,
                                          const gchar *arg_address,
                                          gint arg_port,
                                          GVariant *arg_options)
{
   g_autoptr(GVariant) proxy_data =
      g_variant_ref_sink(g_variant_new("(si)", arg_address, arg_port));

   _au_check_auth(object, "com.steampowered.atomupd1.manage-http-proxy",
                  au_enable_http_proxy_authorized_cb, invocation,
                  g_steal_pointer(&proxy_data), (GDestroyNotify)g_variant_unref);

   return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
au_disable_http_proxy_authorized_cb(AuAtomupd1 *object,
                                    GDBusMethodInvocation *invocation,
                                    gpointer data_pointer)
{
   const gchar *variant = au_atomupd1_get_variant(object);
   const gchar *branch = au_atomupd1_get_branch(object);
   g_autoptr(GVariant) proxy_data = g_variant_ref_sink(g_variant_new("(si)", "", 0));
   g_autoptr(GError) error = NULL;

   if (!_au_update_user_preferences(variant, branch, NULL, &error)) {
      g_dbus_method_invocation_return_error(
         g_steal_pointer(&invocation), G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
         "An error occurred while disabling the HTTP proxy : %s", error->message);
      return;
   }

   au_atomupd1_set_http_proxy(object, proxy_data);

   au_atomupd1_complete_disable_http_proxy(object, g_steal_pointer(&invocation));
}

static gboolean
au_atomupd1_impl_handle_disable_http_proxy(AuAtomupd1 *object,
                                           GDBusMethodInvocation *invocation)
{
   _au_check_auth(object, "com.steampowered.atomupd1.manage-http-proxy",
                  au_disable_http_proxy_authorized_cb, invocation, NULL, NULL);

   return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
init_atomupd1_iface(AuAtomupd1Iface *iface)
{
   iface->handle_cancel_update = au_atomupd1_impl_handle_cancel_update;
   iface->handle_check_for_updates = au_atomupd1_impl_handle_check_for_updates;
   iface->handle_pause_update = au_atomupd1_impl_handle_pause_update;
   iface->handle_reload_configuration = au_atomupd1_impl_handle_reload_configuration;
   iface->handle_resume_update = au_atomupd1_impl_handle_resume_update;
   iface->handle_start_update = au_atomupd1_impl_handle_start_update;
   iface->handle_switch_to_variant = au_atomupd1_impl_handle_switch_to_variant;
   iface->handle_switch_to_branch = au_atomupd1_impl_handle_switch_to_branch;
   iface->handle_enable_http_proxy = au_atomupd1_impl_handle_enable_http_proxy;
   iface->handle_disable_http_proxy = au_atomupd1_impl_handle_disable_http_proxy;
}

G_DEFINE_TYPE_WITH_CODE(AuAtomupd1Impl,
                        au_atomupd1_impl,
                        AU_TYPE_ATOMUPD1_SKELETON,
                        G_IMPLEMENT_INTERFACE(AU_TYPE_ATOMUPD1, init_atomupd1_iface))

static void
au_atomupd1_impl_finalize(GObject *object)
{
   AuAtomupd1Impl *self = (AuAtomupd1Impl *)object;

   g_free(self->config_path);
   g_free(self->config_directory);
   g_free(self->manifest_path);
   g_clear_object(&self->authority);

   // Keep the update file, to be able to reuse it later on
   g_clear_object(&self->updates_json_file);

   if (self->updates_json_copy != NULL) {
      g_file_delete(self->updates_json_copy, NULL, NULL);
      g_clear_object(&self->updates_json_copy);
   }

   g_clear_signal_handler(&self->debug_controller_id, self->debug_controller);
   g_clear_object(&self->debug_controller);

   au_start_update_clear(self);

   G_OBJECT_CLASS(au_atomupd1_impl_parent_class)->finalize(object);
}

static void
au_atomupd1_impl_class_init(AuAtomupd1ImplClass *klass)
{
   GObjectClass *object_class = G_OBJECT_CLASS(klass);

   object_class->finalize = au_atomupd1_impl_finalize;
}

static void
au_atomupd1_impl_init(AuAtomupd1Impl *self)
{
}

/*
 * _str_rstrip_newline:
 * @string:
 *
 * Remove trailing newlines. @string is modified in place.
 */
static gchar *
_str_rstrip_newline(gchar *string)
{
   gssize i;

   g_return_val_if_fail(string != NULL, NULL);

   for (i = strlen(string) - 1; i > 0; i--) {
      if (string[i] == '\n')
         string[i] = '\0';
      else
         break;
   }

   return string;
}

/**
 * au_atomupd1_impl_new:
 * @config_preference: (transfer none) (not nullable): Path to the directory where
 *  the configuration is located.
 * @manifest_preference: (transfer none) (nullable): Path to a custom JSON manifest
 *  file. If %NULL, the path will be the default manifest path.
 * @error: Used to raise an error on failure
 *
 * Returns: (transfer full): a new AuAtomupd1
 */
AuAtomupd1 *
au_atomupd1_impl_new(const gchar *config_directory,
                     const gchar *manifest_preference,
                     GDBusConnection *bus,
                     GError **error)
{
   g_autofree gchar *reboot_content = NULL;
   g_autoptr(GFile) updates_json_parent = NULL;
   const gchar *updates_json_path;
   const gchar *reboot_for_update;
   g_autoptr(GError) local_error = NULL;
   gint64 client_pid = -1;
   gint64 rauc_pid = -1;
   AuAtomupd1Impl *atomupd = g_object_new(AU_TYPE_ATOMUPD1_IMPL, NULL);

   g_return_val_if_fail(config_directory != NULL, NULL);

   atomupd->authority = polkit_authority_get_sync(NULL, error);
   if (atomupd->authority == NULL)
      return NULL;

   atomupd->config_directory = g_strdup(config_directory);

   if (manifest_preference == NULL)
      atomupd->manifest_path = g_strdup(AU_DEFAULT_MANIFEST);
   else
      atomupd->manifest_path = g_strdup(manifest_preference);

   if (!_au_parse_preferences(atomupd, error))
      return NULL;

   if (!_au_parse_manifest(atomupd, error))
      return NULL;

   if (!_au_download_remote_info(atomupd, &local_error)) {
      g_info("Failed to download the remote info: %s", local_error->message);
      g_info("Continuing anyway...");
      g_clear_error(&local_error);
   }

   if (!_au_select_and_load_configuration(atomupd, error))
      return NULL;

   /* This environment variable is used for debugging and automated tests */
   updates_json_path = g_getenv("AU_UPDATES_JSON_FILE");
   if (updates_json_path == NULL)
      updates_json_path = AU_DEFAULT_UPDATE_JSON;

   atomupd->updates_json_file = g_file_new_for_path(updates_json_path);

   updates_json_parent = g_file_get_parent(atomupd->updates_json_file);
   if (!g_file_make_directory_with_parents(updates_json_parent, NULL, &local_error)) {
      if (local_error->code != G_IO_ERROR_EXISTS) {
         g_propagate_error(error, g_steal_pointer(&local_error));
         return NULL;
      }

      g_clear_error(&local_error);
   }

   au_atomupd1_set_version((AuAtomupd1 *)atomupd, ATOMUPD_VERSION);

   client_pid = _au_get_process_pid("steamos-atomupd-client", &local_error);
   if (client_pid > -1) {
      g_debug(
         "There is already a steamos-atomupd-client process running, stopping it...");
      _au_ensure_pid_is_killed(client_pid);
   } else {
      g_debug("%s", local_error->message);
      g_clear_error(&local_error);
   }

   g_debug("Stopping the RAUC service, if it's running...");
   rauc_pid = _au_get_rauc_service_pid(NULL);
   _au_ensure_pid_is_killed(rauc_pid);

   /* This environment variable is used for debugging and automated tests */
   reboot_for_update = g_getenv("AU_REBOOT_FOR_UPDATE");
   if (reboot_for_update == NULL)
      reboot_for_update = AU_REBOOT_FOR_UPDATE;

   if (g_file_get_contents(reboot_for_update, &reboot_content, NULL, NULL)) {
      g_auto(GStrv) splitted = NULL;

      g_debug("An update has already been successfully installed, it will be applied at "
              "the next reboot");

      splitted = g_strsplit(reboot_content, "-", 2);
      if (splitted[0] != NULL) {
         splitted[0] = _str_rstrip_newline(g_strstrip(splitted[0]));
         au_atomupd1_set_update_build_id((AuAtomupd1 *)atomupd, splitted[0]);
      }

      if (splitted[0] != NULL && splitted[1] != NULL) {
         splitted[1] = _str_rstrip_newline(g_strstrip(splitted[1]));
         au_atomupd1_set_update_version((AuAtomupd1 *)atomupd, splitted[1]);
      }

      au_atomupd1_set_update_status((AuAtomupd1 *)atomupd, AU_UPDATE_STATUS_SUCCESSFUL);
   }

   if (g_file_query_exists(atomupd->updates_json_file, NULL)) {
      g_autoptr(JsonParser) parser = json_parser_new();

      json_parser_load_from_file(parser, updates_json_path, &local_error);
      if (local_error != NULL) {
         /* This is not a critical issue. We try to continue because the next time
          * CheckForUpdates is called we will replace this unexpected file */
         g_warning("Unable to parse the existing updates JSON file: %s",
                   local_error->message);
         g_clear_error(&local_error);
      } else {
         const gchar *updated_build_id;
         g_autoptr(GVariant) available = NULL;
         g_autoptr(GVariant) available_later = NULL;
         g_autofree gchar *replacement_eol_variant = NULL;
         JsonNode *root = json_parser_get_root(parser); /* borrowed */

         updated_build_id = au_atomupd1_get_update_build_id((AuAtomupd1 *)atomupd);

         if (root == NULL) {
            g_info("The existing JSON file seems to be empty");
         } else if (!_au_parse_candidates(root, updated_build_id, &available,
                                          &available_later, &replacement_eol_variant,
                                          &local_error)) {
            g_warning("Unable to parse the existing updates JSON file: %s",
                      local_error->message);
            g_clear_error(&local_error);
         } else {
            au_atomupd1_set_updates_available((AuAtomupd1 *)atomupd, available);
            au_atomupd1_set_updates_available_later((AuAtomupd1 *)atomupd,
                                                    available_later);
         }

         if (replacement_eol_variant != NULL) {
            g_debug("Switching from the EOL variant %s to its replacement %s",
                    au_atomupd1_get_variant((AuAtomupd1 *)atomupd),
                    replacement_eol_variant);

            if (!_au_switch_to_variant((AuAtomupd1 *)atomupd, replacement_eol_variant,
                                       FALSE, &local_error)) {
               g_warning("An error occurred while switching to the new variant '%s': %s",
                         replacement_eol_variant, local_error->message);
               g_clear_error(&local_error);
               _au_clear_available_updates((AuAtomupd1 *)atomupd);
            }
         }
      }
   }

   atomupd->debug_controller =
      G_DEBUG_CONTROLLER(g_debug_controller_dbus_new(bus, NULL, error));
   if (atomupd->debug_controller == NULL)
      return NULL;

   atomupd->debug_controller_id =
      g_signal_connect(atomupd->debug_controller, "authorize",
                       G_CALLBACK(debug_controller_authorize_cb), NULL);

   return (AuAtomupd1 *)atomupd;
}
