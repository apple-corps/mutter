/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/*
 * Copyright (C) 2015 Red Hat
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
 * Written by:
 *     Jonas Ådahl <jadahl@gmail.com>
 */

#ifndef META_POINTER_LOCK_WAYLAND_H
#define META_POINTER_LOCK_WAYLAND_H

#include <glib-object.h>

#include "wayland/meta-pointer-confinement-wayland.h"

G_BEGIN_DECLS

#define META_TYPE_POINTER_LOCK_WAYLAND (meta_pointer_lock_wayland_get_type ())
G_DECLARE_FINAL_TYPE (MetaPointerLockWayland, meta_pointer_lock_wayland,
                      META, POINTER_LOCK_WAYLAND, MetaPointerConfinementWayland)

MetaPointerConfinementWayland *meta_pointer_lock_wayland_new (MetaWaylandPointerConstraint *constraint);

G_END_DECLS

#endif /* META_LOCK_WAYLAND_H */
