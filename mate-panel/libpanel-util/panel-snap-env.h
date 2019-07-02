/*
 * panel-snap-env.h: Tools related to running inside a snap
 *
 * Copyright (C) 2019 William Wold
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 * Authors:
 *	William Wold <wm@wmww.sh>
 */

#ifndef PANEL_SNAP_ENV_H
#define PANEL_SNAP_ENV_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _GAppLaunchContext             GAppLaunchContext;

// Is called automatically when needed if not called before
void panel_snap_env_init (char *snap_out_binary_path);

void panel_snap_env_setup_launch_context_if_needed (GAppLaunchContext *context);

#ifdef __cplusplus
}
#endif

#endif // PANEL_SNAP_ENV_H
