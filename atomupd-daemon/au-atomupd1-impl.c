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

#include "utils.h"
#include "au-atomupd1-impl.h"

#include <json-glib/json-glib.h>

struct _AuAtomupd1Impl
{
  AuAtomupd1Skeleton parent_instance;

  gchar *config_path;
  gchar *manifest_path;
};

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
  const gchar *variant = NULL;
  JsonNode *json_node = NULL;  /* borrowed */
  JsonObject *json_object = NULL;  /* borrowed */
  g_autoptr(JsonParser) parser = NULL;

  g_return_val_if_fail (manifest != NULL, NULL);

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
  variant = json_object_get_string_member_with_default (json_object, "variant", NULL);

  if (variant == NULL)
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "the parsed manifest JSON \"%s\" doesn't have the expected \"variant\" key",
                 manifest);

  return g_strdup (variant);
}

static gboolean
au_atomupd1_impl_handle_check_for_updates (AuAtomupd1 *object,
                                           GDBusMethodInvocation *invocation,
                                           GVariant *arg_options)
{
  g_warning ("TODO: check for update is just a stub!");

  au_atomupd1_complete_check_for_updates (object,
                                          g_steal_pointer (&invocation),
                                          g_variant_new ("a{sa{sv}}", NULL),
                                          g_variant_new ("a{sa{sv}}", NULL));

  return TRUE;
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

static gboolean
au_atomupd1_impl_handle_start_update (AuAtomupd1 *object,
                                      GDBusMethodInvocation *invocation,
                                      const gchar *arg_id)
{
  g_warning ("TODO: start update is just a stub!");

  // au_atomupd1_set_update_status (object, AU_UPDATE_STATUS_IN_PROGRESS);

  au_atomupd1_complete_start_update (object, g_steal_pointer (&invocation));

  return TRUE;
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

  /* We currently only have the version 1 of this interface */
  au_atomupd1_set_version ((AuAtomupd1 *)atomupd, 1);

  return (AuAtomupd1 *)atomupd;
}
