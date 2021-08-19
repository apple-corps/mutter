/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/*
 * Copyright (C) 2013 Red Hat Inc.
 * Copyright (C) 2018 DisplayLink (UK) Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * Author: Giovanni Campagna <gcampagn@redhat.com>
 */

/**
 * SECTION:meta-monitor-manager-native
 * @title: MetaMonitorManagerNative
 * @short_description: A subclass of #MetaMonitorManager using Linux DRM
 *
 * #MetaMonitorManagerNative is a subclass of #MetaMonitorManager which
 * implements its functionality "natively": it uses the appropriate
 * functions of the Linux DRM kernel module and using a udev client.
 *
 * See also #MetaMonitorManagerXrandr for an implementation using XRandR.
 */

#include "config.h"

#include "backends/native/meta-monitor-manager-native.h"

#include <drm.h>
#include <errno.h>
#include <gudev/gudev.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "backends/meta-backend-private.h"
#include "backends/meta-crtc.h"
#include "backends/meta-monitor-config-manager.h"
#include "backends/meta-output.h"
#include "backends/native/meta-backend-native.h"
#include "backends/native/meta-crtc-kms.h"
#include "backends/native/meta-gpu-kms.h"
#include "backends/native/meta-kms-update.h"
#include "backends/native/meta-kms.h"
#include "backends/native/meta-launcher.h"
#include "backends/native/meta-output-kms.h"
#include "backends/native/meta-renderer-native.h"
#include "backends/native/meta-virtual-monitor-native.h"
#include "clutter/clutter.h"
#include "meta/main.h"
#include "meta/meta-x11-errors.h"

enum
{
  PROP_0,

  PROP_NEED_OUTPUTS,

  N_PROPS
};

static GParamSpec *obj_props[N_PROPS];

struct _MetaMonitorManagerNative
{
  MetaMonitorManager parent_instance;

  gulong kms_resources_changed_handler_id;

  GHashTable *crtc_gamma_cache;

  gboolean needs_outputs;
};

struct _MetaMonitorManagerNativeClass
{
  MetaMonitorManagerClass parent_class;
};

#define VIRTUAL_OUTPUT_ID_BIT (((uint64_t) 1) << 63)

static void
initable_iface_init (GInitableIface *initable_iface);

G_DEFINE_TYPE_WITH_CODE (MetaMonitorManagerNative, meta_monitor_manager_native,
                         META_TYPE_MONITOR_MANAGER,
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE,
                                                initable_iface_init))

static GBytes *
meta_monitor_manager_native_read_edid (MetaMonitorManager *manager,
                                       MetaOutput         *output)
{
  return meta_output_native_read_edid (META_OUTPUT_NATIVE (output));
}

static void
meta_monitor_manager_native_read_current_state (MetaMonitorManager *manager)
{
  MetaMonitorManagerClass *parent_class =
    META_MONITOR_MANAGER_CLASS (meta_monitor_manager_native_parent_class);
  MetaPowerSave power_save_mode;

  power_save_mode = meta_monitor_manager_get_power_save_mode (manager);
  if (power_save_mode != META_POWER_SAVE_ON)
    meta_monitor_manager_power_save_mode_changed (manager,
                                                  META_POWER_SAVE_ON);

  parent_class->read_current_state (manager);
}

uint64_t
meta_power_save_to_dpms_state (MetaPowerSave power_save)
{
  switch (power_save)
    {
    case META_POWER_SAVE_ON:
      return DRM_MODE_DPMS_ON;
    case META_POWER_SAVE_STANDBY:
      return DRM_MODE_DPMS_STANDBY;
    case META_POWER_SAVE_SUSPEND:
      return DRM_MODE_DPMS_SUSPEND;
    case META_POWER_SAVE_OFF:
      return DRM_MODE_DPMS_OFF;
    case META_POWER_SAVE_UNSUPPORTED:
      return DRM_MODE_DPMS_ON;
    }

  g_warn_if_reached ();
  return DRM_MODE_DPMS_ON;
}

static void
meta_monitor_manager_native_set_power_save_mode (MetaMonitorManager *manager,
                                                 MetaPowerSave       mode)
{
  MetaBackend *backend = meta_monitor_manager_get_backend (manager);
  MetaBackendNative *backend_native = META_BACKEND_NATIVE (backend);
  MetaKms *kms = meta_backend_native_get_kms (backend_native);
  GList *l;

  for (l = meta_backend_get_gpus (backend); l; l = l->next)
    {
      MetaGpuKms *gpu_kms = l->data;

      switch (mode)
        {
        case META_POWER_SAVE_ON:
        case META_POWER_SAVE_UNSUPPORTED:
          {
            g_list_foreach (meta_gpu_get_crtcs (META_GPU (gpu_kms)),
                            (GFunc) meta_crtc_kms_invalidate_gamma,
                            NULL);
            break;
          }
        case META_POWER_SAVE_STANDBY:
        case META_POWER_SAVE_SUSPEND:
        case META_POWER_SAVE_OFF:
          {
            MetaKmsDevice *kms_device = meta_gpu_kms_get_kms_device (gpu_kms);
            MetaKmsUpdate *kms_update;
            MetaKmsUpdateFlag flags;
            g_autoptr (MetaKmsFeedback) kms_feedback = NULL;

            kms_update = meta_kms_ensure_pending_update (kms, kms_device);
            meta_kms_update_set_power_save (kms_update);

            flags = META_KMS_UPDATE_FLAG_NONE;
            kms_feedback = meta_kms_post_pending_update_sync (kms,
                                                              kms_device,
                                                              flags);
            if (meta_kms_feedback_get_result (kms_feedback) !=
                META_KMS_FEEDBACK_PASSED)
              {
                g_warning ("Failed to enter power saving mode: %s",
                           meta_kms_feedback_get_error (kms_feedback)->message);
              }
            break;
          }
        }
    }
}

static void
meta_monitor_manager_native_ensure_initial_config (MetaMonitorManager *manager)
{
  MetaMonitorsConfig *config;

  config = meta_monitor_manager_ensure_configured (manager);

  meta_monitor_manager_update_logical_state (manager, config);
}

static void
apply_crtc_assignments (MetaMonitorManager    *manager,
                        MetaCrtcAssignment   **crtcs,
                        unsigned int           n_crtcs,
                        MetaOutputAssignment **outputs,
                        unsigned int           n_outputs)
{
  MetaBackend *backend = meta_monitor_manager_get_backend (manager);
  g_autoptr (GList) to_configure_outputs = NULL;
  g_autoptr (GList) to_configure_crtcs = NULL;
  unsigned i;
  GList *gpus;
  GList *l;

  gpus = meta_backend_get_gpus (backend);
  for (l = gpus; l; l = l->next)
    {
      MetaGpu *gpu = l->data;
      GList *crtcs;
      GList *outputs;

      outputs = g_list_copy (meta_gpu_get_outputs (gpu));
      to_configure_outputs = g_list_concat (to_configure_outputs, outputs);

      crtcs = g_list_copy (meta_gpu_get_crtcs (gpu));
      to_configure_crtcs = g_list_concat (to_configure_crtcs, crtcs);
    }

  for (l = meta_monitor_manager_get_virtual_monitors (manager); l; l = l->next)
    {
      MetaVirtualMonitor *virtual_monitor = l->data;
      MetaOutput *output = meta_virtual_monitor_get_output (virtual_monitor);
      MetaCrtc *crtc = meta_virtual_monitor_get_crtc (virtual_monitor);

      to_configure_outputs = g_list_append (to_configure_outputs, output);
      to_configure_crtcs = g_list_append (to_configure_crtcs, crtc);
    }

  for (i = 0; i < n_crtcs; i++)
    {
      MetaCrtcAssignment *crtc_assignment = crtcs[i];
      MetaCrtc *crtc = crtc_assignment->crtc;

      to_configure_crtcs = g_list_remove (to_configure_crtcs, crtc);

      if (crtc_assignment->mode == NULL)
        {
          meta_crtc_unset_config (crtc);
        }
      else
        {
          unsigned int j;

          meta_crtc_set_config (crtc,
                                &crtc_assignment->layout,
                                crtc_assignment->mode,
                                crtc_assignment->transform);

          for (j = 0; j < crtc_assignment->outputs->len; j++)
            {
              MetaOutput *output = g_ptr_array_index (crtc_assignment->outputs,
                                                      j);
              MetaOutputAssignment *output_assignment;

              to_configure_outputs = g_list_remove (to_configure_outputs,
                                                    output);

              output_assignment = meta_find_output_assignment (outputs,
                                                               n_outputs,
                                                               output);
              meta_output_assign_crtc (output, crtc, output_assignment);
            }
        }
    }

  g_list_foreach (to_configure_crtcs,
                  (GFunc) meta_crtc_unset_config,
                  NULL);
  g_list_foreach (to_configure_outputs,
                  (GFunc) meta_output_unassign_crtc,
                  NULL);
}

static void
update_screen_size (MetaMonitorManager *manager,
                    MetaMonitorsConfig *config)
{
  GList *l;
  int screen_width = 0;
  int screen_height = 0;

  for (l = config->logical_monitor_configs; l; l = l->next)
    {
      MetaLogicalMonitorConfig *logical_monitor_config = l->data;
      int right_edge;
      int bottom_edge;

      right_edge = (logical_monitor_config->layout.width +
                    logical_monitor_config->layout.x);
      if (right_edge > screen_width)
        screen_width = right_edge;

      bottom_edge = (logical_monitor_config->layout.height +
                     logical_monitor_config->layout.y);
      if (bottom_edge > screen_height)
        screen_height = bottom_edge;
    }

  manager->screen_width = screen_width;
  manager->screen_height = screen_height;
}

static gboolean
meta_monitor_manager_native_apply_monitors_config (MetaMonitorManager        *manager,
                                                   MetaMonitorsConfig        *config,
                                                   MetaMonitorsConfigMethod   method,
                                                   GError                   **error)
{
  GPtrArray *crtc_assignments;
  GPtrArray *output_assignments;

  if (!config)
    {
      if (!manager->in_init)
        {
          MetaBackend *backend = meta_get_backend ();
          MetaRenderer *renderer = meta_backend_get_renderer (backend);

          meta_renderer_native_reset_modes (META_RENDERER_NATIVE (renderer));
        }

      manager->screen_width = META_MONITOR_MANAGER_MIN_SCREEN_WIDTH;
      manager->screen_height = META_MONITOR_MANAGER_MIN_SCREEN_HEIGHT;
      meta_monitor_manager_rebuild (manager, NULL);
      return TRUE;
    }

  if (!meta_monitor_config_manager_assign (manager, config,
                                           &crtc_assignments,
                                           &output_assignments,
                                           error))
    return FALSE;

  if (method == META_MONITORS_CONFIG_METHOD_VERIFY)
    {
      g_ptr_array_free (crtc_assignments, TRUE);
      g_ptr_array_free (output_assignments, TRUE);
      return TRUE;
    }

  apply_crtc_assignments (manager,
                          (MetaCrtcAssignment **) crtc_assignments->pdata,
                          crtc_assignments->len,
                          (MetaOutputAssignment **) output_assignments->pdata,
                          output_assignments->len);

  g_ptr_array_free (crtc_assignments, TRUE);
  g_ptr_array_free (output_assignments, TRUE);

  update_screen_size (manager, config);
  meta_monitor_manager_rebuild (manager, config);

  return TRUE;
}

static void
meta_monitor_manager_native_get_crtc_gamma (MetaMonitorManager  *manager,
                                            MetaCrtc            *crtc,
                                            gsize               *size,
                                            unsigned short     **red,
                                            unsigned short     **green,
                                            unsigned short     **blue)
{
  MetaKmsCrtc *kms_crtc;
  const MetaKmsCrtcState *crtc_state;

  g_return_if_fail (META_IS_CRTC_KMS (crtc));

  kms_crtc = meta_crtc_kms_get_kms_crtc (META_CRTC_KMS (crtc));
  crtc_state = meta_kms_crtc_get_current_state (kms_crtc);

  *size = crtc_state->gamma.size;
  *red = g_memdup2 (crtc_state->gamma.red, *size * sizeof **red);
  *green = g_memdup2 (crtc_state->gamma.green, *size * sizeof **green);
  *blue = g_memdup2 (crtc_state->gamma.blue, *size * sizeof **blue);
}

static char *
generate_gamma_ramp_string (size_t          size,
                            unsigned short *red,
                            unsigned short *green,
                            unsigned short *blue)
{
  GString *string;
  int color;

  string = g_string_new ("[");
  for (color = 0; color < 3; color++)
    {
      unsigned short **color_ptr = NULL;
      char color_char;
      size_t i;

      switch (color)
        {
        case 0:
          color_ptr = &red;
          color_char = 'r';
          break;
        case 1:
          color_ptr = &green;
          color_char = 'g';
          break;
        case 2:
          color_ptr = &blue;
          color_char = 'b';
          break;
        }

      g_assert (color_ptr);
      g_string_append_printf (string, " %c: ", color_char);
      for (i = 0; i < MIN (4, size); i++)
        {
          int j;

          if (size > 4)
            {
              if (i == 2)
                g_string_append (string, ",...");

              if (i >= 2)
                j = i + (size - 4);
              else
                j = i;
            }
          else
            {
              j = i;
            }
          g_string_append_printf (string, "%s%hu",
                                  j == 0 ? "" : ",",
                                  (*color_ptr)[i]);
        }
    }

  g_string_append (string, " ]");

  return g_string_free (string, FALSE);
}

MetaKmsCrtcGamma *
meta_monitor_manager_native_get_cached_crtc_gamma (MetaMonitorManagerNative *manager_native,
                                                   MetaCrtcKms              *crtc_kms)
{
  uint64_t crtc_id;

  crtc_id = meta_crtc_get_id (META_CRTC (crtc_kms));
  return g_hash_table_lookup (manager_native->crtc_gamma_cache,
                              GUINT_TO_POINTER (crtc_id));
}

static void
meta_monitor_manager_native_set_crtc_gamma (MetaMonitorManager *manager,
                                            MetaCrtc           *crtc,
                                            gsize               size,
                                            unsigned short     *red,
                                            unsigned short     *green,
                                            unsigned short     *blue)
{
  MetaMonitorManagerNative *manager_native =
    META_MONITOR_MANAGER_NATIVE (manager);
  MetaCrtcKms *crtc_kms;
  MetaKmsCrtc *kms_crtc;
  g_autofree char *gamma_ramp_string = NULL;
  MetaBackend *backend = meta_monitor_manager_get_backend (manager);
  ClutterStage *stage = CLUTTER_STAGE (meta_backend_get_stage (backend));

  g_return_if_fail (META_IS_CRTC_KMS (crtc));

  crtc_kms = META_CRTC_KMS (crtc);
  kms_crtc = meta_crtc_kms_get_kms_crtc (META_CRTC_KMS (crtc));

  g_hash_table_replace (manager_native->crtc_gamma_cache,
                        GUINT_TO_POINTER (meta_crtc_get_id (crtc)),
                        meta_kms_crtc_gamma_new (kms_crtc, size,
                                                 red, green, blue));

  gamma_ramp_string = generate_gamma_ramp_string (size, red, green, blue);
  g_debug ("Setting CRTC (%" G_GUINT64_FORMAT ") gamma to %s",
           meta_crtc_get_id (crtc), gamma_ramp_string);

  meta_crtc_kms_invalidate_gamma (crtc_kms);
  clutter_stage_schedule_update (stage);
}

static void
handle_hotplug_event (MetaMonitorManager *manager)
{
  meta_monitor_manager_reload (manager);
}

static void
on_kms_resources_changed (MetaKms            *kms,
                          MetaMonitorManager *manager)
{
  handle_hotplug_event (manager);
}

static void
meta_monitor_manager_native_connect_hotplug_handler (MetaMonitorManagerNative *manager_native)
{
  MetaMonitorManager *manager = META_MONITOR_MANAGER (manager_native);
  MetaBackend *backend = meta_monitor_manager_get_backend (manager);
  MetaBackendNative *backend_native = META_BACKEND_NATIVE (backend);
  MetaKms *kms = meta_backend_native_get_kms (backend_native);

  manager_native->kms_resources_changed_handler_id =
    g_signal_connect (kms, "resources-changed",
                      G_CALLBACK (on_kms_resources_changed), manager);
}

static void
meta_monitor_manager_native_disconnect_hotplug_handler (MetaMonitorManagerNative *manager_native)
{
  MetaMonitorManager *manager = META_MONITOR_MANAGER (manager_native);
  MetaBackend *backend = meta_monitor_manager_get_backend (manager);
  MetaBackendNative *backend_native = META_BACKEND_NATIVE (backend);
  MetaKms *kms = meta_backend_native_get_kms (backend_native);

  g_clear_signal_handler (&manager_native->kms_resources_changed_handler_id, kms);
}

void
meta_monitor_manager_native_pause (MetaMonitorManagerNative *manager_native)
{
  meta_monitor_manager_native_disconnect_hotplug_handler (manager_native);
}

void
meta_monitor_manager_native_resume (MetaMonitorManagerNative *manager_native)
{
  MetaMonitorManager *manager = META_MONITOR_MANAGER (manager_native);
  MetaBackend *backend = meta_monitor_manager_get_backend (manager);
  GList *l;

  meta_monitor_manager_native_connect_hotplug_handler (manager_native);

  for (l = meta_backend_get_gpus (backend); l; l = l->next)
    {
      MetaGpu *gpu = l->data;

      g_list_foreach (meta_gpu_get_crtcs (gpu),
                      (GFunc) meta_crtc_kms_invalidate_gamma,
                      NULL);
    }
}

static gboolean
meta_monitor_manager_native_is_transform_handled (MetaMonitorManager  *manager,
                                                  MetaCrtc            *crtc,
                                                  MetaMonitorTransform transform)
{
  return meta_crtc_native_is_transform_handled (META_CRTC_NATIVE (crtc),
                                                transform);
}

static float
meta_monitor_manager_native_calculate_monitor_mode_scale (MetaMonitorManager *manager,
                                                          MetaMonitor        *monitor,
                                                          MetaMonitorMode    *monitor_mode)
{
  return meta_monitor_calculate_mode_scale (monitor, monitor_mode);
}

static float *
meta_monitor_manager_native_calculate_supported_scales (MetaMonitorManager           *manager,
                                                        MetaLogicalMonitorLayoutMode  layout_mode,
                                                        MetaMonitor                  *monitor,
                                                        MetaMonitorMode              *monitor_mode,
                                                        int                          *n_supported_scales)
{
  MetaMonitorScalesConstraint constraints =
    META_MONITOR_SCALES_CONSTRAINT_NONE;

  switch (layout_mode)
    {
    case META_LOGICAL_MONITOR_LAYOUT_MODE_LOGICAL:
      break;
    case META_LOGICAL_MONITOR_LAYOUT_MODE_PHYSICAL:
      constraints |= META_MONITOR_SCALES_CONSTRAINT_NO_FRAC;
      break;
    }

  return meta_monitor_calculate_supported_scales (monitor, monitor_mode,
                                                  constraints,
                                                  n_supported_scales);
}

static MetaMonitorManagerCapability
meta_monitor_manager_native_get_capabilities (MetaMonitorManager *manager)
{
  MetaBackend *backend = meta_monitor_manager_get_backend (manager);
  MetaSettings *settings = meta_backend_get_settings (backend);
  MetaMonitorManagerCapability capabilities =
    META_MONITOR_MANAGER_CAPABILITY_NONE;

  if (meta_settings_is_experimental_feature_enabled (
        settings,
        META_EXPERIMENTAL_FEATURE_SCALE_MONITOR_FRAMEBUFFER))
    capabilities |= META_MONITOR_MANAGER_CAPABILITY_LAYOUT_MODE;

  return capabilities;
}

static gboolean
meta_monitor_manager_native_get_max_screen_size (MetaMonitorManager *manager,
                                                 int                *max_width,
                                                 int                *max_height)
{
  return FALSE;
}

static MetaLogicalMonitorLayoutMode
meta_monitor_manager_native_get_default_layout_mode (MetaMonitorManager *manager)
{
  MetaBackend *backend = meta_monitor_manager_get_backend (manager);
  MetaSettings *settings = meta_backend_get_settings (backend);

  if (meta_settings_is_experimental_feature_enabled (
        settings,
        META_EXPERIMENTAL_FEATURE_SCALE_MONITOR_FRAMEBUFFER))
    return META_LOGICAL_MONITOR_LAYOUT_MODE_LOGICAL;
  else
    return META_LOGICAL_MONITOR_LAYOUT_MODE_PHYSICAL;
}

static MetaVirtualMonitorNative *
find_virtual_monitor (MetaMonitorManagerNative *manager_native,
                      uint64_t                  id)
{
  MetaMonitorManager *manager = META_MONITOR_MANAGER (manager_native);
  GList *l;

  for (l = meta_monitor_manager_get_virtual_monitors (manager); l; l = l->next)
    {
      MetaVirtualMonitorNative *virtual_monitor_native = l->data;

      if (meta_virtual_monitor_native_get_id (virtual_monitor_native) == id)
        return virtual_monitor_native;
    }

  return NULL;
}

static uint64_t
allocate_virtual_monitor_id (MetaMonitorManagerNative *manager_native)
{
  uint64_t id;

  id = 0;

  while (TRUE)
    {
      if (!find_virtual_monitor (manager_native, id))
        return id;

      id++;
    }
}

static MetaVirtualMonitor *
meta_monitor_manager_native_create_virtual_monitor (MetaMonitorManager            *manager,
                                                    const MetaVirtualMonitorInfo  *info,
                                                    GError                       **error)
{
  MetaMonitorManagerNative *manager_native =
    META_MONITOR_MANAGER_NATIVE (manager);
  MetaVirtualMonitorNative *virtual_monitor_native;
  uint64_t id;

  id = allocate_virtual_monitor_id (manager_native);
  virtual_monitor_native = meta_virtual_monitor_native_new (id, info);
  return META_VIRTUAL_MONITOR (virtual_monitor_native);
}

static void
meta_monitor_manager_native_set_property (GObject      *object,
                                          guint         prop_id,
                                          const GValue *value,
                                          GParamSpec   *pspec)
{
  MetaMonitorManagerNative *manager_native =
    META_MONITOR_MANAGER_NATIVE (object);

  switch (prop_id)
    {
    case PROP_NEED_OUTPUTS:
      manager_native->needs_outputs = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
meta_monitor_manager_native_dispose (GObject *object)
{
  MetaMonitorManagerNative *manager_native =
    META_MONITOR_MANAGER_NATIVE (object);

  g_clear_pointer (&manager_native->crtc_gamma_cache,
                   g_hash_table_unref);

  G_OBJECT_CLASS (meta_monitor_manager_native_parent_class)->dispose (object);
}

static gboolean
meta_monitor_manager_native_initable_init (GInitable    *initable,
                                           GCancellable *cancellable,
                                           GError      **error)
{
  MetaMonitorManagerNative *manager_native =
    META_MONITOR_MANAGER_NATIVE (initable);
  MetaMonitorManager *manager = META_MONITOR_MANAGER (manager_native);
  MetaBackend *backend = meta_monitor_manager_get_backend (manager);
  gboolean can_have_outputs;
  GList *l;

  meta_monitor_manager_native_connect_hotplug_handler (manager_native);

  can_have_outputs = FALSE;
  for (l = meta_backend_get_gpus (backend); l; l = l->next)
    {
      MetaGpuKms *gpu_kms = l->data;

      if (meta_gpu_kms_can_have_outputs (gpu_kms))
        {
          can_have_outputs = TRUE;
          break;
        }
    }

  if (manager_native->needs_outputs && !can_have_outputs)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "No GPUs with outputs found");
      return FALSE;
    }

  manager_native->crtc_gamma_cache =
    g_hash_table_new_full (NULL, NULL,
                           NULL,
                           (GDestroyNotify) meta_kms_crtc_gamma_free);

  return TRUE;
}

static void
initable_iface_init (GInitableIface *initable_iface)
{
  initable_iface->init = meta_monitor_manager_native_initable_init;
}

static void
meta_monitor_manager_native_init (MetaMonitorManagerNative *manager_native)
{
  manager_native->needs_outputs = TRUE;
}

static void
meta_monitor_manager_native_class_init (MetaMonitorManagerNativeClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  MetaMonitorManagerClass *manager_class = META_MONITOR_MANAGER_CLASS (klass);

  object_class->set_property = meta_monitor_manager_native_set_property;
  object_class->dispose = meta_monitor_manager_native_dispose;

  manager_class->read_edid =
    meta_monitor_manager_native_read_edid;
  manager_class->read_current_state =
    meta_monitor_manager_native_read_current_state;
  manager_class->ensure_initial_config =
    meta_monitor_manager_native_ensure_initial_config;
  manager_class->apply_monitors_config =
    meta_monitor_manager_native_apply_monitors_config;
  manager_class->set_power_save_mode =
    meta_monitor_manager_native_set_power_save_mode;
  manager_class->get_crtc_gamma =
    meta_monitor_manager_native_get_crtc_gamma;
  manager_class->set_crtc_gamma =
    meta_monitor_manager_native_set_crtc_gamma;
  manager_class->is_transform_handled =
    meta_monitor_manager_native_is_transform_handled;
  manager_class->calculate_monitor_mode_scale =
    meta_monitor_manager_native_calculate_monitor_mode_scale;
  manager_class->calculate_supported_scales =
    meta_monitor_manager_native_calculate_supported_scales;
  manager_class->get_capabilities =
    meta_monitor_manager_native_get_capabilities;
  manager_class->get_max_screen_size =
    meta_monitor_manager_native_get_max_screen_size;
  manager_class->get_default_layout_mode =
    meta_monitor_manager_native_get_default_layout_mode;
  manager_class->create_virtual_monitor =
    meta_monitor_manager_native_create_virtual_monitor;

  obj_props[PROP_NEED_OUTPUTS] =
    g_param_spec_boolean ("needs-outputs",
                          "needs-outputs",
                          "Whether any outputs are needed for operation",
                          TRUE,
                          G_PARAM_WRITABLE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties (object_class, N_PROPS, obj_props);
}
