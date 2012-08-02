/* GIO - GLib Input, Output and Streaming Library
 *   MTP Backend
 * 
 * Copyright (C) 2012 Philip Langdale <philipl@overt.org>
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
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifndef __G_VFS_BACKEND_MTP_H__
#define __G_VFS_BACKEND_MTP_H__

#include <gvfsbackend.h>
#include <gmountspec.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_BACKEND_MTP         (g_vfs_backend_mtp_get_type ())
#define G_VFS_BACKEND_MTP(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_BACKEND_MTP, GVfsBackendMtp))
#define G_VFS_BACKEND_MTP_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_BACKEND_MTP, GVfsBackendMtpClass))
#define G_VFS_IS_BACKEND_MTP(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_BACKEND_MTP))
#define G_VFS_IS_BACKEND_MTP_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_BACKEND_MTP))
#define G_VFS_BACKEND_MTP_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_BACKEND_MTP, GVfsBackendMtpClass))

typedef struct _GVfsBackendMtp        GVfsBackendMtp;
typedef struct _GVfsBackendMtpClass   GVfsBackendMtpClass;

struct _GVfsBackendMtp
{
  GVfsBackend parent_instance;
  GMountSpec *mount_spec;

  GMutex mutex;
  GHashTable *devices;
};

struct _GVfsBackendMtpClass
{
  GVfsBackendClass parent_class;
};

GType g_vfs_backend_mtp_get_type (void) G_GNUC_CONST;

G_END_DECLS

#endif /* __G_VFS_BACKEND_MTP_H__ */
