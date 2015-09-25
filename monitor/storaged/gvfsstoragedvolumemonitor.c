/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/* gvfs - extensions for gio
 *
 * Copyright (C) 2011-2012 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * Author: David Zeuthen <davidz@redhat.com>
 */

#include <config.h>

#include <limits.h>
#include <string.h>
#include <stdlib.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <gio/gio.h>

#include "gvfsstoragedvolumemonitor.h"
#include "gvfsstorageddrive.h"
#include "gvfsstoragedvolume.h"
#include "gvfsstoragedmount.h"
#include "gvfsstoragedutils.h"

static GVfsStoragedVolumeMonitor *the_volume_monitor = NULL;

typedef struct _GVfsStoragedVolumeMonitorClass GVfsStoragedVolumeMonitorClass;

struct _GVfsStoragedVolumeMonitorClass
{
  GNativeVolumeMonitorClass parent_class;
};

struct _GVfsStoragedVolumeMonitor
{
  GNativeVolumeMonitor parent;

  StoragedClient *client;
  GUdevClient *gudev_client;
  GUnixMountMonitor *mount_monitor;

  GList *drives;
  GList *volumes;
  GList *fstab_volumes;
  GList *mounts;
  /* we keep volumes/mounts for blank and audio discs separate to handle e.g. mixed discs properly */
  GList *disc_volumes;
  GList *disc_mounts;
};

static StoragedClient *get_storaged_client_sync (GError **error);

static void update_all               (GVfsStoragedVolumeMonitor  *monitor,
                                      gboolean                   emit_changes,
                                      gboolean                   coldplug);
static void update_drives            (GVfsStoragedVolumeMonitor  *monitor,
                                      GList                    **added_drives,
                                      GList                    **removed_drives,
                                      gboolean                   coldplug);
static void update_volumes           (GVfsStoragedVolumeMonitor  *monitor,
                                      GList                    **added_volumes,
                                      GList                    **removed_volumes,
                                      gboolean                   coldplug);
static void update_fstab_volumes     (GVfsStoragedVolumeMonitor  *monitor,
                                      GList                    **added_volumes,
                                      GList                    **removed_volumes,
                                      gboolean                   coldplug);
static void update_mounts            (GVfsStoragedVolumeMonitor  *monitor,
                                      GList                    **added_mounts,
                                      GList                    **removed_mounts,
                                      gboolean                   coldplug);
static void update_discs             (GVfsStoragedVolumeMonitor  *monitor,
                                      GList                    **added_volumes,
                                      GList                    **removed_volumes,
                                      GList                    **added_mounts,
                                      GList                    **removed_mounts,
                                      gboolean                   coldplug);


static void on_client_changed (StoragedClient *client,
                               gpointer      user_data);

static void mountpoints_changed      (GUnixMountMonitor  *mount_monitor,
                                      gpointer            user_data);

static void mounts_changed           (GUnixMountMonitor  *mount_monitor,
                                      gpointer            user_data);

G_DEFINE_TYPE (GVfsStoragedVolumeMonitor, gvfs_storaged_volume_monitor, G_TYPE_NATIVE_VOLUME_MONITOR)

static void
gvfs_storaged_volume_monitor_dispose (GObject *object)
{
  the_volume_monitor = NULL;

  if (G_OBJECT_CLASS (gvfs_storaged_volume_monitor_parent_class)->dispose != NULL)
    G_OBJECT_CLASS (gvfs_storaged_volume_monitor_parent_class)->dispose (object);
}

static void
gvfs_storaged_volume_monitor_finalize (GObject *object)
{
  GVfsStoragedVolumeMonitor *monitor = GVFS_STORAGED_VOLUME_MONITOR (object);

  g_signal_handlers_disconnect_by_func (monitor->mount_monitor, mountpoints_changed, monitor);
  g_signal_handlers_disconnect_by_func (monitor->mount_monitor, mounts_changed, monitor);
  g_clear_object (&monitor->mount_monitor);

  g_signal_handlers_disconnect_by_func (monitor->client,
                                        G_CALLBACK (on_client_changed),
                                        monitor);

  g_clear_object (&monitor->client);
  g_clear_object (&monitor->gudev_client);

  g_list_free_full (monitor->drives, g_object_unref);
  g_list_free_full (monitor->volumes, g_object_unref);
  g_list_free_full (monitor->fstab_volumes, g_object_unref);
  g_list_free_full (monitor->mounts, g_object_unref);

  g_list_free_full (monitor->disc_volumes, g_object_unref);
  g_list_free_full (monitor->disc_mounts, g_object_unref);

  G_OBJECT_CLASS (gvfs_storaged_volume_monitor_parent_class)->finalize (object);
}

static GList *
get_mounts (GVolumeMonitor *_monitor)
{
  GVfsStoragedVolumeMonitor *monitor = GVFS_STORAGED_VOLUME_MONITOR (_monitor);
  GList *ret;

  ret = g_list_copy (monitor->mounts);
  ret = g_list_concat (ret, g_list_copy (monitor->disc_mounts));
  g_list_foreach (ret, (GFunc) g_object_ref, NULL);
  return ret;
}

static GList *
get_volumes (GVolumeMonitor *_monitor)
{
  GVfsStoragedVolumeMonitor *monitor = GVFS_STORAGED_VOLUME_MONITOR (_monitor);
  GList *ret;

  ret = g_list_copy (monitor->volumes);
  ret = g_list_concat (ret, g_list_copy (monitor->fstab_volumes));
  ret = g_list_concat (ret, g_list_copy (monitor->disc_volumes));
  g_list_foreach (ret, (GFunc) g_object_ref, NULL);
  return ret;
}

static GList *
get_connected_drives (GVolumeMonitor *_monitor)
{
  GVfsStoragedVolumeMonitor *monitor = GVFS_STORAGED_VOLUME_MONITOR (_monitor);
  GList *ret;

  ret = g_list_copy (monitor->drives);
  g_list_foreach (ret, (GFunc) g_object_ref, NULL);
  return ret;
}

static GVolume *
get_volume_for_uuid (GVolumeMonitor *_monitor,
                     const gchar    *uuid)
{
  GVfsStoragedVolumeMonitor *monitor = GVFS_STORAGED_VOLUME_MONITOR (_monitor);
  GVfsStoragedVolume *volume;
  GList *l;

  for (l = monitor->volumes; l != NULL; l = l->next)
    {
      volume = l->data;
      if (gvfs_storaged_volume_has_uuid (l->data, uuid))
        goto found;
    }
  for (l = monitor->fstab_volumes; l != NULL; l = l->next)
    {
      volume = l->data;
      if (gvfs_storaged_volume_has_uuid (l->data, uuid))
        goto found;
    }
  for (l = monitor->disc_volumes; l != NULL; l = l->next)
    {
      volume = l->data;
      if (gvfs_storaged_volume_has_uuid (volume, uuid))
        goto found;
    }

  return NULL;

 found:
  return G_VOLUME (g_object_ref (volume));
}

static GMount *
get_mount_for_uuid (GVolumeMonitor *_monitor,
                    const gchar    *uuid)
{
  GVfsStoragedVolumeMonitor *monitor = GVFS_STORAGED_VOLUME_MONITOR (_monitor);
  GVfsStoragedMount *mount;
  GList *l;

  for (l = monitor->mounts; l != NULL; l = l->next)
    {
      mount = l->data;
      if (gvfs_storaged_mount_has_uuid (l->data, uuid))
        goto found;
    }
  for (l = monitor->disc_mounts; l != NULL; l = l->next)
    {
      mount = l->data;
      if (gvfs_storaged_mount_has_uuid (mount, uuid))
        goto found;
    }

  return NULL;

 found:
  return G_MOUNT (g_object_ref (mount));
}

static GMount *
get_mount_for_mount_path (const gchar  *mount_path,
                          GCancellable *cancellable)
{
  GVfsStoragedVolumeMonitor *monitor = NULL;
  GMount *ret = NULL;

  if (the_volume_monitor == NULL)
    {
      /* Bah, no monitor is set up.. so we have to create one, find
       * what the user asks for and throw it away again.
       */
      monitor = GVFS_STORAGED_VOLUME_MONITOR (gvfs_storaged_volume_monitor_new ());
    }
  else
    {
      monitor = g_object_ref (the_volume_monitor);
    }

  /* creation of the volume monitor could fail */
  if (monitor != NULL)
    {
      GList *l;
      for (l = monitor->mounts; l != NULL; l = l->next)
        {
          GVfsStoragedMount *mount = GVFS_STORAGED_MOUNT (l->data);
          if (g_strcmp0 (gvfs_storaged_mount_get_mount_path (mount), mount_path) == 0)
            {
              ret = g_object_ref (mount);
              goto out;
            }
        }
    }

 out:
  if (monitor != NULL)
    g_object_unref (monitor);
  return ret;
}

static GObject *
gvfs_storaged_volume_monitor_constructor (GType                  type,
                                         guint                  n_construct_properties,
                                         GObjectConstructParam *construct_properties)
{
  GObject *ret = NULL;
  GObjectClass *parent_class;

  if (the_volume_monitor != NULL)
    {
      ret = g_object_ref (the_volume_monitor);
      goto out;
    }

  parent_class = G_OBJECT_CLASS (g_type_class_peek_parent (g_type_class_peek (type)));
  ret = parent_class->constructor (type,
                                   n_construct_properties,
                                   construct_properties);

  the_volume_monitor = GVFS_STORAGED_VOLUME_MONITOR (ret);

 out:
  return ret;
}

static void
gvfs_storaged_volume_monitor_init (GVfsStoragedVolumeMonitor *monitor)
{
  monitor->gudev_client = g_udev_client_new (NULL); /* don't listen to any changes */

  monitor->client = get_storaged_client_sync (NULL);
  g_signal_connect (monitor->client,
                    "changed",
                    G_CALLBACK (on_client_changed),
                    monitor);

  monitor->mount_monitor = g_unix_mount_monitor_get ();
  g_signal_connect (monitor->mount_monitor,
                    "mounts-changed",
                    G_CALLBACK (mounts_changed),
                    monitor);
  g_signal_connect (monitor->mount_monitor,
                    "mountpoints-changed",
                    G_CALLBACK (mountpoints_changed),
                    monitor);

  update_all (monitor, FALSE, TRUE);
}

static gboolean
is_supported (void)
{
  if (get_storaged_client_sync (NULL) != NULL)
    return TRUE;
  return FALSE;
}

static void
gvfs_storaged_volume_monitor_class_init (GVfsStoragedVolumeMonitorClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVolumeMonitorClass *monitor_class = G_VOLUME_MONITOR_CLASS (klass);
  GNativeVolumeMonitorClass *native_class = G_NATIVE_VOLUME_MONITOR_CLASS (klass);

  gobject_class->constructor = gvfs_storaged_volume_monitor_constructor;
  gobject_class->finalize = gvfs_storaged_volume_monitor_finalize;
  gobject_class->dispose = gvfs_storaged_volume_monitor_dispose;

  monitor_class->get_mounts = get_mounts;
  monitor_class->get_volumes = get_volumes;
  monitor_class->get_connected_drives = get_connected_drives;
  monitor_class->get_volume_for_uuid = get_volume_for_uuid;
  monitor_class->get_mount_for_uuid = get_mount_for_uuid;
  monitor_class->is_supported = is_supported;

  native_class->get_mount_for_mount_path = get_mount_for_mount_path;
}

/**
 * gvfs_storaged_volume_monitor_new:
 *
 * Returns:  a new #GVolumeMonitor.
 **/
GVolumeMonitor *
gvfs_storaged_volume_monitor_new (void)
{
  return G_VOLUME_MONITOR (g_object_new (GVFS_TYPE_STORAGED_VOLUME_MONITOR, NULL));
}

/* ---------------------------------------------------------------------------------------------------- */

StoragedClient *
gvfs_storaged_volume_monitor_get_storaged_client (GVfsStoragedVolumeMonitor *monitor)
{
  g_return_val_if_fail (GVFS_IS_STORAGED_VOLUME_MONITOR (monitor), NULL);
  return monitor->client;
}

/* ---------------------------------------------------------------------------------------------------- */

GUdevClient *
gvfs_storaged_volume_monitor_get_gudev_client (GVfsStoragedVolumeMonitor *monitor)
{
  g_return_val_if_fail (GVFS_IS_STORAGED_VOLUME_MONITOR (monitor), NULL);
  return monitor->gudev_client;
}

/* ---------------------------------------------------------------------------------------------------- */

void
gvfs_storaged_volume_monitor_update (GVfsStoragedVolumeMonitor *monitor)
{
  g_return_if_fail (GVFS_IS_STORAGED_VOLUME_MONITOR (monitor));
  storaged_client_settle (monitor->client);
  update_all (monitor, TRUE, FALSE);
}

/* ---------------------------------------------------------------------------------------------------- */

static StoragedClient *
get_storaged_client_sync (GError **error)
{
  static StoragedClient *_client = NULL;
  static GError *_error = NULL;
  static volatile gsize initialized = 0;

  if (g_once_init_enter (&initialized))
    {
      _client = storaged_client_new_sync (NULL, &_error);
      g_once_init_leave (&initialized, 1);
    }

  if (_error != NULL && error != NULL)
    *error = g_error_copy (_error);

  return _client;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
diff_sorted_lists (GList         *list1,
                   GList         *list2,
                   GCompareFunc   compare,
                   GList        **added,
                   GList        **removed,
                   GList        **unchanged)
{
  int order;

  *added = *removed = NULL;
  if (unchanged != NULL)
    *unchanged = NULL;

  while (list1 != NULL &&
         list2 != NULL)
    {
      order = (*compare) (list1->data, list2->data);
      if (order < 0)
        {
          *removed = g_list_prepend (*removed, list1->data);
          list1 = list1->next;
        }
      else if (order > 0)
        {
          *added = g_list_prepend (*added, list2->data);
          list2 = list2->next;
        }
      else
        { /* same item */
          if (unchanged != NULL)
            *unchanged = g_list_prepend (*unchanged, list1->data);
          list1 = list1->next;
          list2 = list2->next;
        }
    }

  while (list1 != NULL)
    {
      *removed = g_list_prepend (*removed, list1->data);
      list1 = list1->next;
    }
  while (list2 != NULL)
    {
      *added = g_list_prepend (*added, list2->data);
      list2 = list2->next;
    }
}

/* ---------------------------------------------------------------------------------------------------- */

static void
object_list_emit (GVfsStoragedVolumeMonitor *monitor,
                  const gchar              *monitor_signal,
                  const gchar              *object_signal,
                  GList                    *objects)
{
  GList *l;
  for (l = objects; l != NULL; l = l->next)
    {
      g_signal_emit_by_name (monitor, monitor_signal, l->data);
      if (object_signal)
        g_signal_emit_by_name (l->data, object_signal);
    }
}

/* ---------------------------------------------------------------------------------------------------- */

static void
on_client_changed (StoragedClient  *client,
                   gpointer       user_data)
{
  GVfsStoragedVolumeMonitor *monitor = GVFS_STORAGED_VOLUME_MONITOR (user_data);
  update_all (monitor, TRUE, FALSE);
}

static void
mountpoints_changed (GUnixMountMonitor *mount_monitor,
                     gpointer           user_data)
{
  GVfsStoragedVolumeMonitor *monitor = GVFS_STORAGED_VOLUME_MONITOR (user_data);
  update_all (monitor, TRUE, FALSE);
}

static void
mounts_changed (GUnixMountMonitor *mount_monitor,
                gpointer           user_data)
{
  GVfsStoragedVolumeMonitor *monitor = GVFS_STORAGED_VOLUME_MONITOR (user_data);
  update_all (monitor, TRUE, FALSE);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
update_all (GVfsStoragedVolumeMonitor *monitor,
            gboolean                  emit_changes,
            gboolean                  coldplug)
{
  GList *added_drives, *removed_drives;
  GList *added_volumes, *removed_volumes;
  GList *added_mounts, *removed_mounts;

  added_drives = NULL;
  removed_drives = NULL;
  added_volumes = NULL;
  removed_volumes = NULL;
  added_mounts = NULL;
  removed_mounts = NULL;

  update_drives (monitor, &added_drives, &removed_drives, coldplug);
  update_volumes (monitor, &added_volumes, &removed_volumes, coldplug);
  update_fstab_volumes (monitor, &added_volumes, &removed_volumes, coldplug);
  update_mounts (monitor, &added_mounts, &removed_mounts, coldplug);
  update_discs (monitor,
                &added_volumes, &removed_volumes,
                &added_mounts, &removed_mounts,
                coldplug);

  if (emit_changes)
    {
      object_list_emit (monitor,
                        "drive-disconnected", NULL,
                        removed_drives);
      object_list_emit (monitor,
                        "drive-connected", NULL,
                        added_drives);

      object_list_emit (monitor,
                        "volume-removed", "removed",
                        removed_volumes);
      object_list_emit (monitor,
                        "volume-added", NULL,
                        added_volumes);

      object_list_emit (monitor,
                        "mount-removed", "unmounted",
                        removed_mounts);
      object_list_emit (monitor,
                        "mount-added", NULL,
                        added_mounts);
    }

  g_list_free_full (removed_drives, g_object_unref);
  g_list_free_full (added_drives, g_object_unref);
  g_list_free_full (removed_volumes, g_object_unref);
  g_list_free_full (added_volumes, g_object_unref);
  g_list_free_full (removed_mounts, g_object_unref);
  g_list_free_full (added_mounts, g_object_unref);
}

/* ---------------------------------------------------------------------------------------------------- */

static GUnixMountPoint *
get_mount_point_for_mount (GUnixMountEntry *mount_entry)
{
  GUnixMountPoint *ret = NULL;
  GList *mount_points, *l;

  mount_points = g_unix_mount_points_get (NULL);
  for (l = mount_points; l != NULL; l = l->next)
    {
      GUnixMountPoint *mount_point = l->data;
      if (g_strcmp0 (g_unix_mount_get_mount_path (mount_entry),
                     g_unix_mount_point_get_mount_path (mount_point)) == 0)
        {
          ret = mount_point;
          goto out;
        }
    }

 out:
  for (l = mount_points; l != NULL; l = l->next)
    {
      GUnixMountPoint *mount_point = l->data;
      if (G_LIKELY (mount_point != ret))
        g_unix_mount_point_free (mount_point);
    }
  g_list_free (mount_points);
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
should_include (const gchar *mount_path,
                const gchar *options)
{
  gboolean ret = FALSE;
  const gchar *home_dir = NULL;
  const gchar *user_name;
  gsize user_name_len;

  g_return_val_if_fail (mount_path != NULL, FALSE);

  /* The x-gvfs-show option trumps everything else */
  if (options != NULL)
    {
      gchar *value;
      value = gvfs_storaged_utils_lookup_fstab_options_value (options, "x-gvfs-show");
      if (value != NULL)
        {
          ret = TRUE;
          g_free (value);
          goto out;
        }
      value = gvfs_storaged_utils_lookup_fstab_options_value (options, "x-gvfs-hide");
      if (value != NULL)
        {
          ret = FALSE;
          g_free (value);
          goto out;
        }
    }

  /* Never display internal mountpoints */
  if (g_unix_is_mount_path_system_internal (mount_path))
    goto out;

  /* Only display things in
   * - /media; and
   * - $HOME; and
   * - /run/media/$USER
   */

  /* Hide mounts within a subdirectory starting with a "." - suppose it was a purpose to hide this mount */
  if (g_strstr_len (mount_path, -1, "/.") != NULL)
    goto out;

  /* Check /media */
  if (g_str_has_prefix (mount_path, "/media/"))
    {
      ret = TRUE;
      goto out;
    }

  /* Check home dir */
  home_dir = g_get_home_dir ();
  if (home_dir != NULL)
    {
      if (g_str_has_prefix (mount_path, home_dir) && mount_path[strlen (home_dir)] == G_DIR_SEPARATOR)
        {
          ret = TRUE;
          goto out;
        }
    }

  /* Check /run/media/$USER/ */
  user_name = g_get_user_name ();
  user_name_len = strlen (user_name);
  if (strncmp (mount_path, "/run/media/", sizeof ("/run/media/") - 1) == 0 &&
      strncmp (mount_path + sizeof ("/run/media/") - 1, user_name, user_name_len) == 0 &&
      mount_path[sizeof ("/run/media/") - 1 + user_name_len] == '/')
    {
      ret = TRUE;
      goto out;
    }

 out:
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
should_include_mount_point (GVfsStoragedVolumeMonitor  *monitor,
                            GUnixMountPoint           *mount_point)
{
  return should_include (g_unix_mount_point_get_mount_path (mount_point),
                         g_unix_mount_point_get_options (mount_point));
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
should_include_mount (GVfsStoragedVolumeMonitor  *monitor,
                      GUnixMountEntry           *mount_entry)
{
  GUnixMountPoint *mount_point;
  gboolean ret;

  /* if mounted at the designated mount point, use that info to decide */
  mount_point = get_mount_point_for_mount (mount_entry);
  if (mount_point != NULL)
    {
      ret = should_include_mount_point (monitor, mount_point);
      g_unix_mount_point_free (mount_point);
      goto out;
    }

  /* otherwise, use the mount's info */
  ret = should_include (g_unix_mount_get_mount_path (mount_entry),
                        NULL); /* no mount options yet - see bug 668132 */

 out:
  return ret;
}


/* ---------------------------------------------------------------------------------------------------- */

static gboolean
should_include_volume_check_mount_points (GVfsStoragedVolumeMonitor *monitor,
                                          StoragedBlock              *block)
{
  gboolean ret = TRUE;
  GDBusObject *obj;
  StoragedFilesystem *fs;
  const gchar* const *mount_points;
  guint n;

  obj = g_dbus_interface_get_object (G_DBUS_INTERFACE (block));
  if (obj == NULL)
    goto out;

  fs = storaged_object_peek_filesystem (STORAGED_OBJECT (obj));
  if (fs == NULL)
    goto out;

  mount_points = storaged_filesystem_get_mount_points (fs);
  for (n = 0; mount_points != NULL && mount_points[n] != NULL; n++)
    {
      const gchar *mount_point = mount_points[n];
      GUnixMountEntry *mount_entry;

      mount_entry = g_unix_mount_at (mount_point, NULL);
      if (mount_entry != NULL)
        {
          if (!should_include_mount (monitor, mount_entry))
            {
              g_unix_mount_free (mount_entry);
              ret = FALSE;
              goto out;
            }
          g_unix_mount_free (mount_entry);
        }
    }

 out:
  return ret;
}

static gboolean
should_include_volume_check_configuration (GVfsStoragedVolumeMonitor *monitor,
                                           StoragedBlock              *block)
{
  gboolean ret = TRUE;
  GVariantIter iter;
  const gchar *configuration_type;
  GVariant *configuration_value;

  g_variant_iter_init (&iter, storaged_block_get_configuration (block));
  while (g_variant_iter_next (&iter, "(&s@a{sv})", &configuration_type, &configuration_value))
    {
      if (g_strcmp0 (configuration_type, "fstab") == 0)
        {
          const gchar *fstab_dir;
          const gchar *fstab_options;
          if (g_variant_lookup (configuration_value, "dir", "^&ay", &fstab_dir) &&
              g_variant_lookup (configuration_value, "opts", "^&ay", &fstab_options))
            {
              if (!should_include (fstab_dir, fstab_options))
                {
                  ret = FALSE;
                  g_variant_unref (configuration_value);
                  goto out;
                }
            }
        }
      g_variant_unref (configuration_value);
    }

 out:
  return ret;
}

static gboolean should_include_drive (GVfsStoragedVolumeMonitor *monitor,
                                      StoragedDrive              *drive);

static gboolean
should_include_volume (GVfsStoragedVolumeMonitor *monitor,
                       StoragedBlock              *block,
                       gboolean                  allow_encrypted_cleartext)
{
  gboolean ret = FALSE;
  GDBusObject *object;
  StoragedFilesystem *filesystem;
  StoragedDrive *storaged_drive = NULL;
  const gchar* const *mount_points;
  StoragedLoop *loop = NULL;

  /* Block:Ignore trumps everything */
  if (storaged_block_get_hint_ignore (block))
    goto out;

  /* If the device (or if a partition, its containing device) is a
   * loop device, check the SetupByUid property - we don't want to
   * show loop devices set up by other users
   */
  loop = storaged_client_get_loop_for_block (monitor->client, block);
  if (loop != NULL)
    {
      GDBusObject *loop_object;
      StoragedBlock *block_for_loop;
      guint setup_by_uid;

      setup_by_uid = storaged_loop_get_setup_by_uid (loop);
      if (setup_by_uid != 0 && setup_by_uid != getuid ())
        goto out;

      /* Work-around bug in Linux where partitions of a loop
       * device (e.g. /dev/loop0p1) are lingering even when the
       * parent loop device (e.g. /dev/loop0) has been cleared
       */
      loop_object = g_dbus_interface_get_object (G_DBUS_INTERFACE (loop));
      if (loop_object == NULL)
        goto out;
      block_for_loop = storaged_object_peek_block (STORAGED_OBJECT (loop_object));
      if (block_for_loop == NULL)
        goto out;
      if (storaged_block_get_size (block_for_loop) == 0)
        goto out;
    }

  /* ignore the volume if the drive is ignored */
  storaged_drive = storaged_client_get_drive_for_block (monitor->client, block);
  if (storaged_drive != NULL)
    {
      if (!should_include_drive (monitor, storaged_drive))
        {
          goto out;
        }
    }

  /* show encrypted volumes... */
  if (g_strcmp0 (storaged_block_get_id_type (block), "crypto_LUKS") == 0)
    {
      StoragedBlock *cleartext_block;
      /* ... unless the volume is unlocked and we don't want to show the cleartext volume */
      cleartext_block = storaged_client_get_cleartext_block (monitor->client, block);
      if (cleartext_block != NULL)
        {
          ret = should_include_volume (monitor, cleartext_block, TRUE);
          g_object_unref (cleartext_block);
        }
      else
        {
          ret = TRUE;
        }
      goto out;
    }

  if (!allow_encrypted_cleartext)
    {
      /* ... but not unlocked volumes (because the volume for the encrypted part morphs
       * into the cleartext part when unlocked)
       */
      if (g_strcmp0 (storaged_block_get_crypto_backing_device (block), "/") != 0)
        {
          goto out;
        }
    }

  /* Check should_include_mount() for all mount points, if any - e.g. if a volume
   * is mounted in a place where the mount is to be ignored, we ignore the volume
   * as well
   */
  if (!should_include_volume_check_mount_points (monitor, block))
    goto out;

  object = g_dbus_interface_get_object (G_DBUS_INTERFACE (block));
  if (object == NULL)
    goto out;

  filesystem = storaged_object_peek_filesystem (STORAGED_OBJECT (object));
  if (filesystem == NULL)
    goto out;

  /* If not mounted but the volume is referenced in /etc/fstab and
   * that configuration indicates the volume should be ignored, then
   * do so
   */
  mount_points = storaged_filesystem_get_mount_points (filesystem);
  if (mount_points == NULL || g_strv_length ((gchar **) mount_points) == 0)
    {
      if (!should_include_volume_check_configuration (monitor, block))
        goto out;
    }

  /* otherwise, we're good to go */
  ret = TRUE;

 out:
  g_clear_object (&storaged_drive);
  g_clear_object (&loop);
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
should_include_drive (GVfsStoragedVolumeMonitor *monitor,
                      StoragedDrive              *drive)
{
  gboolean ret = TRUE;

  /* Don't include drives on other seats */
  if (!gvfs_storaged_utils_is_drive_on_our_seat (drive))
    {
      ret = FALSE;
      goto out;
    }

  /* NOTE: For now, we just include a drive no matter its
   * content. This may be wrong ... for example non-removable drives
   * without anything visible (such RAID components) should probably
   * not be shown. Then again, the GNOME 3 user interface doesn't
   * really show GDrive instances except for in the computer:///
   * location in Nautilus....
   */

 out:

  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static gint
storaged_drive_compare (StoragedDrive *a, StoragedDrive *b)
{
  GDBusObject *oa = g_dbus_interface_get_object (G_DBUS_INTERFACE (a));
  GDBusObject *ob = g_dbus_interface_get_object (G_DBUS_INTERFACE (b));
  /* Either or both of oa, ob can be NULL for the case where a drive
   * is removed but we still hold a reference to the drive interface
   */
  if (oa != NULL && ob != NULL)
    return g_strcmp0 (g_dbus_object_get_object_path (oa), g_dbus_object_get_object_path (ob));
  else
    return (const gchar*) ob - (const gchar*) oa;
}

static gint
block_compare (StoragedBlock *a, StoragedBlock *b)
{
  return g_strcmp0 (storaged_block_get_device (a), storaged_block_get_device (b));
}

/* ---------------------------------------------------------------------------------------------------- */

static GVfsStoragedVolume *
find_disc_volume_for_storaged_drive (GVfsStoragedVolumeMonitor *monitor,
                                   StoragedDrive              *storaged_drive)
{
  GVfsStoragedVolume *ret = NULL;
  GList *l;

  for (l = monitor->disc_volumes; l != NULL; l = l->next)
    {
      GVolume *volume = G_VOLUME (l->data);
      GDrive *drive = g_volume_get_drive (volume);
      if (drive != NULL)
        {
          if (gvfs_storaged_drive_get_storaged_drive (GVFS_STORAGED_DRIVE (drive)) == storaged_drive)
            {
              ret = GVFS_STORAGED_VOLUME (volume);
              g_object_unref (drive);
              goto out;
            }
          g_object_unref (drive);
        }
    }

 out:
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static GVfsStoragedMount *
find_disc_mount_for_storaged_drive (GVfsStoragedVolumeMonitor *monitor,
                                  StoragedDrive              *storaged_drive)
{
  GVfsStoragedMount *ret = NULL;
  GList *l;

  for (l = monitor->disc_mounts; l != NULL; l = l->next)
    {
      GMount *mount = G_MOUNT (l->data);
      GVolume *volume;

      volume = g_mount_get_volume (mount);
      if (volume != NULL)
        {
          GDrive *drive = g_volume_get_drive (volume);
          if (drive != NULL)
            {
              if (gvfs_storaged_drive_get_storaged_drive (GVFS_STORAGED_DRIVE (drive)) == storaged_drive)
                {
                  ret = GVFS_STORAGED_MOUNT (mount);
                  g_object_unref (volume);
                  g_object_unref (drive);
                  goto out;
                }
              g_object_unref (drive);
            }
          g_object_unref (volume);
        }
    }

 out:
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static GVfsStoragedDrive *
find_drive_for_storaged_drive (GVfsStoragedVolumeMonitor *monitor,
                             StoragedDrive              *storaged_drive)
{
  GVfsStoragedDrive *ret = NULL;
  GList *l;

  for (l = monitor->drives; l != NULL; l = l->next)
    {
      GVfsStoragedDrive *drive = GVFS_STORAGED_DRIVE (l->data);
      if (gvfs_storaged_drive_get_storaged_drive (drive) == storaged_drive)
        {
          ret = drive;
          goto out;
        }
    }

 out:
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static GVfsStoragedVolume *
find_volume_for_block (GVfsStoragedVolumeMonitor *monitor,
                       StoragedBlock              *block)
{
  GVfsStoragedVolume *ret = NULL;
  GList *l;

  for (l = monitor->volumes; l != NULL; l = l->next)
    {
      GVfsStoragedVolume *volume = GVFS_STORAGED_VOLUME (l->data);
      if (gvfs_storaged_volume_get_block (volume) == block)
        {
          ret = volume;
          goto out;
        }
    }

 out:
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static GVfsStoragedVolume *
find_fstab_volume_for_mount_point (GVfsStoragedVolumeMonitor *monitor,
                                   GUnixMountPoint          *mount_point)
{
  GVfsStoragedVolume *ret = NULL;
  GList *l;

  for (l = monitor->fstab_volumes; l != NULL; l = l->next)
    {
      GVfsStoragedVolume *volume = GVFS_STORAGED_VOLUME (l->data);
      if (g_unix_mount_point_compare (gvfs_storaged_volume_get_mount_point (volume), mount_point) == 0)
        {
          ret = volume;
          goto out;
        }
    }

 out:
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
mount_point_matches_mount_entry (GUnixMountPoint *mount_point,
                                 GUnixMountEntry *mount_entry)
{
  gboolean ret = FALSE;
  gchar *mp_path = NULL;
  gchar *mp_entry = NULL;

  mp_path = g_strdup (g_unix_mount_point_get_mount_path (mount_point));
  mp_entry = g_strdup (g_unix_mount_get_mount_path (mount_entry));

  if (g_str_has_suffix (mp_path, "/"))
    mp_path[strlen(mp_path) - 1] = '\0';
  if (g_str_has_suffix (mp_entry, "/"))
    mp_entry[strlen(mp_entry) - 1] = '\0';

  if (g_strcmp0 (mp_path, mp_entry) != 0)
    goto out;

  ret = TRUE;

 out:
  g_free (mp_path);
  g_free (mp_entry);
  return ret;
}

static GVfsStoragedVolume *
find_fstab_volume_for_mount_entry (GVfsStoragedVolumeMonitor *monitor,
                                   GUnixMountEntry          *mount_entry)
{
  GVfsStoragedVolume *ret = NULL;
  GList *l;

  for (l = monitor->fstab_volumes; l != NULL; l = l->next)
    {
      GVfsStoragedVolume *volume = GVFS_STORAGED_VOLUME (l->data);
      if (mount_point_matches_mount_entry (gvfs_storaged_volume_get_mount_point (volume), mount_entry))
        {
          ret = volume;
          goto out;
        }
    }

 out:
  return ret;
}

static GVfsStoragedMount *
find_lonely_mount_for_mount_point (GVfsStoragedVolumeMonitor *monitor,
                                   GUnixMountPoint          *mount_point)
{
  GVfsStoragedMount *ret = NULL;
  GList *l;

  for (l = monitor->mounts; l != NULL; l = l->next)
    {
      GVfsStoragedMount *mount = GVFS_STORAGED_MOUNT (l->data);
      if (mount_point != NULL &&
          mount_point_matches_mount_entry (mount_point, gvfs_storaged_mount_get_mount_entry (mount)))
        {
          GVolume *volume = g_mount_get_volume (G_MOUNT (mount));
          if (volume != NULL)
            {
              g_object_unref (volume);
            }
          else
            {
              ret = mount;
              goto out;
            }
        }
    }

 out:
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static GVfsStoragedVolume *
find_volume_for_device (GVfsStoragedVolumeMonitor *monitor,
                        const gchar              *device)
{
  GVfsStoragedVolume *ret = NULL;
  GList *blocks = NULL;
  GList *l;
  struct stat statbuf;

  /* don't consider e.g. network mounts */
  if (g_str_has_prefix (device, "LABEL="))
    {
      blocks = storaged_client_get_block_for_label (monitor->client, device + 6);
      if (blocks != NULL)
        device = storaged_block_get_device (STORAGED_BLOCK (blocks->data));
      else
        goto out;
    }
  else if (g_str_has_prefix (device, "UUID="))
    {
      blocks = storaged_client_get_block_for_uuid (monitor->client, device + 6);
      if (blocks != NULL)
        device = storaged_block_get_device (STORAGED_BLOCK (blocks->data));
      else
        goto out;
    }
  else if (!g_str_has_prefix (device, "/dev/"))
    {
      goto out;
    }

  if (stat (device, &statbuf) != 0)
    goto out;

  for (l = monitor->volumes; l != NULL; l = l->next)
    {
      GVfsStoragedVolume *volume = GVFS_STORAGED_VOLUME (l->data);
      if (gvfs_storaged_volume_get_dev (volume) == statbuf.st_rdev)
        {
          ret = volume;
          goto out;
        }
    }

  for (l = monitor->disc_volumes; l != NULL; l = l->next)
    {
      GVfsStoragedVolume *volume = GVFS_STORAGED_VOLUME (l->data);
      if (gvfs_storaged_volume_get_dev (volume) == statbuf.st_rdev)
        {
          ret = volume;
          goto out;
        }
    }

 out:
  g_list_free_full (blocks, g_object_unref);
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static GVfsStoragedMount *
find_mount_by_mount_path (GVfsStoragedVolumeMonitor *monitor,
                          const gchar              *mount_path)
{
  GVfsStoragedMount *ret = NULL;
  GList *l;

  for (l = monitor->mounts; l != NULL; l = l->next)
    {
      GVfsStoragedMount *mount = GVFS_STORAGED_MOUNT (l->data);
      if (g_strcmp0 (gvfs_storaged_mount_get_mount_path (mount), mount_path) == 0)
        {
          ret = mount;
          goto out;
        }
    }
 out:
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
update_drives (GVfsStoragedVolumeMonitor  *monitor,
               GList                    **added_drives,
               GList                    **removed_drives,
               gboolean                   coldplug)
{
  GList *cur_storaged_drives;
  GList *new_storaged_drives;
  GList *removed, *added;
  GList *l;
  GVfsStoragedDrive *drive;
  GList *objects;

  objects = g_dbus_object_manager_get_objects (storaged_client_get_object_manager (monitor->client));

  cur_storaged_drives = NULL;
  for (l = monitor->drives; l != NULL; l = l->next)
    {
      cur_storaged_drives = g_list_prepend (cur_storaged_drives,
                                          gvfs_storaged_drive_get_storaged_drive (GVFS_STORAGED_DRIVE (l->data)));
    }

  /* remove devices we want to ignore - we do it here so we get to reevaluate
   * on the next update whether they should still be ignored
   */
  new_storaged_drives = NULL;
  for (l = objects; l != NULL; l = l->next)
    {
      StoragedDrive *storaged_drive = storaged_object_peek_drive (STORAGED_OBJECT (l->data));
      if (storaged_drive == NULL)
        continue;
      if (should_include_drive (monitor, storaged_drive))
        new_storaged_drives = g_list_prepend (new_storaged_drives, storaged_drive);
    }

  cur_storaged_drives = g_list_sort (cur_storaged_drives, (GCompareFunc) storaged_drive_compare);
  new_storaged_drives = g_list_sort (new_storaged_drives, (GCompareFunc) storaged_drive_compare);
  diff_sorted_lists (cur_storaged_drives,
                     new_storaged_drives, (GCompareFunc) storaged_drive_compare,
                     &added, &removed, NULL);

  for (l = removed; l != NULL; l = l->next)
    {
      StoragedDrive *storaged_drive = STORAGED_DRIVE (l->data);

      drive = find_drive_for_storaged_drive (monitor, storaged_drive);
      if (drive != NULL)
        {
          /*g_debug ("removing drive %s", gdu_presentable_get_id (p));*/
          gvfs_storaged_drive_disconnected (drive);
          monitor->drives = g_list_remove (monitor->drives, drive);
          *removed_drives = g_list_prepend (*removed_drives, g_object_ref (drive));
          g_object_unref (drive);
        }
    }

  for (l = added; l != NULL; l = l->next)
    {
      StoragedDrive *storaged_drive = STORAGED_DRIVE (l->data);

      drive = find_drive_for_storaged_drive (monitor, storaged_drive);
      if (drive == NULL)
        {
          /*g_debug ("adding drive %s", gdu_presentable_get_id (p));*/
          drive = gvfs_storaged_drive_new (monitor, storaged_drive, coldplug);
          if (storaged_drive != NULL)
            {
              monitor->drives = g_list_prepend (monitor->drives, drive);
              *added_drives = g_list_prepend (*added_drives, g_object_ref (drive));
            }
        }
    }

  g_list_free (added);
  g_list_free (removed);

  g_list_free (cur_storaged_drives);
  g_list_free (new_storaged_drives);

  g_list_free_full (objects, g_object_unref);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
update_volumes (GVfsStoragedVolumeMonitor  *monitor,
                GList                    **added_volumes,
                GList                    **removed_volumes,
                gboolean                   coldplug)
{
  GList *cur_block_volumes;
  GList *new_block_volumes;
  GList *removed, *added;
  GList *l;
  GVfsStoragedVolume *volume;
  GList *objects;

  objects = g_dbus_object_manager_get_objects (storaged_client_get_object_manager (monitor->client));

  cur_block_volumes = NULL;
  for (l = monitor->volumes; l != NULL; l = l->next)
    {
      cur_block_volumes = g_list_prepend (cur_block_volumes,
                                          gvfs_storaged_volume_get_block (GVFS_STORAGED_VOLUME (l->data)));
    }

  new_block_volumes = NULL;
  for (l = objects; l != NULL; l = l->next)
    {
      StoragedBlock *block = storaged_object_peek_block (STORAGED_OBJECT (l->data));
      if (block == NULL)
        continue;
      if (should_include_volume (monitor, block, FALSE))
        new_block_volumes = g_list_prepend (new_block_volumes, block);
    }

  cur_block_volumes = g_list_sort (cur_block_volumes, (GCompareFunc) block_compare);
  new_block_volumes = g_list_sort (new_block_volumes, (GCompareFunc) block_compare);
  diff_sorted_lists (cur_block_volumes,
                     new_block_volumes, (GCompareFunc) block_compare,
                     &added, &removed, NULL);

  for (l = removed; l != NULL; l = l->next)
    {
      StoragedBlock *block = STORAGED_BLOCK (l->data);
      volume = find_volume_for_block (monitor, block);
      if (volume != NULL)
        {
          gvfs_storaged_volume_removed (volume);
          monitor->volumes = g_list_remove (monitor->volumes, volume);
          *removed_volumes = g_list_prepend (*removed_volumes, g_object_ref (volume));
          g_object_unref (volume);
        }
    }

  for (l = added; l != NULL; l = l->next)
    {
      StoragedBlock *block = STORAGED_BLOCK (l->data);
      volume = find_volume_for_block (monitor, block);
      if (volume == NULL)
        {
          GVfsStoragedDrive *drive = NULL;
          StoragedDrive *storaged_drive;

          storaged_drive = storaged_client_get_drive_for_block (monitor->client, block);
          if (storaged_drive != NULL)
            {
              drive = find_drive_for_storaged_drive (monitor, storaged_drive);
              g_object_unref (storaged_drive);
            }
          volume = gvfs_storaged_volume_new (monitor,
                                            block,
                                            NULL, /* mount_point */
                                            drive,
                                            NULL, /* activation_root */
                                            coldplug);
          if (volume != NULL)
            {
              monitor->volumes = g_list_prepend (monitor->volumes, volume);
              *added_volumes = g_list_prepend (*added_volumes, g_object_ref (volume));
            }
         }
    }

  g_list_free (added);
  g_list_free (removed);
  g_list_free (new_block_volumes);
  g_list_free (cur_block_volumes);

  g_list_free_full (objects, g_object_unref);
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
have_storaged_volume_for_mount_point (GVfsStoragedVolumeMonitor *monitor,
                                    GUnixMountPoint          *mount_point)
{
  gboolean ret = FALSE;

  if (find_volume_for_device (monitor, g_unix_mount_point_get_device_path (mount_point)) == NULL)
    goto out;

  ret = TRUE;

 out:
  return ret;
}

static gboolean
mount_point_has_device (GVfsStoragedVolumeMonitor  *monitor,
                        GUnixMountPoint           *mount_point)
{
  gboolean ret = FALSE;
  const gchar *device;
  struct stat statbuf;
  StoragedBlock *block;
  GList *blocks = NULL;

  device = g_unix_mount_point_get_device_path (mount_point);
  if (g_str_has_prefix (device, "LABEL="))
    {
      blocks = storaged_client_get_block_for_label (monitor->client, device + 6);
      if (blocks != NULL)
        device = storaged_block_get_device (STORAGED_BLOCK (blocks->data));
      else
        goto out;
    }
  else if (g_str_has_prefix (device, "UUID="))
    {
      blocks = storaged_client_get_block_for_uuid (monitor->client, device + 6);
      if (blocks != NULL)
        device = storaged_block_get_device (STORAGED_BLOCK (blocks->data));
      else
        goto out;
    }
  else if (!g_str_has_prefix (device, "/dev/"))
    {
      /* NFS, CIFS and other non-device mounts always have a device */
      ret = TRUE;
      goto out;
    }

  if (stat (device, &statbuf) != 0)
    goto out;

  if (statbuf.st_rdev == 0)
    goto out;

  /* assume non-existant if media is not available */
  block = storaged_client_get_block_for_dev (monitor->client, statbuf.st_rdev);
  if (block != NULL)
    {
      StoragedDrive *drive;
      drive = storaged_client_get_drive_for_block (monitor->client, block);
      if (drive != NULL)
        {
          if (!storaged_drive_get_media_available (drive))
            {
              g_object_unref (drive);
              g_object_unref (block);
              goto out;
            }
          g_object_unref (drive);
        }
      g_object_unref (block);
    }
  else
    {
      /* not known by udisks, assume media is available */
    }

  ret = TRUE;

 out:
  g_list_free_full (blocks, g_object_unref);
  return ret;
}

static void
update_fstab_volumes (GVfsStoragedVolumeMonitor  *monitor,
                      GList                    **added_volumes,
                      GList                    **removed_volumes,
                      gboolean                   coldplug)
{
  GList *cur_mount_points;
  GList *new_mount_points;
  GList *added;
  GList *removed;
  GList *l, *ll;
  GVfsStoragedVolume *volume;

  cur_mount_points = NULL;
  for (l = monitor->fstab_volumes; l != NULL; l = l->next)
    {
      GUnixMountPoint *mount_point = gvfs_storaged_volume_get_mount_point (GVFS_STORAGED_VOLUME (l->data));
      if (mount_point != NULL)
        cur_mount_points = g_list_prepend (cur_mount_points, mount_point);
    }

  new_mount_points = g_unix_mount_points_get (NULL);
  /* filter the mount points that we don't want to include */
  for (l = new_mount_points; l != NULL; l = ll)
    {
      GUnixMountPoint *mount_point = l->data;

      ll = l->next;

      if (!should_include_mount_point (monitor, mount_point) ||
          have_storaged_volume_for_mount_point (monitor, mount_point) ||
          !mount_point_has_device (monitor, mount_point))
        {
          new_mount_points = g_list_remove_link (new_mount_points, l);
          g_unix_mount_point_free (mount_point);
        }
    }

  cur_mount_points = g_list_sort (cur_mount_points, (GCompareFunc) g_unix_mount_point_compare);
  new_mount_points = g_list_sort (new_mount_points, (GCompareFunc) g_unix_mount_point_compare);
  diff_sorted_lists (cur_mount_points,
                     new_mount_points, (GCompareFunc) g_unix_mount_point_compare,
                     &added, &removed, NULL);

  for (l = removed; l != NULL; l = l->next)
    {
      GUnixMountPoint *mount_point = l->data;
      volume = find_fstab_volume_for_mount_point (monitor, mount_point);
      if (volume != NULL)
        {
          gvfs_storaged_volume_removed (volume);
          monitor->fstab_volumes = g_list_remove (monitor->fstab_volumes, volume);
          *removed_volumes = g_list_prepend (*removed_volumes, g_object_ref (volume));
          g_object_unref (volume);
        }
    }

  for (l = added; l != NULL; l = l->next)
    {
      GUnixMountPoint *mount_point = l->data;

      volume = find_fstab_volume_for_mount_point (monitor, mount_point);
      if (volume != NULL)
        continue;

      volume = gvfs_storaged_volume_new (monitor,
                                        NULL,        /* block */
                                        mount_point,
                                        NULL,        /* drive */
                                        NULL,        /* activation_root */
                                        coldplug);
      if (volume != NULL)
        {
          GVfsStoragedMount *mount;

          monitor->fstab_volumes = g_list_prepend (monitor->fstab_volumes, volume);
          *added_volumes = g_list_prepend (*added_volumes, g_object_ref (volume));
          /* since @volume takes ownership of @mount_point, don't free it below */
          new_mount_points = g_list_remove (new_mount_points, mount_point);

          /* Could be there's already a mount for this volume - for example, the
           * user could just have added it to the /etc/fstab file
           */
          mount = find_lonely_mount_for_mount_point (monitor, mount_point);
          if (mount != NULL)
            gvfs_storaged_mount_set_volume (mount, volume);
        }
    }

  g_list_free_full (new_mount_points, (GDestroyNotify) g_unix_mount_point_free);

  g_list_free (cur_mount_points);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
update_mounts (GVfsStoragedVolumeMonitor  *monitor,
               GList                    **added_mounts,
               GList                    **removed_mounts,
               gboolean                   coldplug)
{
  GList *cur_mounts;
  GList *new_mounts;
  GList *removed, *added, *unchanged;
  GList *l, *ll;
  GVfsStoragedMount *mount;
  GVfsStoragedVolume *volume;

  cur_mounts = NULL;
  for (l = monitor->mounts; l != NULL; l = l->next)
    {
      GUnixMountEntry *mount_entry;

      mount = GVFS_STORAGED_MOUNT (l->data);
      mount_entry = gvfs_storaged_mount_get_mount_entry (mount);
      if (mount_entry != NULL)
        cur_mounts = g_list_prepend (cur_mounts, mount_entry);
    }

  new_mounts = g_unix_mounts_get (NULL);
  /* remove mounts we want to ignore - we do it here so we get to reevaluate
   * on the next update whether they should still be ignored
   */
  for (l = new_mounts; l != NULL; l = ll)
    {
      GUnixMountEntry *mount_entry = l->data;
      ll = l->next;
      if (!should_include_mount (monitor, mount_entry))
        {
          g_unix_mount_free (mount_entry);
          new_mounts = g_list_delete_link (new_mounts, l);
        }
    }

  cur_mounts = g_list_sort (cur_mounts, (GCompareFunc) g_unix_mount_compare);
  new_mounts = g_list_sort (new_mounts, (GCompareFunc) g_unix_mount_compare);
  diff_sorted_lists (cur_mounts,
                     new_mounts, (GCompareFunc) g_unix_mount_compare,
                     &added, &removed, &unchanged);

  for (l = removed; l != NULL; l = l->next)
    {
      GUnixMountEntry *mount_entry = l->data;
      mount = find_mount_by_mount_path (monitor, g_unix_mount_get_mount_path (mount_entry));
      if (mount != NULL)
        {
          gvfs_storaged_mount_unmounted (mount);
          monitor->mounts = g_list_remove (monitor->mounts, mount);
          *removed_mounts = g_list_prepend (*removed_mounts, g_object_ref (mount));
          /*g_debug ("removed mount at %s", gvfs_storaged_mount_get_mount_path (mount));*/
          g_object_unref (mount);
        }
    }

  for (l = added; l != NULL; l = l->next)
    {
      GUnixMountEntry *mount_entry = l->data;
      volume = find_volume_for_device (monitor, g_unix_mount_get_device_path (mount_entry));
      if (volume == NULL)
        volume = find_fstab_volume_for_mount_entry (monitor, mount_entry);
      mount = gvfs_storaged_mount_new (monitor, mount_entry, volume); /* adopts mount_entry */
      if (mount != NULL)
        {
          monitor->mounts = g_list_prepend (monitor->mounts, mount);
          *added_mounts = g_list_prepend (*added_mounts, g_object_ref (mount));
          /*g_debug ("added mount at %s for %p", gvfs_storaged_mount_get_mount_path (mount), volume);*/
          /* since @mount takes ownership of @mount_entry, don't free it below */
          new_mounts = g_list_remove (new_mounts, mount_entry);
        }
    }

  /* Handle the case where the volume containing the mount appears *after*
   * the mount.
   *
   * This can happen when unlocking+mounting a LUKS device and the two
   * operations are *right* after each other. In that case we get the
   * event from GUnixMountMonitor (which monitors /proc/mounts) before
   * the event from udisks.
   */
  for (l = unchanged; l != NULL; l = l->next)
    {
      GUnixMountEntry *mount_entry = l->data;
      mount = find_mount_by_mount_path (monitor, g_unix_mount_get_mount_path (mount_entry));
      if (mount == NULL)
        {
          g_warning ("No mount object for path %s", g_unix_mount_get_mount_path (mount_entry));
          continue;
        }
      if (gvfs_storaged_mount_get_volume (mount) == NULL)
        {
          volume = find_volume_for_device (monitor, g_unix_mount_get_device_path (mount_entry));
          if (volume == NULL)
            volume = find_fstab_volume_for_mount_entry (monitor, mount_entry);
          if (volume != NULL)
            gvfs_storaged_mount_set_volume (mount, volume);
        }
    }

  g_list_free (added);
  g_list_free (removed);
  g_list_free (unchanged);

  g_list_free_full (new_mounts, (GDestroyNotify) g_unix_mount_free);

  g_list_free (cur_mounts);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
update_discs (GVfsStoragedVolumeMonitor  *monitor,
              GList                    **added_volumes,
              GList                    **removed_volumes,
              GList                    **added_mounts,
              GList                    **removed_mounts,
              gboolean                   coldplug)
{
  GList *objects;
  GList *cur_disc_drives;
  GList *new_disc_drives;
  GList *removed, *added;
  GList *l;
  GVfsStoragedDrive *drive;
  GVfsStoragedVolume *volume;
  GVfsStoragedMount *mount;

  /* we also need to generate GVolume + GMount objects for
   *
   * - optical discs with audio
   * - optical discs that are blank
   *
   */

  objects = g_dbus_object_manager_get_objects (storaged_client_get_object_manager (monitor->client));

  cur_disc_drives = NULL;
  for (l = monitor->disc_volumes; l != NULL; l = l->next)
    {
      volume = GVFS_STORAGED_VOLUME (l->data);
      drive = GVFS_STORAGED_DRIVE (g_volume_get_drive (G_VOLUME (volume)));
      if (drive != NULL)
        {
          cur_disc_drives = g_list_prepend (cur_disc_drives, gvfs_storaged_drive_get_storaged_drive (drive));
          g_object_unref (drive);
        }
    }

  new_disc_drives = NULL;
  for (l = objects; l != NULL; l = l->next)
    {
      StoragedDrive *storaged_drive = storaged_object_peek_drive (STORAGED_OBJECT (l->data));
      if (storaged_drive == NULL)
        continue;

      if (!should_include_drive (monitor, storaged_drive))
        continue;

      /* only consider blank and audio discs */
      if (!(storaged_drive_get_optical_blank (storaged_drive) ||
            storaged_drive_get_optical_num_audio_tracks (storaged_drive) > 0))
        continue;

      new_disc_drives = g_list_prepend (new_disc_drives, storaged_drive);
    }

  cur_disc_drives = g_list_sort (cur_disc_drives, (GCompareFunc) storaged_drive_compare);
  new_disc_drives = g_list_sort (new_disc_drives, (GCompareFunc) storaged_drive_compare);
  diff_sorted_lists (cur_disc_drives, new_disc_drives, (GCompareFunc) storaged_drive_compare,
                     &added, &removed, NULL);

  for (l = removed; l != NULL; l = l->next)
    {
      StoragedDrive *storaged_drive = STORAGED_DRIVE (l->data);

      volume = find_disc_volume_for_storaged_drive (monitor, storaged_drive);
      mount = find_disc_mount_for_storaged_drive (monitor, storaged_drive);

      if (mount != NULL)
        {
          gvfs_storaged_mount_unmounted (mount);
          monitor->disc_mounts = g_list_remove (monitor->disc_mounts, mount);
          *removed_mounts = g_list_prepend (*removed_mounts, mount);
        }
      if (volume != NULL)
        {
          gvfs_storaged_volume_removed (volume);
          monitor->disc_volumes = g_list_remove (monitor->disc_volumes, volume);
          *removed_volumes = g_list_prepend (*removed_volumes, volume);
        }
    }

  for (l = added; l != NULL; l = l->next)
    {
      StoragedDrive *storaged_drive = STORAGED_DRIVE (l->data);
      StoragedBlock *block;

      block = storaged_client_get_block_for_drive (monitor->client, storaged_drive, FALSE);
      if (block != NULL)
        {
          volume = find_disc_volume_for_storaged_drive (monitor, storaged_drive);
          if (volume == NULL)
            {
              gchar *uri;
              GFile *activation_root;
              if (storaged_drive_get_optical_blank (storaged_drive))
                {
                  uri = g_strdup ("burn://");
                }
              else
                {
                  gchar *basename = g_path_get_basename (storaged_block_get_device (block));
                  uri = g_strdup_printf ("cdda://%s", basename);
                  g_free (basename);
                }
              activation_root = g_file_new_for_uri (uri);
              volume = gvfs_storaged_volume_new (monitor,
                                                block,
                                                NULL, /* mount_point */
                                                find_drive_for_storaged_drive (monitor, storaged_drive),
                                                activation_root,
                                                coldplug);
              if (volume != NULL)
                {
                  monitor->disc_volumes = g_list_prepend (monitor->disc_volumes, volume);
                  *added_volumes = g_list_prepend (*added_volumes, g_object_ref (volume));

                  if (storaged_drive_get_optical_blank (storaged_drive))
                    {
                      mount = gvfs_storaged_mount_new (monitor,
                                                      NULL, /* GUnixMountEntry */
                                                      volume);
                      if (mount != NULL)
                        {
                          monitor->disc_mounts = g_list_prepend (monitor->disc_mounts, mount);
                          *added_mounts = g_list_prepend (*added_mounts, g_object_ref (mount));
                        }
                    }
                }

              g_object_unref (activation_root);
              g_free (uri);
            }
          g_object_unref (block);
        }
    }

  g_list_free (added);
  g_list_free (removed);

  g_list_free (new_disc_drives);
  g_list_free (cur_disc_drives);

  g_list_free_full (objects, g_object_unref);
}

/* ---------------------------------------------------------------------------------------------------- */
