/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 *  GThumb
 *
 *  Copyright (C) 2001 The Free Software Foundation, Inc.
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
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Street #330, Boston, MA 02111-1307, USA.
 */

#include <config.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <glade/glade.h>

#include "dlg-save-image.h"
#include "glib-utils.h"
#include "file-data.h"
#include "file-utils.h"
#include "gfile-utils.h"
#include "gtk-utils.h"
#include "gconf-utils.h"
#include "pixbuf-utils.h"
#include "preferences.h"


#define IMAGE_TYPE_AUTOMATIC 0

typedef enum {
	IMAGE_TYPE_JPEG = 0,
	IMAGE_TYPE_PNG,
	IMAGE_TYPE_TGA,
	IMAGE_TYPE_TIFF
} ImageType;

typedef struct {
        const char     *name;
        ImageType       type;
        const char     *mime_type;
} FileOption;

static FileOption file_options[] = {
        { N_("Determine by extension"), IMAGE_TYPE_AUTOMATIC, NULL },
        { "JPEG"                      , IMAGE_TYPE_JPEG     , "image/jpeg" },
        { "PNG"                       , IMAGE_TYPE_PNG      , "image/png" },
        { "TGA"                       , IMAGE_TYPE_TGA      , "image/tga" },
        { "TIFF"                      , IMAGE_TYPE_TIFF     , "image/tiff" },
        { NULL }
};

typedef struct {
	ImageSavedFunc  done_func;
	gpointer        done_data;
	GList	       *metadata;
} SaveImageData;


static void
destroy_cb (GtkWidget *w,
	    GtkWidget *file_sel)
{
	GdkPixbuf     *pixbuf;
	SaveImageData *data;

	pixbuf = g_object_get_data (G_OBJECT (file_sel), "pixbuf");
	if (pixbuf != NULL)
		g_object_unref (pixbuf);

	data = g_object_get_data (G_OBJECT (file_sel), "data");
	g_free (data);
}


static void
file_save_cancel_cb (GtkDialog *file_sel,
		     int        button_number,
		     gpointer  *userdata)

{
	SaveImageData *data;

	data = g_object_get_data (G_OBJECT (file_sel), "data");
	if (data->done_func != NULL)
		(*data->done_func) (NULL, data->done_data);
	gtk_widget_destroy (GTK_WIDGET (file_sel));
}


static gboolean
save_image (GtkWindow     *parent,
     	    FileData      *file,
	    const char    *original_file,
	    GdkPixbuf     *pixbuf,
	    SaveImageData *data,
	    GtkDialog     *file_sel)
{
	gboolean image_saved = FALSE;

	if (file == NULL)
		return FALSE;

	if (file_sel != NULL)
		gtk_widget_hide (GTK_WIDGET (file_sel));

	if ((file->mime_type != NULL) && is_mime_type_writable (file->mime_type)) {
		GError  *error = NULL;
		char    *image_type;
		char   **keys = NULL;
		char   **values = NULL;

		image_type = g_strdup (file->mime_type + strlen("image/"));

		if (dlg_save_options (parent, image_type, &keys, &values)) {
			FileData *fd_orig;
			fd_orig = file_data_new_from_path (original_file);

			if (file_data_has_local_path (file, parent) &&
			    file_data_has_local_path (fd_orig, parent)) {
		
				if (_gdk_pixbuf_savev (pixbuf,
						       file->local_path,
						       fd_orig->local_path,
						       image_type,
						       keys, values,
						       &error))
					image_saved = TRUE;
				else
					_gtk_error_dialog_from_gerror_run (parent, &error);
			}
			
			file_data_unref (fd_orig);
		}

		g_free (image_type);
		g_strfreev (keys);
		g_strfreev (values);
	} 
	else
		_gtk_error_dialog_run (parent,
				       _("Image type not supported: %s"),
				       file->mime_type);

	if (data->done_func != NULL) {
		if (! image_saved)
			file = NULL;
		(*data->done_func) (file, data->done_data);
	}

	if (file_sel != NULL)
		gtk_widget_destroy (GTK_WIDGET (file_sel));

	return TRUE;
}


static void
file_save_ok_cb (GtkDialog *file_sel,
		 int        button_number,
		 gpointer  *userdata)

{
	GtkWindow     *parent;
	GtkWidget     *combo_box;
	GdkPixbuf     *pixbuf;
	FileData      *file = NULL;
	const char    *original_file;
	int            idx;
	SaveImageData *data;

	parent = g_object_get_data (G_OBJECT (file_sel), "parent_window");
	pixbuf = g_object_get_data (G_OBJECT (file_sel), "pixbuf");
	data = g_object_get_data (G_OBJECT (file_sel), "data");
	original_file = g_object_get_data (G_OBJECT (file_sel), "uri");

	file = file_data_new_from_path (gtk_file_chooser_get_uri (GTK_FILE_CHOOSER (file_sel)));

	combo_box = g_object_get_data (G_OBJECT (file_sel), "combo_box");
	idx = gtk_combo_box_get_active (GTK_COMBO_BOX (combo_box));
	if (idx < 0
            || file_options[idx].type == IMAGE_TYPE_AUTOMATIC)
		file_data_update_mime_type (file, FALSE);
	else
		file->mime_type = get_static_string (file_options[idx].mime_type);

	save_image (parent, file, original_file, pixbuf, data, file_sel);
	file_data_unref (file);
}


static void
file_save_response_cb (GtkDialog *file_sel,
		       int        button_number,
		       gpointer  *userdata)
{
	if (button_number == GTK_RESPONSE_ACCEPT)
		file_save_ok_cb (file_sel, button_number, userdata);
	else
		file_save_cancel_cb (file_sel, button_number, userdata);
}


static GtkWidget *
build_file_type_combo_box (void)
{
        GtkWidget   *combo_box;
	int          i;
        GtkListStore *list_store;
        GtkCellRenderer *renderer;
        GtkTreeIter iter;

        combo_box = gtk_combo_box_new ();
        list_store = gtk_list_store_new (1, G_TYPE_STRING);

        for (i = 0; file_options[i].name != NULL; ++i) {
                gtk_list_store_append (list_store, &iter);
                gtk_list_store_set (list_store, &iter,
                                    0, file_options[i].name,
                                    -1);
        }
        gtk_combo_box_set_model (GTK_COMBO_BOX (combo_box),
                                 GTK_TREE_MODEL (list_store));
        g_object_unref (list_store);

        renderer = gtk_cell_renderer_text_new ();
        gtk_cell_layout_clear (GTK_CELL_LAYOUT (combo_box));
        gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (combo_box), renderer, TRUE);
        gtk_cell_layout_set_attributes (GTK_CELL_LAYOUT (combo_box), renderer,
                                        "text", 0, NULL);

        gtk_combo_box_set_active (GTK_COMBO_BOX (combo_box), 0);
        return combo_box;
}


void
dlg_save_image_as (GtkWindow       *parent,
		   const char      *uri,
		   GList           *metadata,
		   GdkPixbuf       *pixbuf,
		   ImageSavedFunc   done_func,
		   gpointer         done_data)
{
	SaveImageData *data;
	GtkWidget     *file_sel;
	GtkWidget     *vbox;
	GtkWidget     *hbox;
	GtkWidget     *combo_box;

	g_return_if_fail (pixbuf != NULL);

	file_sel = gtk_file_chooser_dialog_new(_("Save Image"), parent,
					       GTK_FILE_CHOOSER_ACTION_SAVE,
					       GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
					       GTK_STOCK_SAVE, GTK_RESPONSE_ACCEPT,
					       NULL);
	gtk_file_chooser_set_local_only (GTK_FILE_CHOOSER (file_sel), FALSE);
	gtk_file_chooser_set_do_overwrite_confirmation (GTK_FILE_CHOOSER (file_sel), TRUE);
	gtk_dialog_set_default_response (GTK_DIALOG (file_sel), GTK_RESPONSE_ACCEPT);

	/**/

	vbox = gtk_vbox_new (FALSE, 0);
	gtk_container_set_border_width (GTK_CONTAINER (vbox), 0);
	gtk_file_chooser_set_extra_widget (GTK_FILE_CHOOSER (file_sel),
					   vbox);

	hbox = gtk_hbox_new (FALSE, 0);
	gtk_box_pack_start (GTK_BOX (vbox), hbox, TRUE, TRUE, 6);

	gtk_box_pack_start (GTK_BOX (hbox),
			    gtk_label_new (_("Image type:")),
			    FALSE, FALSE, 0);

	combo_box = build_file_type_combo_box ();
	gtk_box_pack_start (GTK_BOX (hbox), combo_box, FALSE, FALSE, 12);

	gtk_widget_show_all (vbox);

	/**/
	if (uri != NULL) {
		FileData *fd = file_data_new_from_path (uri);
		gtk_file_chooser_set_uri (GTK_FILE_CHOOSER (file_sel), fd->uri);
		file_data_unref (fd);
	} else {
		gtk_file_chooser_set_current_folder_uri (GTK_FILE_CHOOSER (file_sel), get_home_uri ());
	}

	g_object_ref (pixbuf);

	data = g_new0 (SaveImageData, 1);
	data->done_func = done_func;
	data->done_data = done_data;

	data->metadata = metadata;

	g_object_set_data (G_OBJECT (file_sel), "parent_window", parent);
	g_object_set_data (G_OBJECT (file_sel), "pixbuf", pixbuf);
	g_object_set_data (G_OBJECT (file_sel), "data", data);
	g_object_set_data (G_OBJECT (file_sel), "combo_box", combo_box);
	g_object_set_data (G_OBJECT (file_sel), "uri", g_strdup (uri));

	g_signal_connect (GTK_DIALOG (file_sel),
			  "response",
			  G_CALLBACK (file_save_response_cb),
			  NULL);

	g_signal_connect (G_OBJECT (file_sel),
			  "destroy",
			  G_CALLBACK (destroy_cb),
			  file_sel);

	if (parent != NULL) {
		gtk_window_set_transient_for (GTK_WINDOW (file_sel), parent);
		gtk_window_set_modal (GTK_WINDOW (file_sel), TRUE);
	}

	gtk_widget_show (file_sel);
}


void
dlg_save_image (GtkWindow       *parent,
		FileData        *file,
		GList		*metadata,
		GdkPixbuf       *pixbuf,
		ImageSavedFunc   done_func,
		gpointer         done_data)
{
	SaveImageData *data;

	if (file == NULL)
		return;

	data = g_new0 (SaveImageData, 1);
	data->done_func = done_func;
	data->done_data = done_data;
	data->metadata = metadata;

	file_data_update_mime_type (file, TRUE);
	save_image (parent, file, file->utf8_path, pixbuf, data, NULL);
}


#define GLADE_FILE "gthumb_convert.glade"


typedef struct {
	char      *name;
	ImageType  type;
	int        n_params;
} TypeEntry;


TypeEntry types_table[] = {
	{ "jpeg", IMAGE_TYPE_JPEG, 4 },
	{ "png", IMAGE_TYPE_PNG, 0 },
	{ "tiff", IMAGE_TYPE_TIFF, 3 },
	{ "tga", IMAGE_TYPE_TGA, 1 },
	{ NULL, 0, 0 }
};


static TypeEntry*
get_type_from_name (const char *name)
{
	int i;

	for (i = 0; types_table[i].name != NULL; i++)
		if (strcasecmp (types_table[i].name, name) == 0)
			return types_table + i;

	return NULL;
}


gboolean
dlg_save_options (GtkWindow    *parent,
		  const char   *image_type,
		  char       ***keys,
		  char       ***values)
{
	GtkWidget  *dialog;
	GladeXML   *gui;
	char       *dialog_name;
	gboolean    retval = TRUE;
	TypeEntry  *type;

	*keys   = NULL;
	*values = NULL;

	/* This shouldn't happen. */
	if (image_type == NULL) {
		g_warning ("Invalid image type\n");
		return FALSE;
	}

	if (strncmp (image_type, "x-", 2) == 0)
		image_type += 2;

	type = get_type_from_name (image_type);

	/* It doesn't have to be in our list to be writable.
	   However, we don't know how to set options for this
	   image type. */
	if (type == NULL)
		return TRUE;

	/* This file type doesn't have any options. */
	if (type->n_params == 0)
		return TRUE;

	gui = glade_xml_new (GTHUMB_GLADEDIR "/" GLADE_FILE, NULL, NULL);
	if (! gui) {
		g_warning ("Could not find " GLADE_FILE "\n");
		return FALSE;
	}

	dialog_name = g_strconcat (image_type, "_options_dialog", NULL);

	dialog = glade_xml_get_widget (gui, dialog_name);
	g_free (dialog_name);

	/* Set default values */

	switch (type->type) {
		GtkWidget *widget;
		char      *svalue;
		int        ivalue;

	case IMAGE_TYPE_JPEG:
		ivalue = eel_gconf_get_integer (PREF_JPEG_QUALITY, 85);
		widget = glade_xml_get_widget (gui, "jpeg_quality_hscale");
		gtk_range_set_value (GTK_RANGE (widget), (double) ivalue);

		/**/

		ivalue = eel_gconf_get_integer (PREF_JPEG_SMOOTHING, 0);
		widget = glade_xml_get_widget (gui, "jpeg_smooth_hscale");
		gtk_range_set_value (GTK_RANGE (widget), (double) ivalue);

		/**/

		ivalue = eel_gconf_get_boolean (PREF_JPEG_OPTIMIZE, TRUE);
		widget = glade_xml_get_widget (gui, "jpeg_optimize_checkbutton");
		gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (widget), ivalue);

		/**/

		ivalue = eel_gconf_get_boolean (PREF_JPEG_PROGRESSIVE, FALSE);
		widget = glade_xml_get_widget (gui, "jpeg_progressive_checkbutton");
		gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (widget), ivalue);
		break;

	case IMAGE_TYPE_PNG:
		break;

	case IMAGE_TYPE_TIFF:
		svalue = eel_gconf_get_string (PREF_TIFF_COMPRESSION, "deflate");

		if (svalue == NULL)
			widget = NULL;
		else if (strcmp (svalue, "none") == 0)
			widget = glade_xml_get_widget (gui, "tiff_comp_none_radiobutton");
		else if (strcmp (svalue, "deflate") == 0)
			widget = glade_xml_get_widget (gui, "tiff_comp_deflate_radiobutton");
		else if (strcmp (svalue, "jpeg") == 0)
			widget = glade_xml_get_widget (gui, "tiff_comp_jpeg_radiobutton");
		else
			widget = NULL;

		if (widget != NULL)
			gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (widget), TRUE);

		g_free (svalue);

		/**/

		ivalue = eel_gconf_get_integer (PREF_TIFF_HORIZONTAL_RES, 72);
		widget = glade_xml_get_widget (gui, "tiff_hdpi_spinbutton");
		gtk_spin_button_set_value (GTK_SPIN_BUTTON (widget), (double) ivalue);

		/**/

		ivalue = eel_gconf_get_integer (PREF_TIFF_VERTICAL_RES, 72);
		widget = glade_xml_get_widget (gui, "tiff_vdpi_spinbutton");
		gtk_spin_button_set_value (GTK_SPIN_BUTTON (widget), (double) ivalue);

		break;

	case IMAGE_TYPE_TGA:
		ivalue = eel_gconf_get_boolean (PREF_TGA_RLE_COMPRESSION, TRUE);
		widget = glade_xml_get_widget (gui, "tga_rle_compression_checkbutton");
		gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (widget), ivalue);
		break;

	default:
		break;
	}

	/* Run dialog. */

	if (parent != NULL)
		gtk_window_set_transient_for (GTK_WINDOW (dialog), parent);

	if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_OK) {
		GtkWidget *widget;
		int        ivalue, i = 0;

		*keys   = g_malloc (sizeof (char*) * (type->n_params + 1));
		*values = g_malloc (sizeof (char*) * (type->n_params + 1));

		switch (type->type) {
		case IMAGE_TYPE_JPEG:
			(*keys)[i] = g_strdup ("quality");

			widget = glade_xml_get_widget (gui, "jpeg_quality_hscale");
			ivalue = gtk_range_get_value (GTK_RANGE (widget));
			(*values)[i] = g_strdup_printf ("%d", ivalue);

			eel_gconf_set_integer (PREF_JPEG_QUALITY, ivalue);

			/**/

			i++;
			(*keys)[i] = g_strdup ("smooth");

			widget = glade_xml_get_widget (gui, "jpeg_smooth_hscale");
			ivalue = gtk_range_get_value (GTK_RANGE (widget));
			(*values)[i] = g_strdup_printf ("%d", ivalue);

			eel_gconf_set_integer (PREF_JPEG_SMOOTHING, ivalue);

			/**/

			i++;
			(*keys)[i] = g_strdup ("optimize");

			widget = glade_xml_get_widget (gui, "jpeg_optimize_checkbutton");
			ivalue = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (widget));
			(*values)[i] = g_strdup_printf (ivalue ? "yes" : "no");

			eel_gconf_set_boolean (PREF_JPEG_OPTIMIZE, ivalue);

			/**/

			i++;
			(*keys)[i] = g_strdup ("progressive");

			widget = glade_xml_get_widget (gui, "jpeg_progressive_checkbutton");
			ivalue = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (widget));
			(*values)[i] = g_strdup_printf (ivalue ? "yes" : "no");

			eel_gconf_set_boolean (PREF_JPEG_PROGRESSIVE, ivalue);

			break;

		case IMAGE_TYPE_PNG:
			break;

		case IMAGE_TYPE_TIFF:
			(*keys)[i] = g_strdup ("compression");

			widget = glade_xml_get_widget (gui, "tiff_comp_none_radiobutton");
			ivalue = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (widget));
			if (ivalue)
				(*values)[i] = g_strdup_printf ("none");

			widget = glade_xml_get_widget (gui, "tiff_comp_deflate_radiobutton");
			ivalue = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (widget));
			if (ivalue)
				(*values)[i] = g_strdup_printf ("deflate");

			widget = glade_xml_get_widget (gui, "tiff_comp_jpeg_radiobutton");
			ivalue = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (widget));
			if (ivalue)
				(*values)[i] = g_strdup_printf ("jpeg");

			eel_gconf_set_string (PREF_TIFF_COMPRESSION, (*values)[i]);

			/**/

			i++;
			(*keys)[i] = g_strdup ("horizontal dpi");

			widget = glade_xml_get_widget (gui, "tiff_hdpi_spinbutton");
			ivalue = gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (widget));
			(*values)[i] = g_strdup_printf ("%d", ivalue);

			eel_gconf_set_integer (PREF_TIFF_HORIZONTAL_RES, ivalue);

			/**/

			i++;
			(*keys)[i] = g_strdup ("vertical dpi");

			widget = glade_xml_get_widget (gui, "tiff_vdpi_spinbutton");
			ivalue = gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (widget));
			(*values)[i] = g_strdup_printf ("%d", ivalue);

			eel_gconf_set_integer (PREF_TIFF_VERTICAL_RES, ivalue);

			break;

		case IMAGE_TYPE_TGA:
			(*keys)[i] = g_strdup ("compression");

			widget = glade_xml_get_widget (gui, "tga_rle_compression_checkbutton");
			ivalue = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (widget));
			if (ivalue)
				(*values)[i] = g_strdup_printf ("rle");
			else
				(*values)[i] = g_strdup_printf ("none");

			eel_gconf_set_boolean (PREF_TGA_RLE_COMPRESSION, strcmp ((*values)[i], "rle") == 0);

			break;

		default:
			break;
		}

		i++;
		(*keys)[i] = NULL;
		(*values)[i] = NULL;

	} else
		retval = FALSE;

	/**/

	gtk_widget_destroy (dialog);
	g_object_unref (gui);

	return retval;
}