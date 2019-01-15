#ifndef __WAYLAND_BACKEND_H__
#define __WAYLAND_BACKEND_H__

#include <config.h>

#ifndef HAVE_WAYLAND
#error file should only be included when HAVE_WAYLAND is enabled
#endif

#include <gdk/gdk.h>
#include <gdk/gdkwayland.h>
#include <gtk/gtk.h>

#include "panel-toplevel.h"

gboolean is_using_wayland (void);

// wayland_* functions must only be called when using Wayland
void wayland_init (void);
void wayland_panel_toplevel_realize (PanelToplevel *window);
void wayland_panel_toplevel_set_strut (PanelToplevel     *gdk_window,
				       PanelOrientation  orientation,
				       guint32           strut,
				       guint32           strut_start,
				       guint32           strut_end);

#endif /* __WAYLAND_BACKEND_H__ */
