/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 *  GThumb
 *
 *  Copyright (C) 2001-2005 The Free Software Foundation, Inc.
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
#include <time.h>

#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <libgnome/gnome-help.h>
#include <libgnomeui/gnome-dateedit.h>
#include <glade/glade.h>

#include "comments.h"
#include "file-data.h"
#include "gth-exif-utils.h"
#include "file-utils.h"
#include "gfile-utils.h"
#include "gth-utils.h"
#include "gth-window.h"
#include "main.h"
#include "gtk-utils.h"

#define GLADE_FILE "gthumb_tools.glade"


typedef struct {
	GthWindow    *window;
	GladeXML     *gui;

	GtkWidget    *dialog;
	GtkWidget    *ok_button;

	GtkWidget    *cd_last_modified_checkbutton;
	GtkWidget    *cd_comment_checkbutton;
	GtkWidget    *cd_exif_checkbutton;
	GtkWidget    *cd_exif_orig_checkbutton;
	GtkWidget    *cd_exif_dig_checkbutton;

	GtkWidget    *cd_following_date_radiobutton;
	GtkWidget    *cd_created_radiobutton;
	GtkWidget    *cd_current_radiobutton;
	GtkWidget    *cd_exif_radiobutton;
	GtkWidget    *cd_adjust_timezone_radiobutton;
	GtkWidget    *cd_dateedit;

	GtkWidget    *cd_timezone_box;
	GtkWidget    *cd_timezone_spinbutton;

	GList        *file_list;
} DialogData;


/* called when the main dialog is closed. */
static void
destroy_cb (GtkWidget  *widget, 
	    DialogData *data)
{
	file_data_list_free (data->file_list);
	g_object_unref (data->gui);
	g_free (data);
}


static gboolean
exif_time_available (DialogData *data)
{
	FileData *fd;

	if (data->file_list == NULL)
		return FALSE;

	if (data->file_list->next != NULL)
		return TRUE;

	fd = data->file_list->data;

	return (get_exif_time (fd) != 0);
}


#define is_active(x) (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (x)))


static void
ok_clicked (GtkWidget  *button, 
	    DialogData *data)
{
	GList  *scan, *file_list = NULL;
	time_t  mtime = 0;
	time_t  comment_time = 0;

	if (is_active (data->cd_following_date_radiobutton)) {
		mtime = gnome_date_edit_get_time (GNOME_DATE_EDIT (data->cd_dateedit));
		comment_time = mtime;
	} else if (is_active (data->cd_current_radiobutton)) {
		time (&mtime);
		comment_time = mtime;
	}
	
	gth_monitor_pause ();

	for (scan = data->file_list; scan; scan = scan->next) {
		FileData *fdata = scan->data;

		if (is_active (data->cd_created_radiobutton)) {
			mtime = fdata->ctime;
			comment_time = mtime;
		} else if (is_active (data->cd_exif_radiobutton)) {
			mtime = get_exif_time (fdata);
			comment_time = mtime;
		} else if (is_active (data->cd_adjust_timezone_radiobutton)) {
			time_t tz;

			comment_time = 0;
			tz = (int) gtk_spin_button_get_value (GTK_SPIN_BUTTON (data->cd_timezone_spinbutton)) * (60 * 60);
			if (is_active (data->cd_comment_checkbutton)) {
				CommentData *cdata;
				cdata = file_data_get_comment (fdata, TRUE);
				if (cdata != NULL) {
					comment_time = cdata->time + tz;
				}
			}
			if (is_active (data->cd_last_modified_checkbutton)) 
				mtime = fdata->mtime + tz;
		}
			
		if ((mtime <= 0) && (comment_time <= 0))
			continue;

		if (is_active (data->cd_last_modified_checkbutton))
			gfile_set_mtime (fdata->gfile, mtime);

		if (is_active (data->cd_comment_checkbutton)) {
			CommentData *cdata;
			cdata = comments_load_comment (fdata->utf8_path, TRUE);
			if (cdata == NULL)
				cdata = comment_data_new ();
			cdata->time = comment_time;
			comments_save_comment (fdata->utf8_path, cdata);
			comment_data_free (cdata);
		}

		if (is_active (data->cd_exif_checkbutton) ||
		    is_active (data->cd_exif_orig_checkbutton) ||
		    is_active (data->cd_exif_dig_checkbutton)) {
			char      *buf;
			struct tm  tm;
			GList	  *add_metadata = NULL;

			localtime_r (&mtime, &tm);
			buf = g_strdup_printf ("%04d:%02d:%02d %02d:%02d:%02d ", 
					       tm.tm_year + 1900,
			       		       tm.tm_mon + 1,
			       		       tm.tm_mday,
			       		       tm.tm_hour,
			       		       tm.tm_min,
			       		       tm.tm_sec );

			if (is_active (data->cd_exif_checkbutton))
				add_metadata = simple_add_metadata (add_metadata, "Exif.Image.DateTime", buf);

			if (is_active (data->cd_exif_orig_checkbutton))
				add_metadata = simple_add_metadata (add_metadata, "Exif.Photo.DateTimeOriginal", buf);

			if (is_active (data->cd_exif_dig_checkbutton))
				add_metadata = simple_add_metadata (add_metadata, "Exif.Photo.DateTimeDigitized", buf);

                        update_and_save_metadata_fd (fdata, fdata, add_metadata);

			free_metadata (add_metadata);
			g_free (buf);
		}

		file_list = g_list_prepend (file_list, fdata->gfile);
		g_object_ref (fdata->gfile);
	}

	gth_monitor_notify_update_gfiles (GTH_MONITOR_EVENT_CHANGED, file_list);
	gfile_list_free (file_list);

	gth_monitor_resume ();
	
	gtk_widget_destroy (data->dialog);
}


static void
update_sensitivity (DialogData *data)
{
	gtk_widget_set_sensitive (data->ok_button,
				  gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (data->cd_last_modified_checkbutton))
				  || gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (data->cd_comment_checkbutton))
				  || gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (data->cd_exif_checkbutton))
				  || gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (data->cd_exif_orig_checkbutton))
				  || gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (data->cd_exif_dig_checkbutton)));
		
	gtk_widget_set_sensitive (data->cd_dateedit, gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (data->cd_following_date_radiobutton)));
	gtk_widget_set_sensitive (data->cd_timezone_box, gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (data->cd_adjust_timezone_radiobutton)));
}


static void
radio_button_clicked (GtkWidget  *button, 
		      DialogData *data)
{
	update_sensitivity (data);
}



/* called when the "help" button is clicked. */
static void
help_cb (GtkWidget  *widget, 
	 DialogData *data)
{
	gthumb_display_help (GTK_WINDOW (data->dialog), "gthumb-change-date");
}


void
dlg_change_date (GthWindow *window)
{
	DialogData  *data;
	GtkWidget   *cancel_button;
	GtkWidget   *help_button;
	GList       *list;

	list = gth_window_get_file_list_selection_as_fd (window);
	if (list == NULL) {
		g_warning ("No file selected.");
		return;
	}

	/**/

	data = g_new (DialogData, 1);

	data->file_list = list;
	data->gui = glade_xml_new (GTHUMB_GLADEDIR "/" GLADE_FILE, NULL,
				   NULL);

	if (! data->gui) {
		g_warning ("Could not find " GLADE_FILE "\n");
		if (data->file_list != NULL) 
			g_list_free (data->file_list);
		g_free (data);
		return;
	}

	/* Get the widgets. */

	data->dialog = glade_xml_get_widget (data->gui, "change_date_dialog");

	data->cd_last_modified_checkbutton = glade_xml_get_widget (data->gui, "cd_last_modified_checkbutton");
	data->cd_comment_checkbutton = glade_xml_get_widget (data->gui, "cd_comment_checkbutton");
	data->cd_exif_checkbutton = glade_xml_get_widget (data->gui, "cd_exif_checkbutton");
	data->cd_exif_orig_checkbutton = glade_xml_get_widget (data->gui, "cd_exif_orig_checkbutton");
	data->cd_exif_dig_checkbutton = glade_xml_get_widget (data->gui, "cd_exif_dig_checkbutton");

	data->cd_following_date_radiobutton = glade_xml_get_widget (data->gui, "cd_following_date_radiobutton");
	data->cd_created_radiobutton = glade_xml_get_widget (data->gui, "cd_created_radiobutton");
	data->cd_current_radiobutton = glade_xml_get_widget (data->gui, "cd_current_radiobutton");
	data->cd_exif_radiobutton = glade_xml_get_widget (data->gui, "cd_exif_radiobutton");
	data->cd_adjust_timezone_radiobutton = glade_xml_get_widget (data->gui, "cd_adjust_timezone_radiobutton");
	data->cd_dateedit = glade_xml_get_widget (data->gui, "cd_dateedit");

	data->cd_timezone_box = glade_xml_get_widget (data->gui, "cd_timezone_box");
	data->cd_timezone_spinbutton = glade_xml_get_widget (data->gui, "cd_timezone_spinbutton");

	cancel_button = glade_xml_get_widget (data->gui, "cd_cancel_button");
	data->ok_button = glade_xml_get_widget (data->gui, "cd_ok_button");
	help_button = glade_xml_get_widget (data->gui, "cd_help_button");

	/* Set widgets data. */

	gtk_widget_set_sensitive (data->cd_exif_radiobutton, exif_time_available (data));

	gtk_widget_set_sensitive (data->cd_dateedit, gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (data->cd_following_date_radiobutton)));

	update_sensitivity (data);

	/* Set the signals handlers. */
	
	g_signal_connect (G_OBJECT (data->dialog),
			  "destroy",
			  G_CALLBACK (destroy_cb),
			  data);

	g_signal_connect_swapped (G_OBJECT (cancel_button), 
				  "clicked",
				  G_CALLBACK (gtk_widget_destroy),
				  G_OBJECT (data->dialog));
	g_signal_connect (G_OBJECT (data->ok_button), 
			  "clicked",
			  G_CALLBACK (ok_clicked),
			  data);
	g_signal_connect (G_OBJECT (help_button), 
			  "clicked",
			  G_CALLBACK (help_cb),
			  data);

	g_signal_connect (G_OBJECT (data->cd_last_modified_checkbutton), 
			  "clicked",
			  G_CALLBACK (radio_button_clicked),
			  data);
	g_signal_connect (G_OBJECT (data->cd_comment_checkbutton), 
			  "clicked",
			  G_CALLBACK (radio_button_clicked),
			  data);
	g_signal_connect (G_OBJECT (data->cd_exif_checkbutton), 
			  "clicked",
			  G_CALLBACK (radio_button_clicked),
			  data); 
        g_signal_connect (G_OBJECT (data->cd_exif_orig_checkbutton),
                          "clicked",
                          G_CALLBACK (radio_button_clicked),
                          data);
        g_signal_connect (G_OBJECT (data->cd_exif_dig_checkbutton),
                          "clicked",
                          G_CALLBACK (radio_button_clicked),
                          data);
	
	g_signal_connect (G_OBJECT (data->cd_following_date_radiobutton), 
			  "clicked",
			  G_CALLBACK (radio_button_clicked),
			  data);
	g_signal_connect (G_OBJECT (data->cd_created_radiobutton), 
			  "clicked",
			  G_CALLBACK (radio_button_clicked),
			  data);
	g_signal_connect (G_OBJECT (data->cd_current_radiobutton), 
			  "clicked",
			  G_CALLBACK (radio_button_clicked),
			  data);
	g_signal_connect (G_OBJECT (data->cd_exif_radiobutton), 
			  "clicked",
			  G_CALLBACK (radio_button_clicked),
			  data);
	g_signal_connect (G_OBJECT (data->cd_adjust_timezone_radiobutton), 
			  "clicked",
			  G_CALLBACK (radio_button_clicked),
			  data);

	/* Run dialog. */

	gtk_window_set_transient_for (GTK_WINDOW (data->dialog), 
				      GTK_WINDOW (window));
	gtk_window_set_modal (GTK_WINDOW (data->dialog), TRUE); 
	gtk_widget_show (data->dialog);
}