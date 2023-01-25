/*
 * Copyright © 2021-2022 Collabora Ltd.
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

#include <glib.h>
#include <glib/gstdio.h>
#include <glib-unix.h>
#include <gio/gio.h>
#include <gio/gunixinputstream.h>

#include "utils.h"
#include "au-atomupd1-impl.h"

#include <json-glib/json-glib.h>

struct _AuAtomupd1Impl
{
  AuAtomupd1Skeleton parent_instance;

  GPid install_pid;
  guint install_event_source;
  gchar *config_path;
  gchar *manifest_path;
  GFile *updates_json_file;
  GFile *updates_json_copy;
  GFileMonitor *variant_monitor;
  gulong changed_id;
  GDataInputStream *start_update_stdout_stream;
};

typedef struct
{
  AuAtomupd1 *object;
  GDBusMethodInvocation *invocation;
} RequestData;

typedef struct
{
  RequestData *req;
  gint standard_output;
} QueryData;

typedef struct
{
  const gchar *expanded;
  const gchar *contracted;
} VariantConversion;

/* This is the same contracted->expanded relation that steamos-update uses */
static const VariantConversion variant_conversions[] =
{
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
_request_data_free (RequestData *self)
{
  if (self->invocation != NULL)
      g_dbus_method_invocation_return_error (g_steal_pointer (&self->invocation),
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Request was freed without being handled");

  g_clear_object (&self->object);

  g_slice_free (RequestData, self);
}

static void
_query_data_free (QueryData *self)
{
  _request_data_free (self->req);

  if (self->standard_output > -1)
    g_close (self->standard_output, NULL);

  g_slice_free (QueryData, self);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (RequestData, _request_data_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (QueryData, _query_data_free)

static QueryData *
au_query_data_new (void)
{
  QueryData *data = g_slice_new0 (QueryData);
  data->standard_output = -1;

  data->req = g_slice_new0 (RequestData);

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
_au_get_expanded_variant (const gchar *variant)
{
  gsize i;

  g_return_val_if_fail (variant != NULL, NULL);

  for (i = 0; i < G_N_ELEMENTS (variant_conversions); i++)
    {
      if (g_strcmp0 (variant, variant_conversions[i].contracted) == 0)
        return g_strdup (variant_conversions[i].expanded);
    }

  g_debug ("The variant %s doesn't need to be expanded", variant);
  return g_strdup (variant);
}

/*
 * _au_get_legacy_contracted_variant:
 * @variant: (not nullable): Variant to contract
 *
 * In Jupiter we historically stored the chosen variant in a contracted
 * form. This method will convert the canonical expanded variant into the
 * contracted version.
 *
 * If the variant isn't one that required to be contracted, a copy of
 * @variant will be returned.
 *
 * Returns: (transfer full): The contracted version of @variant, or a copy of
 *  @variant, if it doesn't need to be contracted.
 */
static gchar *
_au_get_legacy_contracted_variant (const gchar *variant)
{
  gsize i;

  g_return_val_if_fail (variant != NULL, NULL);

  for (i = 0; i < G_N_ELEMENTS (variant_conversions); i++)
    {
      if (g_strcmp0 (variant, variant_conversions[i].expanded) == 0)
        return g_strdup (variant_conversions[i].contracted);
    }

  g_debug ("The variant %s doesn't have a legacy contracted version", variant);
  return g_strdup (variant);
}

static const gchar *
_au_get_branch_file_path (void)
{
  static const gchar *branch_file = NULL;

  if (branch_file == NULL)
    {
      /* This environment variable is used for debugging and automated tests */
      branch_file = g_getenv ("AU_CHOSEN_BRANCH_FILE");

      if (branch_file == NULL)
        branch_file = AU_DEFAULT_BRANCH_PATH;
    }

  return branch_file;
}

static gchar * _au_get_default_variant (const gchar *manifest,
                                        GError **error);

/*
 * _au_get_chosen_variant:
 * @manifest_path: (not nullable): Path to the configuration file
 * @error: Used to raise an error on failure
 *
 * Retrieve the variant that is currently being tracked. This value is taken
 * by parsing the branch file. If the file is empty or not available, it will
 * be returned the default variant from the JSON manifest file.
 *
 * Returns: (transfer full): The chosen variant that is currently being
 *  tracked
 */
static gchar *
_au_get_chosen_variant (const gchar *manifest_path,
                        GError **error)
{
  g_autofree gchar *variant = NULL;
  g_autoptr(GError) local_error = NULL;
  const char *search;
  gsize len;

  if (!g_file_get_contents (_au_get_branch_file_path (), &variant, &len, &local_error))
    {
      if (local_error->code != G_FILE_ERROR_NOENT)
        {
          g_propagate_error (error, g_steal_pointer (&local_error));
          return NULL;
        }

      g_clear_error (&local_error);
      return _au_get_default_variant (manifest_path, error);
    }

  if (len == 0)
    return _au_get_default_variant (manifest_path, error);

  /* Remove eventual trailing newline that might be added by steamos-select-branch */
  if (variant[len-1] == '\n')
    variant[len-1] = '\0';

  search = strstr (variant, "\n");
  if (search != NULL)
    {
      /* If we have multiple newlines the file is likely malformed */
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to parse the branch file path \"%s\"",
                   _au_get_branch_file_path ());
      return NULL;
    }

  return g_steal_pointer (&variant);
}

/*
 * _au_set_variant:
 *
 * Update the Variant property with the expanded version of @variant and
 * clear the eventual available updates list.
 */
static void
_au_set_variant (AuAtomupd1 *object,
                 const gchar *variant)
{
  g_autoptr(GVariant) available = NULL;
  g_autoptr(GVariant) available_later = NULL;
  g_autofree gchar *expanded_variant = _au_get_expanded_variant (variant);

  if (g_strcmp0 (expanded_variant, au_atomupd1_get_variant (object)) == 0)
    {
      g_debug ("We are already tracking the variant %s, nothing to do", expanded_variant);
      return;
    }

  g_debug ("%s is the new chosen variant", expanded_variant);

  available = g_variant_new ("a{sa{sv}}", NULL);
  available_later = g_variant_new ("a{sa{sv}}", NULL);
  au_atomupd1_set_versions_available (object, g_steal_pointer (&available));
  au_atomupd1_set_versions_available_later (object, g_steal_pointer (&available_later));
  au_atomupd1_set_variant (object, expanded_variant);
}

/*
 * _au_variant_changed_cb:
 *
 * When the branch file changes, the new content is parsed and reflected
 * in the "Variant" property.
 */
static void
_au_variant_changed_cb (GFileMonitor *monitor,
                        GFile *file,
                        GFile *other_file,
                        GFileMonitorEvent event_type,
                        AuAtomupd1 *atomupd)
{
  AuAtomupd1Impl *self = AU_ATOMUPD1_IMPL (atomupd);
  g_autoptr(GError) error = NULL;
  g_autofree gchar *variant = NULL;

  g_debug ("The chosen branch file changed (monitor event %i)", event_type);

  if (event_type == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT)
    {
      variant = _au_get_chosen_variant (self->manifest_path, &error);
      if (variant == NULL)
        {
          g_warning ("An error occurred while loading the updated chosen variant: %s",
                     error->message);
          return;
        }

      _au_set_variant (atomupd, variant);
    }
}

/*
 * _au_get_manifest_path_from_config:
 * @client_config: (not nullable): Object that holds the configuration key file
 *
 * Returns: (type filename) (transfer full): The configured path to the manifest,
 *  or %NULL if the configuration doesn't have the "Manifest" field.
 */
static gchar *
_au_get_manifest_path_from_config (GKeyFile *client_config)
{
  g_return_val_if_fail (client_config != NULL, NULL);

  return g_key_file_get_string (client_config, "Host", "Manifest", NULL);
}

/*
 * _au_get_known_variants_from_config:
 * @client_config: (not nullable): Object that holds the configuration key file
 * @error: Used to raise an error on failure
 *
 * The only allowed symbols for the known variants are lowercase
 * and uppercase word characters, numbers, underscore, hyphen and the semicolon
 * as a separator.
 * Eventual variants that have symbols outside those allowed, will be
 * skipped.
 *
 * Returns: (array zero-terminated=1) (transfer full) (nullable): The list
 *  of known variants, or %NULL if the configuration doesn't have the
 *  "Variants" field.
 */
static gchar **
_au_get_known_variants_from_config (GKeyFile *client_config,
                                    GError **error)
{
  g_autoptr(GRegex) regex;
  g_autoptr(GPtrArray) valid_variants = NULL;
  g_auto(GStrv) variants = NULL;
  g_return_val_if_fail (client_config != NULL, NULL);

  regex = g_regex_new ("^[a-zA-Z0-9_-]+$", 0, 0, NULL);
  g_assert (regex != NULL);    /* known to be valid at compile-time */

  valid_variants = g_ptr_array_new_with_free_func (g_free);

  variants = g_key_file_get_string_list (client_config, "Server", "Variants", NULL, error);
  if (variants == NULL)
    return NULL;

  /* Sanitize the input. This helps us to skip improper/unexpected user inputs */
  for (gsize i = 0; variants[i] != NULL; i++)
    {
      if (g_regex_match (regex, variants[i], 0, NULL))
        g_ptr_array_add (valid_variants, g_strdup (variants[i]));
      else
        g_warning ("The variant \"%s\" has characters that are not allowed, skipping...",
                   variants[i]);
    }

  g_ptr_array_add (valid_variants, NULL);

  return (gchar **) g_ptr_array_free (g_steal_pointer (&valid_variants), FALSE);
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
_au_get_string_from_manifest (const gchar *manifest,
                              const gchar *key,
                              GError **error)
{
  const gchar *value = NULL;
  JsonNode *json_node = NULL;  /* borrowed */
  JsonObject *json_object = NULL;  /* borrowed */
  g_autoptr(JsonParser) parser = NULL;

  g_return_val_if_fail (manifest != NULL, NULL);
  g_return_val_if_fail (key != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  parser = json_parser_new ();
  if (!json_parser_load_from_file (parser, manifest, error))
    return NULL;

  json_node = json_parser_get_root (parser);
  if (json_node == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "failed to parse the manifest JSON \"%s\"", manifest);
      return NULL;
    }

  json_object = json_node_get_object (json_node);
  value = json_object_get_string_member_with_default (json_object, key, NULL);

  if (value == NULL)
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "the parsed manifest JSON \"%s\" doesn't have the expected \"%s\" key",
                 manifest, key);

  return g_strdup (value);
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
_au_get_default_variant (const gchar *manifest,
                         GError **error)
{
  return _au_get_string_from_manifest (manifest, "variant", error);
}

/*
 * _au_get_current_system_version:
 * @manifest: (not nullable): Path to the JSON manifest file
 * @error: Used to raise an error on failure
 *
 * Returns: (type filename) (transfer full): The system version buildid, taken
 *  from the manifest file.
 */
static gchar *
_au_get_current_system_version (const gchar *manifest,
                                GError **error)
{
  return _au_get_string_from_manifest (manifest, "buildid", error);
}

/*
 * @candidate_obj: (not nullable): A JSON object representing the image update
 * @id: (inout) (not nullable):
 * @variant: (inout) (not nullable):
 * @size: (inout) (not nullable):
 * @error: Used to raise an error on failure
 */
static gboolean
_au_parse_image (JsonObject *candidate_obj,
                 gchar **id,
                 gchar **variant,
                 gint64 *size,
                 GError **error)
{
  JsonObject *img_obj = NULL;  /* borrowed */
  JsonNode *img_node = NULL;  /* borrowed */
  const gchar *local_id = NULL;
  const gchar *local_variant = NULL;
  gint64 local_size;

  g_return_val_if_fail (candidate_obj != NULL, FALSE);
  g_return_val_if_fail (id != NULL, FALSE);
  g_return_val_if_fail (variant != NULL, FALSE);
  g_return_val_if_fail (size != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  img_node = json_object_get_member (candidate_obj, "image");
  img_obj = json_node_get_object (img_node);

  local_size = json_object_get_int_member_with_default (img_obj, "estimated_size", 0);
  local_id = json_object_get_string_member_with_default (img_obj, "buildid", NULL);
  local_variant = json_object_get_string_member_with_default (img_obj, "variant", NULL);
  if (local_id == NULL || local_variant == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "The \"image\" JSON object doesn't have the expected members");
      return FALSE;
    }

  *id = g_strdup (local_id);
  *variant = g_strdup (local_variant);
  *size = local_size;

  return TRUE;
}

/*
 * @json_object: (not nullable): A JSON object representing the available updates
 * @type:
 * @updated_version: Update buildid that has been already installed and is
 *  waiting a reboot, or %NULL if there isn't such update
 * @available_builder: (not nullable): The available updates will be added to this
 *  GVariant builder
 * @available_later_builder: (not nullable): The available updates, that require a
 *  newer system version, will be added to this GVariant builder
 * @error: Used to raise an error on failure
 *
 * Returns: %TRUE on success (even if nothing was appended to either of the
 *  GVariant builders)
 */
static gboolean
_au_get_json_array_candidates (JsonObject *json_object,
                               AuUpdateType type,
                               const gchar *updated_version,
                               GVariantBuilder *available_builder,
                               GVariantBuilder *available_later_builder,
                               GError **error)
{
  const gchar *type_string = NULL;
  g_autofree gchar *requires = NULL;
  JsonObject *sub_obj = NULL;  /* borrowed */
  JsonNode *sub_node = NULL;  /* borrowed */
  JsonArray *array = NULL;  /* borrowed */
  guint array_size;
  gsize i;

  g_return_val_if_fail (json_object != NULL, FALSE);
  g_return_val_if_fail (available_builder != NULL, FALSE);
  g_return_val_if_fail (available_later_builder != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  switch (type)
    {
      case AU_UPDATE_TYPE_MINOR:
        type_string = "minor";
        break;

      case AU_UPDATE_TYPE_MAJOR:
        type_string = "major";
        break;

      default:
        g_return_val_if_reached (FALSE);
    }

  if (!json_object_has_member (json_object, type_string))
    return TRUE;

  sub_node = json_object_get_member (json_object, type_string);
  sub_obj = json_node_get_object (sub_node);

  if (!json_object_has_member (sub_obj, "candidates"))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "The JSON doesn't have the expected \"candidates\" member");
      return FALSE;
    }

  /* Note that despite its name, the `candidates` member does not
   * actually list multiple possible updates that can be applied
   * immediately. Instead, it lists a single update that can be
   * applied immediately, followed by 0 or more updates that can
   * only be applied after passing through earlier checkpoints. */
  array = json_object_get_array_member (sub_obj, "candidates");

  array_size = json_array_get_length (array);

  for (i = 0; i < array_size; i++)
    {
      g_autofree gchar *id = NULL;
      g_autofree gchar *variant = NULL;
      gint64 size;
      GVariantBuilder builder;

      if (!_au_parse_image (json_array_get_object_element (array, i),
                            &id, &variant, &size, error))
        return FALSE;

      if (i == 0 && g_strcmp0 (id, updated_version) == 0)
        {
          /* If the first proposed update matches the version already
           * applied (and is pending a reboot), there's nothing left for
           * us to do. Otherwise, we would be applying the same update
           * twice - something which we want avoid. */
          g_debug ("The proposed update to version '%s' has already been applied. Reboot to start using it.",
                   id);
          return TRUE;
        }

      g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));

      g_variant_builder_add (&builder, "{sv}", "variant",
                             g_variant_new_string (variant));
      g_variant_builder_add (&builder, "{sv}", "estimated_size",
                             g_variant_new_uint64 (size));
      g_variant_builder_add (&builder, "{sv}", "update_type",
                             g_variant_new_uint32 (type));
      if (requires != NULL)
        g_variant_builder_add (&builder, "{sv}", "requires",
                               g_variant_new_string (requires));

      g_variant_builder_add (i == 0 ? available_builder : available_later_builder,
                             "{sa{sv}}",
                             id,
                             &builder);

      g_clear_pointer (&requires, g_free);
      requires = g_steal_pointer (&id);
    }

  return TRUE;
}

/*
 * @json_node: (not nullable): The JsonNode of the steamos-atomupd-client output
 * @updated_version: Update buildid that has been already installed and is
 *  waiting a reboot, or %NULL if there isn't such update
 * @available: (out) (not optional): Map of available updates that can be installed
 * @available_later: (out) (not optional): Map of available updates that require
 *  a newer system version
 * @error: Used to raise an error on failure
 */
static gboolean
_au_parse_candidates (JsonNode *json_node,
                      const gchar *updated_version,
                      GVariant **available,
                      GVariant **available_later,
                      GError **error)
{
  g_auto(GVariantBuilder) available_builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("a{sa{sv}}"));
  g_auto(GVariantBuilder) available_later_builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("a{sa{sv}}"));
  JsonObject *json_object = NULL;  /* borrowed */

  g_return_val_if_fail (json_node != NULL, FALSE);
  g_return_val_if_fail (available != NULL, FALSE);
  g_return_val_if_fail (*available == NULL, FALSE);
  g_return_val_if_fail (available_later != NULL, FALSE);
  g_return_val_if_fail (*available_later == NULL, FALSE);

  json_object = json_node_get_object (json_node);

  if (!_au_get_json_array_candidates (json_object, AU_UPDATE_TYPE_MINOR, updated_version,
                                      &available_builder, &available_later_builder, error))
    return FALSE;

  if (!_au_get_json_array_candidates (json_object, AU_UPDATE_TYPE_MAJOR, updated_version,
                                      &available_builder, &available_later_builder, error))
    return FALSE;

  *available = g_variant_ref_sink (g_variant_builder_end (&available_builder));
  *available_later = g_variant_ref_sink (g_variant_builder_end (&available_later_builder));

  return TRUE;
}

static void
on_query_completed (GPid pid,
                    gint wait_status,
                    gpointer user_data)
{
  g_autoptr(QueryData) data = user_data;
  g_autoptr(GIOChannel) stdout_channel = NULL;
  g_autoptr(GVariant) available = NULL;
  g_autoptr(GVariant) available_later = NULL;
  g_autoptr(JsonNode) json_node = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *output = NULL;
  const gchar *updated_version = NULL;
  gsize out_length;
  AuUpdateStatus current_status;
  AuAtomupd1Impl *self = AU_ATOMUPD1_IMPL (data->req->object);

  if (!g_spawn_check_wait_status (wait_status, &error))
    {
      g_dbus_method_invocation_return_error (g_steal_pointer (&data->req->invocation),
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "An error occurred calling the 'steamos-atomupd-client' helper: %s",
                                             error->message);
      return;
    }

  stdout_channel = g_io_channel_unix_new (data->standard_output);
  if (g_io_channel_read_to_end (stdout_channel, &output, &out_length, &error) != G_IO_STATUS_NORMAL)
    {
      g_dbus_method_invocation_return_error (g_steal_pointer (&data->req->invocation),
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "An error occurred reading the output of 'steamos-atomupd-client' helper: %s",
                                             error->message);
      return;
    }

  if (out_length == 0 || output[0] == '\0')
    {
      /* In theory when no updates are available we should receive an empty
       * JSON object (i.e. {}). Is it okay to assume no updates or should we
       * throw an error here? */
      available = g_variant_new ("a{sa{sv}}", NULL);
      available_later = g_variant_new ("a{sa{sv}}", NULL);
      goto success;
    }

  if (out_length != strlen (output))
    {
      /* This might happen if there is the terminating null byte '\0' followed
       * by some other data */
      g_dbus_method_invocation_return_error (g_steal_pointer (&data->req->invocation),
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Helper output is not valid JSON: contains \\0");
      return;
    }

  json_node = json_from_string (output, &error);
  if (json_node == NULL)
    {
      if (error == NULL)
        {
          /* The helper returned an empty JSON, there are no available updates */
          available = g_variant_new ("a{sa{sv}}", NULL);
          available_later = g_variant_new ("a{sa{sv}}", NULL);
          goto success;
        }
      else
        {
          g_dbus_method_invocation_return_error (g_steal_pointer (&data->req->invocation),
                                                 G_DBUS_ERROR,
                                                 G_DBUS_ERROR_FAILED,
                                                 "The helper output is not a valid JSON: %s",
                                                 error->message);
          return;
        }
    }

  current_status = au_atomupd1_get_update_status (data->req->object);
  if (current_status == AU_UPDATE_STATUS_SUCCESSFUL)
    updated_version = au_atomupd1_get_update_version (data->req->object);

  if (!_au_parse_candidates (json_node, updated_version, &available, &available_later, &error))
    {
      g_dbus_method_invocation_return_error (g_steal_pointer (&data->req->invocation),
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "An error occurred while parsing the helper output JSON: %s",
                                             error->message);
      return;
    }

  if (!g_file_replace_contents (self->updates_json_file, output, out_length, NULL, FALSE,
                                G_FILE_CREATE_NONE, NULL, NULL, &error))
    {
      g_dbus_method_invocation_return_error (g_steal_pointer (&data->req->invocation),
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "An error occurred while storing the helper output JSON: %s",
                                             error->message);
      return;
    }

success:
  au_atomupd1_set_versions_available (data->req->object, available);
  au_atomupd1_set_versions_available_later (data->req->object, available_later);
  au_atomupd1_complete_check_for_updates (data->req->object,
                                          g_steal_pointer (&data->req->invocation),
                                          available,
                                          available_later);
}

static gboolean
au_atomupd1_impl_handle_switch_to_variant (AuAtomupd1 *object,
                                           GDBusMethodInvocation *invocation,
                                           const gchar *arg_variant)
{
  g_autofree gchar *contracted_variant = NULL;
  g_autoptr(GError) error = NULL;

  contracted_variant = _au_get_legacy_contracted_variant (arg_variant);

  /* Store the legacy contracted version to ensure compatibility with steamos-update */
  if (!g_file_set_contents (_au_get_branch_file_path (), contracted_variant, -1, &error))
    {
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "An error occurred while storing the chosen variant: %s",
                                             error->message);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  _au_set_variant (object, arg_variant);

  au_atomupd1_complete_switch_to_variant (object, g_steal_pointer (&invocation));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
au_atomupd1_impl_handle_check_for_updates (AuAtomupd1 *object,
                                           GDBusMethodInvocation *invocation,
                                           GVariant *arg_options)
{
  const gchar *variant = NULL;
  const gchar *key = NULL;
  GVariant *value = NULL;
  GVariantIter iter;
  GPid child_pid;
  g_autoptr(QueryData) data = au_query_data_new ();
  g_autoptr(GPtrArray) argv = NULL;
  g_autoptr(GError) error = NULL;
  AuAtomupd1Impl *self = AU_ATOMUPD1_IMPL (object);

  g_return_val_if_fail (self->config_path != NULL, FALSE);
  g_return_val_if_fail (self->manifest_path != NULL, FALSE);

  g_variant_iter_init (&iter, arg_options);

  while (g_variant_iter_loop (&iter, "{sv}", &key, &value))
    {
      /* Reserved for future use */
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "The argument '%s' is not a valid option",
                                             key);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  variant = au_atomupd1_get_variant (object);

  argv = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (argv, g_strdup ("steamos-atomupd-client"));
  g_ptr_array_add (argv, g_strdup ("--config"));
  g_ptr_array_add (argv, g_strdup (self->config_path));
  g_ptr_array_add (argv, g_strdup ("--manifest-file"));
  g_ptr_array_add (argv, g_strdup (self->manifest_path));
  g_ptr_array_add (argv, g_strdup ("--variant"));
  g_ptr_array_add (argv, g_strdup (variant));
  g_ptr_array_add (argv, g_strdup ("--query-only"));
  g_ptr_array_add (argv, g_strdup ("--estimate-download-size"));
  g_ptr_array_add (argv, NULL);

  if (!g_spawn_async_with_pipes (NULL,    /* working directory */
                                 (gchar **) argv->pdata,
                                 NULL,    /* envp */
                                 G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                                 NULL,    /* child setup */
                                 NULL,    /* user data */
                                 &child_pid,
                                 NULL,    /* standard input */
                                 &data->standard_output,
                                 NULL,    /* standard error */
                                 &error))
    {
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "An error occurred calling the 'steamos-atomupd-client' helper: %s",
                                             error->message);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  data->req->invocation = g_steal_pointer (&invocation);
  data->req->object = g_object_ref (object);
  g_child_watch_add (child_pid, on_query_completed, g_steal_pointer (&data));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
_au_atomupd1_set_update_status_and_error (AuAtomupd1 *object,
                                          guint status,
                                          const gchar *error_code,
                                          const gchar *error_message)
{
  g_return_if_fail (object != NULL);

  au_atomupd1_set_update_status (object, status);
  au_atomupd1_set_failure_code (object, error_code);
  au_atomupd1_set_failure_message (object, error_message);
  g_dbus_interface_skeleton_flush (G_DBUS_INTERFACE_SKELETON (object));
}

/*
 * Returns: The RAUC service PID or -1 if an error occurred.
 */
static gint64
_au_get_rauc_service_pid (GError **error)
{
  g_autofree gchar *output = NULL;
  gchar *endptr = NULL;
  gint wait_status = 0;
  gint64 rauc_pid;

  const gchar *systemctl_argv[] = {
    "systemctl",
    "show",
    "--property",
    "MainPID",
    "rauc",
    NULL,
  };

  g_return_val_if_fail (error == NULL || *error == NULL, -1);

  if (!g_spawn_sync (NULL,  /* working directory */
                     (gchar **) systemctl_argv,
                     NULL,  /* envp */
                     G_SPAWN_SEARCH_PATH,
                     NULL,  /* child setup */
                     NULL,  /* user data */
                     &output,
                     NULL,  /* stderr */
                     &wait_status,
                     error))
    {
      return -1;
    }

  if (!g_spawn_check_wait_status (wait_status, error))
    return -1;

  if (!g_str_has_prefix (output, "MainPID="))
    {
      g_debug ("Systemctl output is '%s' instead of the expected 'MainPID=X'", output);
      g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                   "An error occurred while trying to gather the RAUC PID");
      return -1;
    }

  rauc_pid = g_ascii_strtoll (output + strlen("MainPID="), &endptr, 10);
  if (endptr == NULL || output + strlen("MainPID=") == (const char *) endptr)
    {
      g_debug ("Unable to parse Systemctl output: %s", output);
      g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                   "An error occurred while trying to gather the RAUC PID");
      return -1;
    }

  return rauc_pid;
}

/*
 * Returns: The @process PID or -1 if it's not available or an error occurred.
 */
static gint64
_au_get_process_pid (const gchar *process,
                     GError **error)
{
  g_autofree gchar *output = NULL;
  gchar *endptr = NULL;
  gint wait_status = 0;
  gint64 pid;

  g_return_val_if_fail (process != NULL, -1);
  g_return_val_if_fail (error == NULL || *error == NULL, -1);

  const gchar *argv[] = {
    "pidof",
    "--single-shot",
    "-x",
    process,
    NULL,
  };

  if (!g_spawn_sync (NULL,  /* working directory */
                     (gchar **) argv,
                     NULL,  /* envp */
                     G_SPAWN_SEARCH_PATH,
                     NULL,  /* child setup */
                     NULL,  /* user data */
                     &output,
                     NULL,  /* stderr */
                     &wait_status,
                     error))
    return -1;

  if (WIFEXITED (wait_status) && WEXITSTATUS (wait_status) == 1)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "There isn't a running process for %s", process);
      return -1;
    }

  if (!g_spawn_check_wait_status (wait_status, error))
    return -1;

  pid = g_ascii_strtoll (output, &endptr, 10);
  if (endptr == NULL || output == (const char *) endptr)
    {
      g_debug ("Unable to parse pidof output: %s", output);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "An error occurred while trying to gather the %s PID", process);
      return -1;
    }

  return pid;
}

static void
_au_ensure_pid_is_killed (GPid pid)
{
  gsize i;
  int status;
  int pgid = getpgid (pid);

  if (pid < 1)
    return;

  g_debug ("Sending SIGTERM to PID %i", pid);

  if (kill (pid, SIGTERM) == 0)
    {
      /* The PIDs we are trying to stop usually do it in less than a second.
       * We wait up to 2s and, if they are still running, we will send a
       * SIGKILL. */
      for (i = 0; i < 4; i++)
        {
          int saved_errno;

          if (waitpid (pid, &status, WNOHANG | WUNTRACED) > 0)
            {
              if (WIFEXITED (status))
                goto success;

              if (WIFSTOPPED (status))
                {
                  g_debug ("PID %i is currently paused, sending SIGCONT to the group %i",
                           pid, pgid);
                  killpg (pgid, SIGCONT);
                }
            }
          else
            {
              saved_errno = errno;

              if (saved_errno == ESRCH)
                goto success;

              if (saved_errno == ECHILD)
                {
                  /* The PID may not be our child, i.e. the rauc service.
                   * It is still safe to kill it, because it is the process
                   * responsible for applying an update and will gracefully
                   * handle the kill(). When we'll try to apply another update
                   * this service will be automatically executed again. */
                  if (kill (pid, 0) != 0)
                    goto success;

                  /* If this process is not our child, we can't use waitpid() and WIFSTOPPED().
                   * Instead we send a SIGCONT regardless of the status of the process. */
                  g_debug ("Sending SIGCONT to the group %i to ensure that the PIDs are not paused",
                           pgid);
                  killpg (pgid, SIGCONT);
                }
            }

          g_debug ("PID %i is still running", pid);
          g_usleep (0.5 * G_USEC_PER_SEC);
        }
    }

  g_debug ("Sending SIGKILL to PID %i", pid);
  kill (pid, SIGKILL);
  waitpid (pid, NULL, 0);

success:
  g_debug ("PID %i terminated successfully", pid);
}

static void
_au_cancel_async (GTask *task,
                  gpointer source_object,
			            gpointer data,
                  GCancellable *cancellable)
{
  gint64 rauc_pid;
  g_autoptr(GError) error = NULL;
  GPid pid = GPOINTER_TO_INT (data);

  /* The first thing to kill is the install helper. Otherwise, if we kill
   * RAUC while the helper is still running, the helper might execute RAUC
   * again before we are able to send the termination signal to the helper. */
  _au_ensure_pid_is_killed (pid);

  /* At the moment a RAUC operation can't be cancelled using its D-Bus API.
   * For this reason we get its PID number and send a SIGTERM/SIGKILL to it. */
  rauc_pid = _au_get_rauc_service_pid (&error);

  if (rauc_pid < 0)
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  _au_ensure_pid_is_killed (rauc_pid);

  g_task_return_boolean (task, TRUE);
}

static void
cancel_callback (GObject *source_object,
                 GAsyncResult *result,
                 gpointer user_data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(RequestData) data = user_data;

  if (g_task_propagate_boolean (G_TASK (result), &error))
    {
      au_atomupd1_set_update_status (data->object, AU_UPDATE_STATUS_CANCELLED);
      au_atomupd1_complete_cancel_update (data->object,
                                          g_steal_pointer (&data->invocation));
    }
  else
    {
      /* We failed to cancel a running update, probably the update is still
       * running, but we don't know for certain. */
      g_dbus_method_invocation_return_error (g_steal_pointer (&data->invocation),
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Failed to cancel an update: %s",
                                             error->message);
    }
}

static gboolean
au_atomupd1_impl_handle_cancel_update (AuAtomupd1 *object,
                                       GDBusMethodInvocation *invocation)
{
  g_autoptr(RequestData) data = g_slice_new0 (RequestData);
  g_autoptr(GTask) task = NULL;
  AuAtomupd1Impl *self = AU_ATOMUPD1_IMPL (object);

  if (au_atomupd1_get_update_status (object) != AU_UPDATE_STATUS_IN_PROGRESS
      && au_atomupd1_get_update_status (object) != AU_UPDATE_STATUS_PAUSED)
    {
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "There isn't an update in progress that can be cancelled");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  /* Remove the previous child watch callback */
  if (self->install_event_source != 0)
    g_source_remove (self->install_event_source);

  self->install_event_source = 0;

  data->invocation = g_steal_pointer (&invocation);
  data->object = g_object_ref (object);

  task = g_task_new (NULL, NULL, cancel_callback, g_steal_pointer (&data));

  g_task_set_task_data (task, GINT_TO_POINTER(self->install_pid), NULL);
  g_task_run_in_thread (task, _au_cancel_async);

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
_au_send_signal_to_install_procs (AuAtomupd1Impl *self,
                                  int sig,
                                  GError **error)
{
  gint64 rauc_pid;
  int rauc_pgid;
  int saved_errno;

  g_return_val_if_fail (self != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (self->install_pid == 0)
    {
      g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                   "Unexpectedly the PID of the install helper is not set");
      return FALSE;
    }

  g_debug ("Sending signal %i to the install helper with PID %i",
           sig, self->install_pid);

  if (kill (self->install_pid, sig) < 0)
    {
      saved_errno = errno;
      g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                   "Unable to send signal %i to the update helper: %s",
                   sig, g_strerror (saved_errno));
      return FALSE;
    }

  rauc_pid = _au_get_rauc_service_pid (error);
  if (rauc_pid < 0)
    return FALSE;

  if (rauc_pid > 0)
    {
      /* Send the signal to the entire PGID, to include the eventual Desync process */
      rauc_pgid = getpgid (rauc_pid);
      g_debug ("Sending signal %i to the RAUC service PGID %i", sig, rauc_pgid);

      if (killpg (rauc_pgid, sig) < 0)
        {
          saved_errno = errno;
          g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                       "Unable to send signal %i to the RAUC service: %s",
                       sig, g_strerror (saved_errno));
          return FALSE;
        }
    }
  return TRUE;
}

static gboolean
au_atomupd1_impl_handle_pause_update (AuAtomupd1 *object,
                                      GDBusMethodInvocation *invocation)
{
  g_autoptr(GError) error = NULL;
  AuAtomupd1Impl *self = AU_ATOMUPD1_IMPL (object);

  if (au_atomupd1_get_update_status (object) != AU_UPDATE_STATUS_IN_PROGRESS)
    {
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "There isn't an update in progress that can be paused");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!_au_send_signal_to_install_procs (self, SIGSTOP, &error))
    {
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             error->domain,
                                             error->code,
                                             "An error occurred while attempting to pause the installation process: %s",
                                             error->message);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  au_atomupd1_set_update_status (object, AU_UPDATE_STATUS_PAUSED);
  au_atomupd1_complete_pause_update (object, g_steal_pointer (&invocation));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
_au_client_stdout_update_cb (GObject *object_stream,
                             GAsyncResult *result,
                             gpointer user_data)
{
  GDataInputStream *stream = (GDataInputStream *)object_stream;
  g_autoptr(AuAtomupd1) object = user_data;
  AuAtomupd1Impl *self = AU_ATOMUPD1_IMPL (object);
  size_t len;
  const gchar *cursor = NULL;
  gchar *endptr = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *line = NULL;
  g_auto(GStrv) parts = NULL;
  g_autoptr(GDateTime) time_estimation = g_date_time_new_now_utc ();

  if (self->start_update_stdout_stream != stream)
    return;

  /* steamos-atomupd-client will periodically print updates regarding the
   * upgrade process. These updates are formatted as "XX.XX% DdHhMMmSSs".
   * The estimated remaining time may be missing if we are either using Casync
   * or if it is currently unknown.
   * Examples of valid values include: "15.85% 08m44s", "0.00%", "4.31% 00m56s",
   * "47.00% 1h12m05s" and "100%". */

  line = g_data_input_stream_read_line_finish (stream, result, NULL, &error);

  if (error != NULL)
    {
      g_debug ("Unable to read the update progress: %s", error->message);
      return;
    }

  /* If there is nothing more to read, just return */
  if (line == NULL)
    return;

  parts = g_strsplit (g_strstrip (line), " ", 2);

  len = strlen (parts[0]);
  if (len < 2 || parts[0][len - 1] != '%')
    {
      g_debug ("Unable to parse the completed percentage: %s", parts[0]);
      return;
    }

  /* Remove the percentage sign */
  parts[0][len - 1] = '\0';

  /* The percentage here is not locale dependent, we don't have to worry
   * about comma vs period for the decimals. */
  au_atomupd1_set_progress_percentage (object, g_ascii_strtod (parts[0], NULL));

  g_data_input_stream_read_line_async (stream, G_PRIORITY_DEFAULT, NULL,
                                       _au_client_stdout_update_cb,
                                       g_object_ref (object));

  if (parts[1] == NULL)
    {
      au_atomupd1_set_estimated_completion_time (object, 0);
      g_dbus_interface_skeleton_flush (G_DBUS_INTERFACE_SKELETON (object));
      return;
    }

  cursor = parts[1];
  while (*cursor != '\0')
    {
      g_autoptr(GDateTime) updated_estimation = NULL;
      guint64 value;

      endptr = NULL;
      value = g_ascii_strtoull (cursor, &endptr, 10);

      if (endptr == NULL || cursor == (const char *) endptr)
        {
          g_debug ("Unable to parse the expected remaining time: %s", parts[1]);
          au_atomupd1_set_estimated_completion_time (object, 0);
          g_dbus_interface_skeleton_flush (G_DBUS_INTERFACE_SKELETON (object));
          return;
        }

      cursor = endptr + 1;

      switch (endptr[0])
        {
          case 'd':
            updated_estimation = g_date_time_add_days (time_estimation, value);
            break;

          case 'h':
            updated_estimation = g_date_time_add_hours (time_estimation, value);
            break;

          case 'm':
            updated_estimation = g_date_time_add_minutes (time_estimation, value);
            break;

          case 's':
            updated_estimation = g_date_time_add_seconds (time_estimation, value);
            break;

          default:
            g_debug ("Unable to parse the expected remaining time: %s", parts[1]);
            au_atomupd1_set_estimated_completion_time (object, 0);
            g_dbus_interface_skeleton_flush (G_DBUS_INTERFACE_SKELETON (object));
            return;
        }

      g_date_time_unref (time_estimation);
      time_estimation = g_steal_pointer (&updated_estimation);
    }

  au_atomupd1_set_estimated_completion_time (object,
                                             g_date_time_to_unix (time_estimation));
  g_dbus_interface_skeleton_flush (G_DBUS_INTERFACE_SKELETON (object));
}

static void
au_start_update_clear (AuAtomupd1Impl *self)
{
  g_clear_object (&self->start_update_stdout_stream);
  self->install_event_source = 0;
  self->install_pid = 0;
}

static void
child_watch_cb (GPid pid,
                gint wait_status,
                gpointer user_data)
{
  g_autoptr(AuAtomupd1) object = user_data;
  g_autoptr(GError) error = NULL;

  if (g_spawn_check_wait_status (wait_status, &error))
    {
      g_debug ("The update has been successfully applied");
      _au_atomupd1_set_update_status_and_error (object, AU_UPDATE_STATUS_SUCCESSFUL, NULL, NULL);
    }
  else
    {
      g_debug ("'steamos-atomupd-client' helper returned an error: %s", error->message);
      _au_atomupd1_set_update_status_and_error (object, AU_UPDATE_STATUS_FAILED,
                                                "org.freedesktop.DBus.Error",
                                                error->message);
    }

  au_start_update_clear ((AuAtomupd1Impl *)object);
}

static gboolean
au_atomupd1_impl_handle_start_update (AuAtomupd1 *object,
                                      GDBusMethodInvocation *invocation,
                                      const gchar *arg_id)
{
  AuAtomupd1Impl *self = (AuAtomupd1Impl *)object;
  g_autoptr(GPtrArray) argv = NULL;
  g_autoptr(GFileIOStream) stream = NULL;
  g_autoptr(GInputStream) unix_stream = NULL;
  g_autoptr(GError) error = NULL;
  AuUpdateStatus current_status;
  gint client_stdout;

  current_status = au_atomupd1_get_update_status (object);
  if (current_status == AU_UPDATE_STATUS_IN_PROGRESS
      || current_status == AU_UPDATE_STATUS_PAUSED)
    {
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Failed to start a new update because one "
                                             "is already in progress");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!g_file_query_exists (self->updates_json_file, NULL))
    {
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "It is not possible to start an update "
                                             "before calling \"CheckForUpdates\"");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  au_atomupd1_set_update_version (object, arg_id);

  /* Create a copy of the json file because we will pass that to the
   * 'steamos-atomupd-client' helper and, if in the meantime we check again
   * for updates, we may replace that file with a newer version. */

  if (self->updates_json_copy != NULL)
    {
      g_file_delete (self->updates_json_copy, NULL, NULL);
      g_clear_object (&self->updates_json_copy);
    }

  self->updates_json_copy = g_file_new_tmp ("steamos-atomupd-XXXXXX.json", &stream, &error);
  if (self->updates_json_copy == NULL)
    {
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Failed to create a copy of the JSON update file %s",
                                             error->message);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }
  if (!g_file_copy (self->updates_json_file, self->updates_json_copy, G_FILE_COPY_OVERWRITE,
                    NULL, NULL, NULL, &error))
    {
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Failed to create a copy of the JSON update file %s",
                                             error->message);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  argv = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (argv, g_strdup ("steamos-atomupd-client"));
  g_ptr_array_add (argv, g_strdup ("--config"));
  g_ptr_array_add (argv, g_strdup (self->config_path));
  g_ptr_array_add (argv, g_strdup ("--update-file"));
  g_ptr_array_add (argv, g_file_get_path (self->updates_json_copy));
  g_ptr_array_add (argv, g_strdup ("--update-version"));
  g_ptr_array_add (argv, g_strdup (arg_id));
  g_ptr_array_add (argv, NULL);

  au_start_update_clear (self);
  if (!g_spawn_async_with_pipes (NULL,    /* working directory */
                                 (gchar **) argv->pdata,
                                 NULL,    /* envp */
                                 G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                                 NULL,    /* child setup */
                                 NULL,    /* user data */
                                 &self->install_pid,
                                 NULL,    /* standard input */
                                 &client_stdout,
                                 NULL,    /* standard error */
                                 &error))
    {
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Failed to launch the \"steamos-atomupd-client\" helper: %s",
                                             error->message);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  unix_stream = g_unix_input_stream_new (client_stdout, TRUE);
  self->start_update_stdout_stream = g_data_input_stream_new (unix_stream);

  g_data_input_stream_read_line_async (self->start_update_stdout_stream,
                                       G_PRIORITY_DEFAULT, NULL,
                                       _au_client_stdout_update_cb,
                                       g_object_ref (object));

  self->install_event_source = g_child_watch_add (self->install_pid,
                                                  (GChildWatchFunc) child_watch_cb,
                                                  g_object_ref (object));

  au_atomupd1_set_progress_percentage (object, 0);
  _au_atomupd1_set_update_status_and_error (object, AU_UPDATE_STATUS_IN_PROGRESS,
                                            NULL, NULL);

  au_atomupd1_complete_start_update (object, g_steal_pointer (&invocation));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
au_atomupd1_impl_handle_resume_update (AuAtomupd1 *object,
                                       GDBusMethodInvocation *invocation)
{
  g_autoptr(GError) error = NULL;
  AuAtomupd1Impl *self = AU_ATOMUPD1_IMPL (object);

  if (au_atomupd1_get_update_status (object) != AU_UPDATE_STATUS_PAUSED)
    {
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "There isn't a paused update that can be resumed");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!_au_send_signal_to_install_procs (self, SIGCONT, &error))
    {
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             error->domain,
                                             error->code,
                                             "An error occurred while attempting to resume the installation process: %s",
                                             error->message);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  au_atomupd1_set_update_status (object, AU_UPDATE_STATUS_IN_PROGRESS);
  au_atomupd1_complete_resume_update (object, g_steal_pointer (&invocation));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
init_atomupd1_iface (AuAtomupd1Iface *iface)
{
  iface->handle_cancel_update = au_atomupd1_impl_handle_cancel_update;
  iface->handle_check_for_updates = au_atomupd1_impl_handle_check_for_updates;
  iface->handle_pause_update = au_atomupd1_impl_handle_pause_update;
  iface->handle_resume_update = au_atomupd1_impl_handle_resume_update;
  iface->handle_start_update = au_atomupd1_impl_handle_start_update;
  iface->handle_switch_to_variant = au_atomupd1_impl_handle_switch_to_variant;
}

G_DEFINE_TYPE_WITH_CODE (AuAtomupd1Impl, au_atomupd1_impl, AU_TYPE_ATOMUPD1_SKELETON,
                         G_IMPLEMENT_INTERFACE (AU_TYPE_ATOMUPD1, init_atomupd1_iface))

static void
au_atomupd1_impl_finalize (GObject *object)
{
  AuAtomupd1Impl *self = (AuAtomupd1Impl *)object;

  g_free (self->config_path);
  g_free (self->manifest_path);

  // Keep the update file, to be able to reuse it later on
  g_clear_object (&self->updates_json_file);

  if (self->updates_json_copy != NULL)
    {
      g_file_delete (self->updates_json_copy, NULL, NULL);
      g_clear_object (&self->updates_json_copy);
    }

  if (self->changed_id != 0)
    {
      g_signal_handler_disconnect (self->variant_monitor, self->changed_id);
      self->changed_id = 0;
    }

  g_clear_object (&self->variant_monitor);

  au_start_update_clear (self);

  G_OBJECT_CLASS (au_atomupd1_impl_parent_class)->finalize (object);
}

static void
au_atomupd1_impl_class_init (AuAtomupd1ImplClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = au_atomupd1_impl_finalize;
}

static void
au_atomupd1_impl_init (AuAtomupd1Impl *self)
{
}

/**
 * au_atomupd1_impl_new:
 * @config_preference: (transfer none) (nullable): Path to the configuration
 *  file. If %NULL, the default configuration path will be used instead.
 * @manifest_preference: (transfer none) (nullable): Path to the JSON manifest
 *  file. If %NULL, the path will be either the one in the configuration file,
 *  if any, or the default manifest path.
 * @error: Used to raise an error on failure
 *
 * Returns: (transfer full): a new AuAtomupd1
 */
AuAtomupd1 *
au_atomupd1_impl_new (const gchar *config_preference,
                      const gchar *manifest_preference,
                      GError **error)
{
  g_autofree gchar *variant = NULL;
  g_autofree gchar *expanded_variant = NULL;
  g_autofree gchar *system_version = NULL;
  g_autofree gchar *manifest_from_config = NULL;
  g_autofree gchar *installed_version = NULL;
  g_auto(GStrv) known_variants = NULL;
  g_autoptr(GFile) updates_json_parent = NULL;
  g_autoptr(GFile) branch_file = NULL;
  const gchar *updates_json_path;
  const gchar *reboot_for_update;
  g_autoptr(GError) local_error = NULL;
  gint64 client_pid = -1;
  gint64 rauc_pid = -1;
  AuAtomupd1Impl *atomupd = g_object_new (AU_TYPE_ATOMUPD1_IMPL, NULL);
  g_autoptr(GKeyFile) client_config = g_key_file_new ();

  if (config_preference != NULL)
    atomupd->config_path = g_strdup (config_preference);
  else
    atomupd->config_path = g_strdup (AU_DEFAULT_CONFIG);

  if (!g_key_file_load_from_file (client_config, atomupd->config_path,
                                  G_KEY_FILE_NONE, &local_error))
    {
      /* The configuration file is not mandatory, continue anyway */
      g_debug ("Failed to load the config file %s: %s", atomupd->config_path,
               local_error->message);
      g_clear_error (&local_error);
    }

  if (manifest_preference != NULL)
    {
      atomupd->manifest_path = g_strdup (manifest_preference);
    }
  else
    {
      manifest_from_config = _au_get_manifest_path_from_config (client_config);
      if (manifest_from_config != NULL)
        atomupd->manifest_path = g_steal_pointer (&manifest_from_config);
      else
        atomupd->manifest_path = g_strdup (AU_DEFAULT_MANIFEST);
    }

  g_debug ("Getting the list of known variants");
  known_variants = _au_get_known_variants_from_config (client_config, &local_error);
  if (known_variants == NULL)
    {
      /* Log the error, leave "KnownVariants" as empty and try to continue */
      g_warning ("Failed to get the list of known variants from %s: %s",
                 atomupd->config_path, local_error->message);
      g_clear_error (&local_error);
    }
  else
    {
      au_atomupd1_set_known_variants ((AuAtomupd1 *)atomupd, (const gchar *const *)known_variants);
    }

  /* This environment variable is used for debugging and automated tests */
  updates_json_path = g_getenv ("AU_UPDATES_JSON_FILE");
  if (updates_json_path == NULL)
    updates_json_path = AU_DEFAULT_UPDATE_JSON;

  atomupd->updates_json_file = g_file_new_for_path (updates_json_path);

  updates_json_parent = g_file_get_parent (atomupd->updates_json_file);
  if (!g_file_make_directory_with_parents (updates_json_parent, NULL, &local_error))
    {
      if (local_error->code != G_IO_ERROR_EXISTS)
        {
          g_propagate_error (error, g_steal_pointer (&local_error));
          return NULL;
        }

      g_clear_error (&local_error);
    }

  /* Monitor the file for changes to avoid an out of sync situation, e.g. in
   * case the CLI steamos-select-branch is manually invoked. */
  branch_file = g_file_new_for_path (_au_get_branch_file_path ());
  atomupd->variant_monitor = g_file_monitor_file (branch_file, G_FILE_MONITOR_NONE,
                                                  NULL, error);
  if (atomupd->variant_monitor == NULL)
    return NULL;

  atomupd->changed_id = g_signal_connect (atomupd->variant_monitor, "changed",
                                          G_CALLBACK (_au_variant_changed_cb), atomupd);

  variant = _au_get_chosen_variant (atomupd->manifest_path, error);
  if (variant == NULL)
    return NULL;
  expanded_variant = _au_get_expanded_variant (variant);

  g_debug ("Tracking the variant %s", expanded_variant);
  au_atomupd1_set_variant ((AuAtomupd1 *)atomupd, expanded_variant);

  system_version = _au_get_current_system_version (atomupd->manifest_path, error);
  if (system_version == NULL)
    return NULL;

  au_atomupd1_set_current_version ((AuAtomupd1 *)atomupd, system_version);

  /* We currently only have the version 1 of this interface */
  au_atomupd1_set_version ((AuAtomupd1 *)atomupd, 1);

  client_pid = _au_get_process_pid ("steamos-atomupd-client", &local_error);
  if (client_pid > -1)
    {
      g_debug ("There is already a steamos-atomupd-client process running, stopping it...");
      _au_ensure_pid_is_killed (client_pid);
    }
  else
    {
      g_debug ("%s", local_error->message);
      g_clear_error (&local_error);
    }

  g_debug ("Stopping the RAUC service, if it's running...");
  rauc_pid = _au_get_rauc_service_pid (NULL);
  _au_ensure_pid_is_killed (rauc_pid);

  /* This environment variable is used for debugging and automated tests */
  reboot_for_update = g_getenv ("AU_REBOOT_FOR_UPDATE");
  if (reboot_for_update == NULL)
    reboot_for_update = AU_REBOOT_FOR_UPDATE;

  if (g_file_get_contents (reboot_for_update, &installed_version, NULL, NULL))
    {
      gssize i;

      g_debug ("An update has already been successfully installed, it will be applied at the next reboot");
      installed_version = g_strstrip (installed_version);
      for (i = strlen (installed_version) - 1; i > 0; i--)
        {
          if (installed_version[i] == '\n')
            installed_version[i] = '\0';
          else
            break;
        }

      au_atomupd1_set_update_version ((AuAtomupd1 *)atomupd, installed_version);
      au_atomupd1_set_update_status ((AuAtomupd1 *)atomupd, AU_UPDATE_STATUS_SUCCESSFUL);
    }

  return (AuAtomupd1 *)atomupd;
}
