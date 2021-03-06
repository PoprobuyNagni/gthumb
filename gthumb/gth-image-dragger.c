/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 *  GThumb
 *
 *  Copyright (C) 2009 Free Software Foundation, Inc.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>
#include <math.h>
#include "cairo-scale.h"
#include "cairo-utils.h"
#include "glib-utils.h"
#include "gth-image-dragger.h"
#include "pixbuf-utils.h"


#define SIZE_TOO_BIG_FOR_SCALE_BILINEAR (3000 * 3000)


struct _GthImageDraggerPrivate {
	GthImageViewer  *viewer;
	gboolean         draggable;
	cairo_surface_t *scaled;
	GthTask         *scale_task;
};


static void gth_image_dragger_gth_image_tool_interface_init (GthImageViewerToolInterface *iface);


G_DEFINE_TYPE_WITH_CODE (GthImageDragger,
			 gth_image_dragger,
			 G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (GTH_TYPE_IMAGE_VIEWER_TOOL,
					        gth_image_dragger_gth_image_tool_interface_init))


static void
gth_image_dragger_finalize (GObject *object)
{
	GthImageDragger *self;

	g_return_if_fail (object != NULL);
	g_return_if_fail (GTH_IS_IMAGE_DRAGGER (object));

	self = GTH_IMAGE_DRAGGER (object);
	_cairo_clear_surface (&self->priv->scaled);
	if (self->priv->scale_task != NULL)
		gth_task_cancel (self->priv->scale_task);

	/* Chain up */
	G_OBJECT_CLASS (gth_image_dragger_parent_class)->finalize (object);
}


static void
gth_image_dragger_class_init (GthImageDraggerClass *class)
{
	GObjectClass *gobject_class;

	g_type_class_add_private (class, sizeof (GthImageDraggerPrivate));

	gobject_class = (GObjectClass*) class;
	gobject_class->finalize = gth_image_dragger_finalize;
}


static void
gth_image_dragger_init (GthImageDragger *dragger)
{
	dragger->priv = G_TYPE_INSTANCE_GET_PRIVATE (dragger, GTH_TYPE_IMAGE_DRAGGER, GthImageDraggerPrivate);
	dragger->priv->scaled = NULL;
	dragger->priv->scale_task = NULL;
	dragger->priv->viewer = NULL;
	dragger->priv->draggable = FALSE;
}


static void
gth_image_dragger_set_viewer (GthImageViewerTool *base,
			      GthImageViewer     *image_viewer)
{
	GthImageDragger *self = GTH_IMAGE_DRAGGER (base);
	self->priv->viewer = image_viewer;
}


static void
gth_image_dragger_unset_viewer (GthImageViewerTool *base,
		       	        GthImageViewer     *image_viewer)
{
	GthImageDragger *self = GTH_IMAGE_DRAGGER (base);
	self->priv->viewer = NULL;
}


static void
gth_image_dragger_realize (GthImageViewerTool *base)
{
	/* void */
}


static void
gth_image_dragger_unrealize (GthImageViewerTool *base)
{
	/* void */
}


static void
_gth_image_dragger_update_cursor (GthImageDragger *self)
{
	GdkCursor *cursor;

	cursor = gdk_cursor_new (GDK_LEFT_PTR);
	gth_image_viewer_set_cursor (self->priv->viewer, cursor);

	g_object_unref (cursor);
}


static void
gth_image_dragger_size_allocate (GthImageViewerTool *base,
				 GtkAllocation      *allocation)
{
	GthImageDragger *self;
	GthImageViewer  *viewer;
	double           h_page_size;
	double           v_page_size;
	double           h_upper;
	double           v_upper;

	self = (GthImageDragger *) base;
	viewer = (GthImageViewer *) self->priv->viewer;

	h_page_size = gtk_adjustment_get_page_size (viewer->hadj);
	v_page_size = gtk_adjustment_get_page_size (viewer->vadj);
	h_upper = gtk_adjustment_get_upper (viewer->hadj);
	v_upper = gtk_adjustment_get_upper (viewer->vadj);

	self->priv->draggable = (h_page_size > 0) && (v_page_size > 0) && ((h_upper > h_page_size) || (v_upper > v_page_size));
	if (gtk_widget_get_realized (GTK_WIDGET (viewer)))
		_gth_image_dragger_update_cursor (self);
}


static void
gth_image_dragger_map (GthImageViewerTool *base)
{
	/* void */
}


static void
gth_image_dragger_unmap (GthImageViewerTool *base)
{
	/* void */
}


static void
gth_image_dragger_draw (GthImageViewerTool *self,
		        cairo_t            *cr)
{
	GthImageDragger *dragger;
	GthImageViewer  *viewer;

	dragger = (GthImageDragger *) self;
	viewer = dragger->priv->viewer;

	gth_image_viewer_paint_background (viewer, cr);

	if (gth_image_viewer_get_current_image (viewer) == NULL)
		return;

	if (dragger->priv->scaled != NULL)
		gth_image_viewer_paint_region (viewer,
					       cr,
					       dragger->priv->scaled,
					       viewer->x_offset - viewer->image_area.x,
					       viewer->y_offset - viewer->image_area.y,
					       &viewer->image_area,
					       NULL,
					       CAIRO_FILTER_FAST);
	else
		gth_image_viewer_paint_region (viewer,
					       cr,
					       gth_image_viewer_get_current_image (viewer),
					       viewer->x_offset - viewer->image_area.x,
					       viewer->y_offset - viewer->image_area.y,
					       &viewer->image_area,
					       NULL,
					       gth_image_viewer_get_zoom_quality_filter (viewer));

	gth_image_viewer_apply_painters (viewer, cr);
}


static gboolean
gth_image_dragger_button_press (GthImageViewerTool *self,
				GdkEventButton     *event)
{
	GthImageDragger *dragger;
	GthImageViewer  *viewer;
	GtkWidget       *widget;

	dragger = (GthImageDragger *) self;
	viewer = dragger->priv->viewer;
	widget = GTK_WIDGET (viewer);

	if (! dragger->priv->draggable)
		return FALSE;

	if (((event->button == 1) || (event->button == 2)) &&
			! viewer->dragging) {
		GdkCursor     *cursor;
		GdkGrabStatus  retval;

		cursor = gdk_cursor_new_from_name (gtk_widget_get_display (widget), "grabbing");
		retval = gdk_device_grab (event->device,
					  gtk_widget_get_window (widget),
					  GDK_OWNERSHIP_WINDOW,
					  FALSE,
					  (GDK_POINTER_MOTION_MASK
					   | GDK_POINTER_MOTION_HINT_MASK
					   | GDK_BUTTON_RELEASE_MASK),
					  cursor,
					  event->time);

		if (cursor != NULL)
			g_object_unref (cursor);

		if (retval != GDK_GRAB_SUCCESS)
			return FALSE;

		viewer->pressed = TRUE;
		viewer->dragging = TRUE;

		return TRUE;
	}

	return FALSE;
}


static gboolean
gth_image_dragger_button_release (GthImageViewerTool *self,
				  GdkEventButton     *event)
{
	GthImageDragger *dragger;
	GthImageViewer  *viewer;

	if ((event->button != 1) && (event->button != 2))
		return FALSE;

	dragger = (GthImageDragger *) self;
	viewer = dragger->priv->viewer;

	if (viewer->dragging)
		gdk_device_ungrab (event->device, event->time);

	return TRUE;
}


static gboolean
gth_image_dragger_motion_notify (GthImageViewerTool *self,
				 GdkEventMotion     *event)
{
	GthImageDragger *dragger;
	GthImageViewer  *viewer;

	dragger = (GthImageDragger *) self;
	viewer = dragger->priv->viewer;

	if (! viewer->pressed)
		return FALSE;

	viewer->dragging = TRUE;

	if (! event->is_hint)
		return FALSE;

	gth_image_viewer_scroll_to (viewer,
				    viewer->drag_x_start - event->x,
				    viewer->drag_y_start - event->y);

	return TRUE;
}


/* -- _gth_image_dragger_scale -- */


typedef struct {
	GthImageDragger *dragger;
	cairo_surface_t *image;
	int              new_width;
	int              new_height;
	cairo_surface_t *scaled;
	scale_filter_t   filter;
} ScaleData;


static void
scale_data_free (ScaleData *scale_data)
{
	if (scale_data == NULL)
		return;
	cairo_surface_destroy (scale_data->image);
	cairo_surface_destroy (scale_data->scaled);
	g_free (scale_data);
}


static gpointer
_gth_image_dragger_scale_exec (GthAsyncTask *task,
			       gpointer      user_data)
{
	ScaleData *scale_data = user_data;

	scale_data->scaled = _cairo_image_surface_scale (scale_data->image,
							 scale_data->new_width,
							 scale_data->new_height,
							 scale_data->filter,
							 task);

	return NULL;
}


static void
_gth_image_dragger_scale_after (GthAsyncTask *task,
				GError       *error,
				gpointer      user_data)
{
	ScaleData *scale_data = user_data;

	if (error == NULL) {
		GthImageDragger *dragger = scale_data->dragger;

		_cairo_clear_surface (&dragger->priv->scaled);
		dragger->priv->scaled = cairo_surface_reference (scale_data->scaled);
		if (dragger->priv->viewer != NULL)
			gtk_widget_queue_draw (GTK_WIDGET (dragger->priv->viewer));

		if (GTH_TASK (task) == dragger->priv->scale_task)
			dragger->priv->scale_task = NULL;
	}

	g_object_unref (task);
}


static void
_gth_image_dragger_create_scaled_high_quality (GthImageDragger *self,
					       cairo_surface_t *image,
					       int              new_width,
					       int              new_height,
					       scale_filter_t   filter)
{
	ScaleData *scale_data;

	if (self->priv->scale_task != NULL)
		gth_task_cancel (self->priv->scale_task);

	scale_data = g_new0 (ScaleData, 1);
	scale_data->dragger = self;
	scale_data->image = cairo_surface_reference (image);
	scale_data->new_width = new_width;
	scale_data->new_height = new_height;
	scale_data->scaled = NULL;
	scale_data->filter = filter;

	self->priv->scale_task = gth_async_task_new (NULL,
						     _gth_image_dragger_scale_exec,
						     _gth_image_dragger_scale_after,
						     scale_data,
						     (GDestroyNotify) scale_data_free);

	gth_task_exec (self->priv->scale_task, NULL);
}


static void
_gth_image_dragger_update_scaled_image (GthImageDragger *self)
{
	cairo_surface_t *image;
	int              image_width, image_height;
	int              original_width, original_height;
	double           zoom;
	int              new_width, new_height;
	scale_filter_t   filter;

	if (self->priv->scale_task != NULL) {
		gth_task_cancel (self->priv->scale_task);
		self->priv->scale_task = NULL;
	}

	_cairo_clear_surface (&self->priv->scaled);

	if (self->priv->viewer == NULL)
		return;

	if (gth_image_viewer_is_animation (self->priv->viewer))
		return;

	image = gth_image_viewer_get_current_image (self->priv->viewer);
	if ((image == NULL) || (cairo_surface_status (image) != CAIRO_STATUS_SUCCESS))
		return;

	image_width = cairo_image_surface_get_width (image);
	image_height = cairo_image_surface_get_height (image);
	gth_image_viewer_get_original_size (self->priv->viewer, &original_width, &original_height);

	if ((original_width != image_width) || (original_height != image_height))
		return;

	if (gth_image_viewer_get_zoom_quality (self->priv->viewer) == GTH_ZOOM_QUALITY_LOW)
		return;

	zoom = gth_image_viewer_get_zoom (self->priv->viewer);
	if (zoom >= 1.0)
		return;

	new_width = zoom * image_width;
	new_height = zoom * image_height;

	if (image_width * image_height > SIZE_TOO_BIG_FOR_SCALE_BILINEAR) {
		self->priv->scaled = _cairo_image_surface_scale_nearest (image, new_width, new_height);
		filter = SCALE_FILTER_BOX;
	}
	else {
		self->priv->scaled = _cairo_image_surface_scale_bilinear (image, new_width, new_height);
		filter = SCALE_FILTER_TRIANGLE;
	}

	_gth_image_dragger_create_scaled_high_quality (self, image, new_width, new_height, filter);
}


static void
gth_image_dragger_image_changed (GthImageViewerTool *self)
{
	_gth_image_dragger_update_scaled_image (GTH_IMAGE_DRAGGER (self));
}


static void
gth_image_dragger_zoom_changed (GthImageViewerTool *self)
{
	_gth_image_dragger_update_scaled_image (GTH_IMAGE_DRAGGER (self));
}


static void
gth_image_dragger_gth_image_tool_interface_init (GthImageViewerToolInterface *iface)
{
	iface->set_viewer = gth_image_dragger_set_viewer;
	iface->unset_viewer = gth_image_dragger_unset_viewer;
	iface->realize = gth_image_dragger_realize;
	iface->unrealize = gth_image_dragger_unrealize;
	iface->size_allocate = gth_image_dragger_size_allocate;
	iface->map = gth_image_dragger_map;
	iface->unmap = gth_image_dragger_unmap;
	iface->draw = gth_image_dragger_draw;
	iface->button_press = gth_image_dragger_button_press;
	iface->button_release = gth_image_dragger_button_release;
	iface->motion_notify = gth_image_dragger_motion_notify;
	iface->image_changed = gth_image_dragger_image_changed;
	iface->zoom_changed = gth_image_dragger_zoom_changed;
}


GthImageViewerTool *
gth_image_dragger_new (void)
{
	return (GthImageViewerTool *) g_object_new (GTH_TYPE_IMAGE_DRAGGER, NULL);
}
