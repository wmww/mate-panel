#include <config.h>

#include "wayland-backend.h"

#include "panel-toplevel.h"
#include "wayland-layer-shell-gtk.h"

// Gap between a widget and its tooltip
static const int tooltip_placement_spacing = 6;

void
wayland_panel_toplevel_realize (PanelToplevel *toplevel)
{
	wayland_shell_surface_new_layer_surface (GTK_WINDOW (toplevel), NULL, ZWLR_LAYER_SHELL_V1_LAYER_TOP, "mate");
}

void
wayland_panel_toplevel_set_strut (PanelToplevel    *toplevel,
				  PanelOrientation  orientation,
				  guint32           strut_size,
				  guint32           strut_start,
				  guint32           strut_end)
{
	WaylandShellSurface *shell_surface;
	uint32_t anchor = 0;

	shell_surface = gtk_widget_get_wayland_shell_surface (GTK_WIDGET (toplevel));

	g_return_if_fail (shell_surface);
	if (!shell_surface->layer_surface)
		return;

	switch (orientation) {
	case PANEL_ORIENTATION_LEFT:
		anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
		break;
	case PANEL_ORIENTATION_RIGHT:
		anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
		break;
	case PANEL_ORIENTATION_TOP:
		anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
		break;
	case PANEL_ORIENTATION_BOTTOM:
		anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
		break;
	default:
		g_warning ("Invalid panel orientation %d", orientation);
	}

	wayland_shell_surface_set_layer_surface_info (shell_surface, anchor, strut_size);
}

static void
widget_get_pointer_position (GtkWidget *widget, gint *pointer_x, gint *pointer_y)
{
	GdkWindow *window;
	GdkDisplay *display;
	GdkSeat *seat;
	GdkDevice *pointer;

	window = gdk_window_get_toplevel (gtk_widget_get_window (widget));
	display = gdk_window_get_display (window);
	seat = gdk_display_get_default_seat (display);
	pointer = gdk_seat_get_pointer (seat);
	gdk_window_get_device_position (window, pointer, pointer_x, pointer_y, NULL);
}

static void
wayland_popup_map_callback (WaylandShellSurface *shell_surface)
{
	WaylandShellSurface *parent_shell_surface, *toplevel_shell_surface;
	PanelToplevel *toplevel;
	PanelOrientation toplevel_orientation;
	enum xdg_positioner_anchor anchor = XDG_POSITIONER_ANCHOR_TOP_LEFT;
	enum xdg_positioner_gravity gravity = XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT;
	GdkPoint offset = {0, 0};

	g_return_if_fail (shell_surface);

	parent_shell_surface = wayland_shell_surface_get_parent (shell_surface);
	toplevel_shell_surface = wayland_shell_surface_get_toplevel (shell_surface);
	if (toplevel_shell_surface) {
		toplevel = PANEL_TOPLEVEL (toplevel_shell_surface->gtk_window);
	} else {
		toplevel = NULL;
	}
	if (toplevel) {
		toplevel_orientation = panel_toplevel_get_orientation (toplevel);
	} else {
		toplevel_orientation = PANEL_ORIENTATION_BOTTOM;
	}

	if (shell_surface->is_tooltip) {
		switch (toplevel_orientation) {
		case PANEL_ORIENTATION_TOP:
			anchor = XDG_POSITIONER_ANCHOR_BOTTOM;
			gravity = XDG_POSITIONER_GRAVITY_BOTTOM;
			offset.y += tooltip_placement_spacing;
			break;
		case PANEL_ORIENTATION_RIGHT:
			anchor = XDG_POSITIONER_ANCHOR_LEFT;
			gravity = XDG_POSITIONER_GRAVITY_LEFT;
			offset.x -= tooltip_placement_spacing;
			break;
		case PANEL_ORIENTATION_BOTTOM:
			anchor = XDG_POSITIONER_ANCHOR_TOP;
			gravity = XDG_POSITIONER_GRAVITY_TOP;
			offset.y -= tooltip_placement_spacing;
			break;
		case PANEL_ORIENTATION_LEFT:
			anchor = XDG_POSITIONER_ANCHOR_RIGHT;
			gravity = XDG_POSITIONER_GRAVITY_RIGHT;
			offset.x += tooltip_placement_spacing;
			break;
		}
	}
	else if (parent_shell_surface && parent_shell_surface->transient_for_widget) {
		// We assume popups on popups behave like nested menus
		anchor = XDG_POSITIONER_ANCHOR_TOP_RIGHT;
		gravity = XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT;
	} else {
		switch (toplevel_orientation) {
		case PANEL_ORIENTATION_TOP:
			anchor = XDG_POSITIONER_ANCHOR_BOTTOM_LEFT;
			gravity = XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT;
			break;
		case PANEL_ORIENTATION_RIGHT:
			anchor = XDG_POSITIONER_ANCHOR_TOP_LEFT;
			gravity = XDG_POSITIONER_GRAVITY_BOTTOM_LEFT;
			break;
		case PANEL_ORIENTATION_BOTTOM:
			anchor = XDG_POSITIONER_ANCHOR_TOP_LEFT;
			gravity = XDG_POSITIONER_GRAVITY_TOP_RIGHT;
			break;
		case PANEL_ORIENTATION_LEFT:
			anchor = XDG_POSITIONER_ANCHOR_TOP_RIGHT;
			gravity = XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT;
			break;
		}
		if (shell_surface->transient_for_widget == GTK_WIDGET (toplevel)) {
			anchor = XDG_POSITIONER_ANCHOR_TOP_LEFT;
			widget_get_pointer_position (shell_surface->transient_for_widget, &offset.x, &offset.y);
		}
	}

	wayland_shell_surface_map_popup (shell_surface, anchor, gravity, offset);
}

void
wayland_init ()
{
	wayland_shell_surface_global_init (wayland_popup_map_callback);
}


