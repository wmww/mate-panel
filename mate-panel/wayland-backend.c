#include <config.h>
#include <math.h>

#include "wayland-backend.h"

#include "panel-toplevel.h"

#include "wayland-protocols/wlr-layer-shell-unstable-v1-client.h"
#include "wayland-protocols/xdg-shell-client.h"

struct zwlr_layer_shell_v1 *layer_shell_global = NULL;
struct xdg_wm_base *xdg_wm_base_global = NULL;
static gboolean wayland_has_initialized = FALSE;

// the last widget to get the query-tooltip callback, becomes the parent of new tooltips
static GtkWidget *last_query_tooltip_widget = NULL;

// Gap between a widget and its tooltip
static const int tooltip_placement_spacing = 6;

// Both always attached to GDK windows (not GTK ones)
static const char *wayland_shell_surface_key = "wayland_shell_surface";

gboolean
is_using_wayland ()
{
	return GDK_IS_WAYLAND_DISPLAY (gdk_display_get_default ());
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
wl_registry_handle_global (void *data,
			   struct wl_registry *registry,
			   uint32_t id,
			   const char *interface,
			   uint32_t version)
{
	// pull out needed globals
	if (strcmp (interface, zwlr_layer_shell_v1_interface.name) == 0) {
		layer_shell_global = wl_registry_bind (registry, id, &zwlr_layer_shell_v1_interface, 1);
	} else if (strcmp (interface, xdg_wm_base_interface.name) == 0) {
		xdg_wm_base_global = wl_registry_bind (registry, id, &xdg_wm_base_interface, 1);
	}
}

static void
wl_registry_handle_global_remove (void *data,
				  struct wl_registry *registry,
				  uint32_t id)
{
	// who cares
}

static const struct wl_registry_listener wl_registry_listener = {
	.global = wl_registry_handle_global,
	.global_remove = wl_registry_handle_global_remove,
};

// Gets the upper left and size of the portion of the window that is actually used (not shadows and whatnot)
// It does this by walking down the gdk_window tree, as long as there is exactly one child
static void
wayland_widget_get_logical_geom (GtkWidget *widget, GdkRectangle *geom)
{
	GdkWindow *window;
	GList *list;

	window = gtk_widget_get_window (widget);
	list = gdk_window_get_children (window);
	if (list && !list->next) // If there is exactly one child window
		window = list->data;
	gdk_window_get_geometry (window, &geom->x, &geom->y, &geom->width, &geom->height);
}

struct _LayerSurfaceInfo {
	PanelOrientation orientation;
	int exclusive_zone;
};

struct _WaylandShellSurface {
	GtkWindow *gtk_window;
	GtkWidget *transient_for_widget;
	gint width, height;

	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *xdg_toplevel;
	struct xdg_popup *xdg_popup;
	gboolean is_tooltip;

	struct zwlr_layer_surface_v1 *layer_surface;
	struct _LayerSurfaceInfo *layer_surface_info;
};

typedef struct _WaylandShellSurface WaylandShellSurface;

static WaylandShellSurface *
wayland_widget_get_wayland_shell_surface (GtkWidget *widget)
{
	GdkWindow *gdk_window;

	g_return_val_if_fail (widget, NULL);
	gdk_window = gtk_widget_get_window (gtk_widget_get_toplevel (widget));
	if (!gdk_window)
		return NULL;
	return g_object_get_data (G_OBJECT (gdk_window), wayland_shell_surface_key);
}

// Idempotent; deletes the Wayland objects associated with a window
static void
wayland_shell_surface_unmap (WaylandShellSurface *self)
{
	// This function must be called before the wl_surface can be safely destroyed

	self->width = 0;
	self->height = 0;

	if (self->xdg_popup) {
		xdg_popup_destroy (self->xdg_popup);
		self->xdg_popup = NULL;
	}
	if (self->xdg_toplevel) {
		xdg_toplevel_destroy (self->xdg_toplevel);
		self->xdg_toplevel = NULL;
	}
	if (self->xdg_surface) {
		// Important that XDG surfaces are destroyed after their role (popup or toplevel)
		xdg_surface_destroy (self->xdg_surface);
		self->xdg_surface = NULL;
	}

	if (self->layer_surface) {
		zwlr_layer_surface_v1_destroy (self->layer_surface);
		self->layer_surface = NULL;
	}
	if (self->layer_surface_info) {
		g_free (self->layer_surface_info);
		self->layer_surface_info = NULL;
	}
}

// Should only be used in wayland_shell_surface_new as the destroy listener
// Other usages may cause a double free
static void
wayland_shell_surface_destroy_cb (WaylandShellSurface *self) {
	wayland_shell_surface_unmap (self);
	free (self);
}

static void
wayland_shell_surface_set_size (WaylandShellSurface *self, gint width, gint height)
{
	if (self->width != width || self->height != height) {
		self->width  = width;
		self->height = height;
		if (self->layer_surface)
			zwlr_layer_surface_v1_set_size (self->layer_surface, self->width, self->height);
		if (self->xdg_surface)
			xdg_surface_set_window_geometry (self->xdg_surface, 0, 0, self->width, self->height);
	}
}

static void
wayland_widget_size_allocate_cb (GtkWidget           *widget,
				 GdkRectangle        *allocation,
				 WaylandShellSurface *shell_surface)
{
	wayland_shell_surface_set_size (shell_surface,
					allocation->width,
					allocation->height);
}

static WaylandShellSurface *
wayland_shell_surface_new (GtkWindow *gtk_window)
{
	WaylandShellSurface *self;
	GdkWindow *gdk_window;

	g_return_val_if_fail (!wayland_widget_get_wayland_shell_surface (GTK_WIDGET (gtk_window)), NULL);

	g_return_val_if_fail (gtk_window, NULL);
	gdk_window = gtk_widget_get_window (gtk_widget_get_toplevel (GTK_WIDGET (gtk_window)));
	g_return_val_if_fail (gdk_window, NULL);

	self = g_new0 (WaylandShellSurface, 1);
	self->gtk_window = gtk_window;

	gdk_wayland_window_set_use_custom_surface (gdk_window);
	g_object_set_data_full (G_OBJECT (gdk_window),
				wayland_shell_surface_key,
				self,
				(GDestroyNotify) wayland_shell_surface_destroy_cb);
	g_signal_connect (gtk_window, "size-allocate", G_CALLBACK (wayland_widget_size_allocate_cb), self);

	return self;
}

static struct xdg_popup *
wayland_shell_surface_get_child_xdg_popup (WaylandShellSurface *self,
					   struct xdg_surface *popup_xdg_surface,
					   struct xdg_positioner *positioner)
{
	if (self->layer_surface) {
		struct xdg_popup *xdg_popup;
		xdg_popup = xdg_surface_get_popup (popup_xdg_surface, NULL, positioner);
		zwlr_layer_surface_v1_get_popup (self->layer_surface, xdg_popup);
		return xdg_popup;
	} else if (self->xdg_surface) {
		return xdg_surface_get_popup (popup_xdg_surface, self->xdg_surface, positioner);
	} else {
		g_warning ("Wayland shell surface %p has no layer or xdg shell surface wayland objects", self);
		return NULL;
	}
}

static struct xdg_positioner *
wayland_shell_surface_get_xdg_positioner (WaylandShellSurface *self,
					  enum xdg_positioner_anchor anchor,
					  enum xdg_positioner_gravity gravity,
					  GdkPoint offset)
{
	GdkRectangle popup_geom; // Rectangle on the wayland surface which makes up the "logical" window (cuts off boarders and shadows)
	struct xdg_positioner *positioner; // Wayland object we're building
	GdkPoint attach_widget_on_window; // Location of the transient for widget on its parent window
	GtkAllocation attach_widget_allocation; // Size of the transient for widget
	GdkWindow *popup_window;
	gint popup_width, popup_height; // Size of the Wayland surface
	double popup_anchor_x = 0, popup_anchor_y = 0; // From 0.0 to 1.0, relative to popup surface size, the point on the popup that will be attached
	GdkPoint positioner_offset; // The final calculated offset to be sent to the positioner

	g_return_val_if_fail (wayland_has_initialized, NULL);
	g_return_val_if_fail (xdg_wm_base_global, NULL);
	g_return_val_if_fail (self->transient_for_widget, NULL);

	gtk_widget_translate_coordinates (self->transient_for_widget, gtk_widget_get_toplevel (self->transient_for_widget),
					  0, 0, &attach_widget_on_window.x, &attach_widget_on_window.y);
	gtk_widget_get_allocated_size (self->transient_for_widget, &attach_widget_allocation, NULL);
	wayland_widget_get_logical_geom (GTK_WIDGET (self->gtk_window), &popup_geom);
	popup_window = gtk_widget_get_window (GTK_WIDGET (self->gtk_window));
	popup_width = gdk_window_get_width (popup_window);
	popup_height = gdk_window_get_height (popup_window);

	switch (gravity) {
	case XDG_POSITIONER_GRAVITY_LEFT:
	case XDG_POSITIONER_GRAVITY_BOTTOM_LEFT:
	case XDG_POSITIONER_GRAVITY_TOP_LEFT:
		popup_anchor_x = 1;
		break;

	case XDG_POSITIONER_GRAVITY_NONE:
	case XDG_POSITIONER_GRAVITY_BOTTOM:
	case XDG_POSITIONER_GRAVITY_TOP:
		popup_anchor_x = 0.5;
		break;

	case XDG_POSITIONER_GRAVITY_RIGHT:
	case XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT:
	case XDG_POSITIONER_GRAVITY_TOP_RIGHT:
		popup_anchor_x = 0;
		break;
	}
	switch (gravity) {
	case XDG_POSITIONER_GRAVITY_TOP:
	case XDG_POSITIONER_GRAVITY_TOP_LEFT:
	case XDG_POSITIONER_GRAVITY_TOP_RIGHT:
		popup_anchor_y = 1;
		break;

	case XDG_POSITIONER_GRAVITY_NONE:
	case XDG_POSITIONER_GRAVITY_LEFT:
	case XDG_POSITIONER_GRAVITY_RIGHT:
		popup_anchor_y = 0.5;
		break;

	case XDG_POSITIONER_GRAVITY_BOTTOM:
	case XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT:
	case XDG_POSITIONER_GRAVITY_BOTTOM_LEFT:
		popup_anchor_y = 0;
		break;
	}
	positioner_offset.x = (popup_width  - popup_geom.width)  * popup_anchor_x - popup_geom.x + offset.x;
	positioner_offset.y = (popup_height - popup_geom.height) * popup_anchor_y - popup_geom.y + offset.y;

	positioner = xdg_wm_base_create_positioner (xdg_wm_base_global);

	xdg_positioner_set_anchor_rect (positioner,
					MAX (attach_widget_on_window.x, 0), MAX (attach_widget_on_window.y, 0),
					MAX (attach_widget_allocation.width, 1), MAX (attach_widget_allocation.height, 1));
	xdg_positioner_set_offset (positioner, positioner_offset.x, positioner_offset.y);
	xdg_positioner_set_anchor (positioner, anchor);
	xdg_positioner_set_gravity (positioner, gravity);
	xdg_positioner_set_constraint_adjustment (positioner,
						  XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X
						  | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y);

	return positioner;
}

static WaylandShellSurface *
wayland_shell_surface_get_parent (WaylandShellSurface *self)
{
	if (self->transient_for_widget) {
		return wayland_widget_get_wayland_shell_surface (self->transient_for_widget);
	} else {
		return NULL;
	}
}

static PanelToplevel *
wayland_shell_surface_get_panel_toplevel (WaylandShellSurface *shell_surface)
{
	while (shell_surface) {
		if (PANEL_IS_TOPLEVEL (shell_surface->gtk_window)) {
			return PANEL_TOPLEVEL (shell_surface->gtk_window);
		} else {
			shell_surface = wayland_shell_surface_get_parent (shell_surface);
		}
	}

	g_warning ("Wayland shell surface does not have a PanelToplevel");
	return NULL;
}

static void
layer_surface_handle_configure (void *wayland_shell_surface,
				struct zwlr_layer_surface_v1 *surface,
				uint32_t serial,
				uint32_t w,
				uint32_t h)
{
	// WaylandShellSurface *self = wayland_shell_surface;
	// TODO: resize the GTK window
	// gtk_window_set_default_size (GTK_WINDOW (window), width, height);
	zwlr_layer_surface_v1_ack_configure (surface, serial);
}

static void
layer_surface_handle_closed (void *wayland_shell_surface,
			    struct zwlr_layer_surface_v1 *surface)
{
	// WaylandShellSurface *self = wayland_shell_surface;
	// TODO: close the GTK window and destroy the layer shell surface object
}

struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_handle_configure,
	.closed = layer_surface_handle_closed,
};

static void
xdg_surface_handle_configure (void *wayland_shell_surface,
			      struct xdg_surface *xdg_surface,
			      uint32_t serial)

{
	// WaylandShellSurface *self = wayland_shell_surface;
	xdg_surface_ack_configure (xdg_surface, serial);
}

struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_handle_configure,
};


static void
xdg_popup_handle_configure (void *wayland_shell_surface,
			    struct xdg_popup *xdg_popup,
			    int32_t x,
			    int32_t y,
			    int32_t width,
			    int32_t height)
{
	WaylandShellSurface *self = wayland_shell_surface;
	gtk_window_resize (self->gtk_window, width, height);
}

static void
xdg_popup_handle_popup_done (void *wayland_shell_surface,
			     struct xdg_popup *xdg_popup)
{
	WaylandShellSurface *self = wayland_shell_surface;
	gtk_widget_unmap (GTK_WIDGET (self->gtk_window));
}

static const struct xdg_popup_listener xdg_popup_listener = {
	.configure = xdg_popup_handle_configure,
	.popup_done = xdg_popup_handle_popup_done,
};

static void
wayland_shell_surface_map_as_popup (WaylandShellSurface *self,
				    struct xdg_positioner *positioner)
{
	GtkRequisition popup_size;
	GdkWindow *popup_window;
	struct wl_surface *popup_wl_surface;
	WaylandShellSurface *transient_for_wayland_shell_surface;

	wayland_shell_surface_unmap (self);

	g_return_if_fail (self->transient_for_widget);

	gtk_widget_get_preferred_size (GTK_WIDGET (self->gtk_window), NULL, &popup_size);
	xdg_positioner_set_size (positioner, popup_size.width, popup_size.height);

	popup_window = gtk_widget_get_window (GTK_WIDGET (self->gtk_window));
	g_return_if_fail (popup_window);

	popup_wl_surface = gdk_wayland_window_get_wl_surface (popup_window);
	g_return_if_fail (popup_wl_surface);
	self->xdg_surface = xdg_wm_base_get_xdg_surface (xdg_wm_base_global, popup_wl_surface);
	xdg_surface_add_listener (self->xdg_surface, &xdg_surface_listener, self);

	transient_for_wayland_shell_surface = wayland_widget_get_wayland_shell_surface (self->transient_for_widget);
	g_return_if_fail (transient_for_wayland_shell_surface);
	self->xdg_popup = wayland_shell_surface_get_child_xdg_popup (transient_for_wayland_shell_surface,
								     self->xdg_surface,
								     positioner);
	g_return_if_fail (self->xdg_popup);
	xdg_popup_add_listener (self->xdg_popup, &xdg_popup_listener, self);

	wayland_shell_surface_set_size (self,
					gtk_widget_get_allocated_width (GTK_WIDGET (self->gtk_window)),
					gtk_widget_get_allocated_height (GTK_WIDGET (self->gtk_window)));

	wl_surface_commit (popup_wl_surface);
	wl_display_roundtrip (gdk_wayland_display_get_wl_display (gdk_window_get_display (popup_window)));
}

void
wayland_panel_toplevel_realize (PanelToplevel *toplevel)
{
	GtkWidget *widget;
	GdkDisplay *gdk_display;
	GdkWindow *gdk_window;
	WaylandShellSurface *shell_surface;
	struct wl_surface *wl_surface;

	g_assert (wayland_has_initialized);

	widget = GTK_WIDGET (toplevel);
	gdk_display = gdk_window_get_display (gtk_widget_get_window (widget));

	g_assert (GDK_IS_WAYLAND_DISPLAY (gdk_display));
	g_assert (wayland_has_initialized);

	gdk_window = gtk_widget_get_window (widget);
	g_return_if_fail (gdk_window);

	shell_surface = wayland_shell_surface_new (GTK_WINDOW (toplevel));
	g_return_if_fail (shell_surface);

	wl_surface = gdk_wayland_window_get_wl_surface (gdk_window);
	g_return_if_fail (wl_surface);

	if (layer_shell_global) {
		uint32_t layer = ZWLR_LAYER_SHELL_V1_LAYER_TOP;
		char *namespace = "mate"; // not sure what this is for

		shell_surface->layer_surface = zwlr_layer_shell_v1_get_layer_surface (layer_shell_global,
										      wl_surface,
										      NULL,
										      layer,
										      namespace);
		g_return_if_fail (shell_surface->layer_surface);
		zwlr_layer_surface_v1_set_keyboard_interactivity (shell_surface->layer_surface, FALSE);
		zwlr_layer_surface_v1_add_listener (shell_surface->layer_surface, &layer_surface_listener, shell_surface);
	} else if (xdg_wm_base_global) {
		g_warning ("Layer Shell Wayland protocol not supported, panel will not be placed correctly");
		shell_surface->xdg_surface = xdg_wm_base_get_xdg_surface (xdg_wm_base_global, wl_surface);
		shell_surface->xdg_toplevel = xdg_surface_get_toplevel (shell_surface->xdg_surface);
		xdg_surface_add_listener (shell_surface->xdg_surface, &xdg_surface_listener, shell_surface);
	} else {
		g_warning ("Neither Layer Shell or XDG shell stable Wayland protocols detected, panel can not be drawn");
		return;
	}
	wayland_shell_surface_set_size (shell_surface,
					gtk_widget_get_allocated_width (widget),
					gtk_widget_get_allocated_height (widget));
	wl_surface_commit (wl_surface);
	wl_display_roundtrip (gdk_wayland_display_get_wl_display (gdk_display));
}

void
wayland_panel_toplevel_set_strut (PanelToplevel    *toplevel,
				  PanelOrientation  orientation,
				  guint32           strut_size,
				  guint32           strut_start,
				  guint32           strut_end)
{
	GdkWindow *gdk_window;
	WaylandShellSurface *shell_surface;
	struct _LayerSurfaceInfo *info;
	gboolean initial_setup = FALSE;
	gboolean needs_commit = FALSE;

	gdk_window = gtk_widget_get_window (GTK_WIDGET (toplevel));
	g_return_if_fail (gdk_window);
	shell_surface = g_object_get_data (G_OBJECT (gdk_window), wayland_shell_surface_key);
	g_return_if_fail (shell_surface);
	if (!shell_surface->layer_surface)
		return;

	if (!shell_surface->layer_surface_info) {
		shell_surface->layer_surface_info = g_new0 (struct _LayerSurfaceInfo, 1);
		initial_setup = TRUE;
	}

	info = shell_surface->layer_surface_info;

	if (initial_setup || info->orientation != orientation) {
		uint32_t anchor;
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
			anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
		}
		zwlr_layer_surface_v1_set_anchor (shell_surface->layer_surface, anchor);
		info->orientation = orientation;
		needs_commit = TRUE;
	}

	if (initial_setup || info->exclusive_zone != strut_size) {
		zwlr_layer_surface_v1_set_exclusive_zone (shell_surface->layer_surface, strut_size);
		info->exclusive_zone = strut_size;
		needs_commit = TRUE;
	}

	if (needs_commit) {
		wl_surface_commit (gdk_wayland_window_get_wl_surface (gdk_window));
	}
}

static gboolean
wayland_popup_map_event_cb (GtkWidget *popup_widget, GdkEvent *event, WaylandShellSurface *shell_surface)
{
	WaylandShellSurface *parent_shell_surface;
	PanelToplevel *toplevel;
	PanelOrientation toplevel_orientation;
	enum xdg_positioner_anchor anchor = XDG_POSITIONER_ANCHOR_TOP_LEFT;
	enum xdg_positioner_gravity gravity = XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT;
	GdkPoint offset = {0, 0};
	struct xdg_positioner *positioner;

	g_return_val_if_fail (shell_surface, FALSE);
	g_warn_if_fail (popup_widget == GTK_WIDGET (shell_surface->gtk_window));

	if (shell_surface->is_tooltip) {
		g_warn_if_fail (last_query_tooltip_widget);
		shell_surface->transient_for_widget = last_query_tooltip_widget;
	}

	parent_shell_surface = wayland_shell_surface_get_parent (shell_surface);
	toplevel = wayland_shell_surface_get_panel_toplevel (shell_surface);
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

	positioner = wayland_shell_surface_get_xdg_positioner (shell_surface, anchor, gravity, offset);
	wayland_shell_surface_map_as_popup (shell_surface, positioner);
	xdg_positioner_destroy (positioner);

	return TRUE;
}

// This callback only does anything for popups of Wayland surfaces
static void
wayland_window_realize_override_cb (GtkWindow *gtk_window, void *_data)
{
	GtkWidget *window_widget, *transient_for_widget;
	gboolean is_tooltip = FALSE;

	// Call the default realize handler
	GValue args[1] = { G_VALUE_INIT };
	g_value_init_from_instance (&args[0], gtk_window);
	g_signal_chain_from_overridden (args, NULL);
	g_value_unset (&args[0]);

	window_widget = GTK_WIDGET (gtk_window);

	if (g_strcmp0 (gtk_widget_get_name (GTK_WIDGET (gtk_window)), "GtkTooltipWindow") == 0) {
		is_tooltip = TRUE;
	}

	transient_for_widget = gtk_window_get_attached_to (gtk_window);
	if (!transient_for_widget)
		transient_for_widget = GTK_WIDGET (gtk_window_get_transient_for (gtk_window));

	if (transient_for_widget && wayland_widget_get_wayland_shell_surface (transient_for_widget)) {
		WaylandShellSurface *shell_surface;

		shell_surface = wayland_widget_get_wayland_shell_surface (window_widget);

		if (shell_surface) {
			wayland_shell_surface_unmap (shell_surface);
		} else {
			shell_surface = wayland_shell_surface_new (gtk_window);
			g_signal_connect (gtk_window, "map-event", G_CALLBACK (wayland_popup_map_event_cb), shell_surface);
			// unmap needs to be handled by a type-level override
		}

		shell_surface->is_tooltip = is_tooltip;
		shell_surface->transient_for_widget = transient_for_widget;
	}
}

// This callback must override the default unmap handler, so it can run first
// wayland_popup_data_unmap () must be called before GtkWidget's unmap, or Wayland objects are destroyed in the wrong order
static void
wayland_window_unmap_override_cb (GtkWindow *gtk_window, void *_data)
{
	WaylandShellSurface *shell_surface;

	shell_surface = wayland_widget_get_wayland_shell_surface (GTK_WIDGET (gtk_window));
	if (shell_surface)
		wayland_shell_surface_unmap (shell_surface);

	// Call the default unmap handler
	GValue args[1] = { G_VALUE_INIT };
	g_value_init_from_instance (&args[0], gtk_window);
	g_signal_chain_from_overridden (args, NULL);
	g_value_unset (&args[0]);
}

static gboolean
wayland_query_tooltip_emission_hook (GSignalInvocationHint *_ihint,
				     guint n_param_values,
				     const GValue *param_values,
				     gpointer _data)
{
	last_query_tooltip_widget = GTK_WIDGET (g_value_peek_pointer(&param_values[0]));
	return TRUE; // Always stay connected
}

void wayland_init() {
	GdkDisplay *gdk_display;
	gint realize_signal_id, unmap_signal_id, query_tooltip_signal_id;
	GClosure *realize_closure, *unmap_closure;

	g_assert_false (wayland_has_initialized);

	gdk_display = gdk_display_get_default ();
	g_assert (GDK_IS_WAYLAND_DISPLAY (gdk_display));

	struct wl_display *wl_display = gdk_wayland_display_get_wl_display (gdk_display);
	struct wl_registry *wl_registry = wl_display_get_registry (wl_display);
	wl_registry_add_listener (wl_registry, &wl_registry_listener, NULL);
	wl_display_roundtrip (wl_display);

	if (!layer_shell_global)
		g_warning ("It appears your Wayland compositor does not support the Layer Shell protocol");

	realize_signal_id = g_signal_lookup ("realize", GTK_TYPE_WINDOW);
	realize_closure = g_cclosure_new (G_CALLBACK (wayland_window_realize_override_cb), NULL, NULL);
	g_signal_override_class_closure (realize_signal_id, GTK_TYPE_WINDOW, realize_closure);

	unmap_signal_id = g_signal_lookup ("unmap", GTK_TYPE_WINDOW);
	unmap_closure = g_cclosure_new (G_CALLBACK (wayland_window_unmap_override_cb), NULL, NULL);
	g_signal_override_class_closure (unmap_signal_id, GTK_TYPE_WINDOW, unmap_closure);

	query_tooltip_signal_id = g_signal_lookup ("query-tooltip", GTK_TYPE_WIDGET);
	g_signal_add_emission_hook (query_tooltip_signal_id, 0, wayland_query_tooltip_emission_hook, NULL, NULL);

	wayland_has_initialized = TRUE;
}
