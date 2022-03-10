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

  gchar *config_path;
  gchar *manifest_path;
  GFile *updates_json_file;
  GFile *updates_json_copy;
  GDataInputStream *start_update_stdout_stream;
};

typedef struct
{
  AuAtomupd1 *object;
  GDBusMethodInvocation *invocation;
  gint standard_output;
} QueryData;

static void
_query_data_free (QueryData *self)
{
  if (self->invocation != NULL)
      g_dbus_method_invocation_return_error (g_steal_pointer (&self->invocation),
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Request was freed without being handled");

  g_clear_object (&self->object);

  if (self->standard_output > -1)
    g_close (self->standard_output, NULL);

  g_slice_free (QueryData, self);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (QueryData, _query_data_free)

static QueryData *
au_query_data_new (void)
{
  QueryData *data = g_slice_new0 (QueryData);
  data->standard_output = -1;

  return data;
}

/*
 * _au_get_manifest_path_from_config:
 * @config: (not nullable): Path to the configuration file
 *
 * Returns: (type filename) (transfer full): The configured path to the manifest,
 *  or %NULL if the configuration doesn't have the "Manifest" field.
 */
static gchar *
_au_get_manifest_path_from_config (const gchar *config)
{
  g_autoptr(GKeyFile) client_config = g_key_file_new ();

  g_return_val_if_fail (config != NULL, NULL);

  if (!g_key_file_load_from_file (client_config, config, G_KEY_FILE_NONE, NULL))
    return NULL;

  return g_key_file_get_string (client_config, "Host", "Manifest", NULL);
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
 *  manifest file.
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
 * @images: (not nullable): The parsed image details will be added in this
 *  GVariant builder
 * @json_object: (not nullable): A JSON object representing a single image update
 * @type:
 * @requires: (nullable): Indicates the required build id in order to apply this
 *  image update. If set to %NULL, it is assumed that this image could be applied
 *  without any restrictions
 * @error: Used to raise an error on failure
 *
 * Returns the parsed image build id or %NULL, if an error occurred.
 */
static const gchar *
_au_parse_image (GVariantBuilder *images,
                 JsonObject *candidate_obj,
                 AuUpdateType type,
                 const gchar *requires,
                 GError **error)
{
  JsonObject *img_obj = NULL;  /* borrowed */
  JsonNode *img_node = NULL;  /* borrowed */
  const gchar *id = NULL;
  const gchar *variant = NULL;
  gint64 size;
  GVariantBuilder builder;

  g_return_val_if_fail (images != NULL, NULL);
  g_return_val_if_fail (candidate_obj != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));

  img_node = json_object_get_member (candidate_obj, "image");
  img_obj = json_node_get_object (img_node);

  size = json_object_get_int_member_with_default (img_obj, "estimated_size", 0);
  id = json_object_get_string_member_with_default (img_obj, "buildid", NULL);
  variant = json_object_get_string_member_with_default (img_obj, "variant", NULL);
  if (id == NULL || variant == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "The \"image\" JSON object doesn't have the expected members");
      return NULL;
    }

  g_variant_builder_add (&builder, "{sv}", "variant",
                          g_variant_new_string (variant));
  g_variant_builder_add (&builder, "{sv}", "estimated_size",
                          g_variant_new_uint64 (size));
  g_variant_builder_add (&builder, "{sv}", "update_type",
                          g_variant_new_uint32 (type));
  if (requires != NULL)
    g_variant_builder_add (&builder, "{sv}", "requires",
                           g_variant_new_string (requires));

  g_variant_builder_add (images,
                         "{sa{sv}}",
                         id,
                         &builder);
  return id;
}

/*
 * @json_object: (not nullable): A JSON object representing the available updates
 * @type:
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
                               GVariantBuilder *available_builder,
                               GVariantBuilder *available_later_builder,
                               GError **error)
{
  const gchar *type_string = NULL;
  const gchar *requires = NULL;
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
      requires = _au_parse_image (i == 0 ? available_builder : available_later_builder,
                                  json_array_get_object_element (array, i),
                                  type,
                                  requires,
                                  error);
      if (requires == NULL)
        return FALSE;
    }

  return TRUE;
}

/*
 * @json_node: (not nullable): The JsonNode of the steamos-atomupd-client output
 * @available: (out) (not optional): Map of available updates that can be installed
 * @available_later: (out) (not optional): Map of available updates that require
 *  a newer system version
 * @error: Used to raise an error on failure
 */
static gboolean
_au_parse_candidates (JsonNode *json_node,
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

  if (!_au_get_json_array_candidates (json_object, AU_UPDATE_TYPE_MINOR, &available_builder,
                                      &available_later_builder, error))
    return FALSE;

  if (!_au_get_json_array_candidates (json_object, AU_UPDATE_TYPE_MAJOR, &available_builder,
                                      &available_later_builder, error))
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
  QueryData *data = user_data;
  g_autoptr(GIOChannel) stdout_channel = NULL;
  g_autoptr(GVariant) available = NULL;
  g_autoptr(GVariant) available_later = NULL;
  g_autoptr(GFileIOStream) stream = NULL;
  g_autoptr(JsonNode) json_node = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *output = NULL;
  gsize out_length;
  AuAtomupd1Impl *self = (AuAtomupd1Impl *)data->object;

  if (!g_spawn_check_wait_status (wait_status, &error))
    {
      g_dbus_method_invocation_return_error (g_steal_pointer (&data->invocation),
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "An error occurred calling the 'steamos-atomupd-client' helper: %s",
                                             error->message);
      return;
    }

  stdout_channel = g_io_channel_unix_new (data->standard_output);
  if (g_io_channel_read_to_end (stdout_channel, &output, &out_length, &error) != G_IO_STATUS_NORMAL)
    {
      g_dbus_method_invocation_return_error (g_steal_pointer (&data->invocation),
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
      g_dbus_method_invocation_return_error (g_steal_pointer (&data->invocation),
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
          g_dbus_method_invocation_return_error (g_steal_pointer (&data->invocation),
                                                 G_DBUS_ERROR,
                                                 G_DBUS_ERROR_FAILED,
                                                 "The helper output is not a valid JSON: %s",
                                                 error->message);
          return;
        }
    }

  if (!_au_parse_candidates (json_node, &available, &available_later, &error))
    {
      g_dbus_method_invocation_return_error (g_steal_pointer (&data->invocation),
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "An error occurred while parsing the helper output JSON: %s",
                                             error->message);
      return;
    }

  /* Store the update info in a file */
  if (self->updates_json_file != NULL)
    {
      g_file_delete (self->updates_json_file, NULL, NULL);
      g_clear_object (&self->updates_json_file);
    }

  self->updates_json_file = g_file_new_tmp ("atomupd-updates-XXXXXX.json", &stream, &error);

  if (self->updates_json_file == NULL)
    {
      g_dbus_method_invocation_return_error (g_steal_pointer (&data->invocation),
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "An error occurred while storing the helper output JSON: %s",
                                             error->message);
      return;
    }
  if (!g_file_replace_contents (self->updates_json_file, output, out_length, NULL, FALSE,
                                G_FILE_CREATE_NONE, NULL, NULL, &error))
    {
      g_dbus_method_invocation_return_error (g_steal_pointer (&data->invocation),
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "An error occurred while storing the helper output JSON: %s",
                                             error->message);
      return;
    }

success:
  au_atomupd1_set_versions_available (data->object, available);
  au_atomupd1_set_versions_available_later (data->object, available_later);
  au_atomupd1_complete_check_for_updates (data->object,
                                          g_steal_pointer (&data->invocation),
                                          available,
                                          available_later);
}

static gboolean
au_atomupd1_impl_handle_check_for_updates (AuAtomupd1 *object,
                                           GDBusMethodInvocation *invocation,
                                           GVariant *arg_options)
{
  g_autofree gchar *variant = NULL;
  const gchar *key = NULL;
  GVariant *value = NULL;
  GVariantIter iter;
  GPid child_pid;
  g_autoptr(QueryData) data = au_query_data_new ();
  g_autoptr(GPtrArray) argv = NULL;
  g_autoptr(GError) error = NULL;
  AuAtomupd1Impl *self = (AuAtomupd1Impl *)object;

  g_return_val_if_fail (self->config_path != NULL, FALSE);
  g_return_val_if_fail (self->manifest_path != NULL, FALSE);

  g_variant_iter_init (&iter, arg_options);

  while (g_variant_iter_loop (&iter, "{sv}", &key, &value))
    {
      if (g_strcmp0 (key, "variant") == 0)
        {
          if (g_variant_is_of_type (value, G_VARIANT_TYPE_STRING))
            {
              variant = g_strdup (g_variant_get_string (value, NULL));
              continue;
            }
        }
      else
        {
          g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                                 G_DBUS_ERROR,
                                                 G_DBUS_ERROR_FAILED,
                                                 "The argument '%s' is not a valid option",
                                                 key);
          return G_DBUS_METHOD_INVOCATION_HANDLED;
        }

      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "The option argument '%s' has an unexpected value type",
                                             key);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (variant == NULL)
    {
      variant = _au_get_default_variant (self->manifest_path, &error);
      if (variant == NULL)
        {
          g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                                 G_DBUS_ERROR,
                                                 G_DBUS_ERROR_FAILED,
                                                 "An error occurred while parsing the manifest: %s",
                                                 error->message);
          return G_DBUS_METHOD_INVOCATION_HANDLED;
        }
    }

  au_atomupd1_set_variant (object, variant);

  argv = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (argv, g_strdup ("steamos-atomupd-client"));
  g_ptr_array_add (argv, g_strdup ("--config"));
  g_ptr_array_add (argv, g_strdup (self->config_path));
  g_ptr_array_add (argv, g_strdup ("--manifest-file"));
  g_ptr_array_add (argv, g_strdup (self->manifest_path));
  g_ptr_array_add (argv, g_strdup ("--variant"));
  g_ptr_array_add (argv, g_steal_pointer (&variant));
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

  data->invocation = g_steal_pointer (&invocation);
  data->object = g_object_ref (object);
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

static gboolean
au_atomupd1_impl_handle_cancel_update (AuAtomupd1 *object,
                                       GDBusMethodInvocation *invocation)
{
  g_warning ("TODO: cancel update is just a stub!");

  if (au_atomupd1_get_update_status (object) != AU_UPDATE_STATUS_IN_PROGRESS)
    {
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "There isn't an update in progress that can be cancelled");
      return TRUE;
    }

  // au_atomupd1_set_update_status (object, AU_UPDATE_STATUS_CANCELLED);

  au_atomupd1_complete_cancel_update (object, g_steal_pointer (&invocation));

  return TRUE;
}

static gboolean
au_atomupd1_impl_handle_pause_update (AuAtomupd1 *object,
                                      GDBusMethodInvocation *invocation)
{
  g_warning ("TODO: pause update is just a stub!");

  if (au_atomupd1_get_update_status (object) != AU_UPDATE_STATUS_IN_PROGRESS)
    {
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "There isn't an update in progress that can be paused");
      return TRUE;
    }

  // au_atomupd1_set_update_status (object, AU_UPDATE_STATUS_PAUSED);

  au_atomupd1_complete_pause_update (object, g_steal_pointer (&invocation));

  return TRUE;
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
            time_estimation = g_date_time_add_days (time_estimation, value);
            break;

          case 'h':
            time_estimation = g_date_time_add_hours (time_estimation, value);
            break;

          case 'm':
            time_estimation = g_date_time_add_minutes (time_estimation, value);
            break;

          case 's':
            time_estimation = g_date_time_add_seconds (time_estimation, value);
            break;

          default:
            g_debug ("Unable to parse the expected remaining time: %s", parts[1]);
            au_atomupd1_set_estimated_completion_time (object, 0);
            g_dbus_interface_skeleton_flush (G_DBUS_INTERFACE_SKELETON (object));
            return;
        }
    }

  au_atomupd1_set_estimated_completion_time (object,
                                             g_date_time_to_unix (time_estimation));
  g_dbus_interface_skeleton_flush (G_DBUS_INTERFACE_SKELETON (object));
}

static void
au_start_update_clear (AuAtomupd1Impl *self)
{
  g_clear_object (&self->start_update_stdout_stream);
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
  GPid client_pid;
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

  if (self->updates_json_file == NULL)
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

  if (!g_spawn_async_with_pipes (NULL,    /* working directory */
                                 (gchar **) argv->pdata,
                                 NULL,    /* envp */
                                 G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                                 NULL,    /* child setup */
                                 NULL,    /* user data */
                                 &client_pid,
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

  au_start_update_clear (self);
  unix_stream = g_unix_input_stream_new (client_stdout, TRUE);
  self->start_update_stdout_stream = g_data_input_stream_new (unix_stream);

  g_data_input_stream_read_line_async (self->start_update_stdout_stream,
                                       G_PRIORITY_DEFAULT, NULL,
                                       _au_client_stdout_update_cb,
                                       g_object_ref (object));

  g_child_watch_add (client_pid, (GChildWatchFunc) child_watch_cb,
                     g_object_ref (object));

  _au_atomupd1_set_update_status_and_error (object, AU_UPDATE_STATUS_IN_PROGRESS,
                                            NULL, NULL);

  au_atomupd1_complete_start_update (object, g_steal_pointer (&invocation));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
au_atomupd1_impl_handle_resume_update (AuAtomupd1 *object,
                                       GDBusMethodInvocation *invocation)
{
  g_warning ("TODO: resume update is just a stub!");

  if (au_atomupd1_get_update_status (object) != AU_UPDATE_STATUS_PAUSED)
    {
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "There isn't a paused update that can be resumed");
      return TRUE;
    }

  // au_atomupd1_set_update_status (object, AU_UPDATE_STATUS_IN_PROGRESS);

  au_atomupd1_complete_resume_update (object, g_steal_pointer (&invocation));

  return TRUE;
}

static void
init_atomupd1_iface (AuAtomupd1Iface *iface)
{
  iface->handle_cancel_update = au_atomupd1_impl_handle_cancel_update;
  iface->handle_check_for_updates = au_atomupd1_impl_handle_check_for_updates;
  iface->handle_pause_update = au_atomupd1_impl_handle_pause_update;
  iface->handle_resume_update = au_atomupd1_impl_handle_resume_update;
  iface->handle_start_update = au_atomupd1_impl_handle_start_update;
}

G_DEFINE_TYPE_WITH_CODE (AuAtomupd1Impl, au_atomupd1_impl, AU_TYPE_ATOMUPD1_SKELETON,
                         G_IMPLEMENT_INTERFACE (AU_TYPE_ATOMUPD1, init_atomupd1_iface))

static void
au_atomupd1_impl_finalize (GObject *object)
{
  AuAtomupd1Impl *self = (AuAtomupd1Impl *)object;

  g_free (self->config_path);
  g_free (self->manifest_path);

  if (self->updates_json_file != NULL)
    {
      g_file_delete (self->updates_json_file, NULL, NULL);
      g_clear_object (&self->updates_json_file);
    }

  if (self->updates_json_copy != NULL)
    {
      g_file_delete (self->updates_json_copy, NULL, NULL);
      g_clear_object (&self->updates_json_copy);
    }

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
  g_autofree gchar *system_version = NULL;
  g_autofree gchar *manifest_from_config = NULL;
  AuAtomupd1Impl *atomupd = g_object_new (AU_TYPE_ATOMUPD1_IMPL, NULL);

  if (config_preference != NULL)
    atomupd->config_path = g_strdup (config_preference);
  else
    atomupd->config_path = g_strdup (AU_DEFAULT_CONFIG);

  if (manifest_preference != NULL)
    {
      atomupd->manifest_path = g_strdup (manifest_preference);
    }
  else
    {
      manifest_from_config = _au_get_manifest_path_from_config (atomupd->config_path);
      if (manifest_from_config != NULL)
        atomupd->manifest_path = g_steal_pointer (&manifest_from_config);
      else
        atomupd->manifest_path = g_strdup (AU_DEFAULT_MANIFEST);
    }

  variant = _au_get_default_variant (atomupd->manifest_path, error);
  if (variant == NULL)
    return NULL;

  au_atomupd1_set_variant ((AuAtomupd1 *)atomupd, variant);

  system_version = _au_get_current_system_version (atomupd->manifest_path, error);
  if (system_version == NULL)
    return NULL;

  au_atomupd1_set_current_version ((AuAtomupd1 *)atomupd, system_version);

  /* We currently only have the version 1 of this interface */
  au_atomupd1_set_version ((AuAtomupd1 *)atomupd, 1);

  return (AuAtomupd1 *)atomupd;
}
