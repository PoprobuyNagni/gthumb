/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 *  GThumb
 *
 *  Copyright (C) 2001, 2003 Free Software Foundation, Inc.
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
#include <unistd.h>

#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <gio/gio.h>
#include <libgnomevfs/gnome-vfs-utils.h>
#include <libgnomevfs/gnome-vfs-ops.h>
#include <glade/glade.h>

#include "thumb-cache.h"
#include "comments.h"
#include "dlg-file-utils.h"
#include "file-utils.h"
#include "glib-utils.h"
#include "image-viewer.h"
#include "main.h"
#include "gconf-utils.h"
#include "gtk-utils.h"
#include "gth-browser.h"
#include "gth-folder-selection-dialog.h"
#include "gfile-utils.h"

#define GLADE_FILE "gthumb.glade"
#define GLADE_PNG_EXP_FILE "gthumb_png_exporter.glade"
#define DISPLAY_PROGRESS_DELAY 1000
#define FILE_NAME_MAX_LENGTH   30
#define PREVIEW_SIZE           150

typedef enum {
	OVERWRITE_RESULT_UNSPECIFIED,
	OVERWRITE_RESULT_YES,
	OVERWRITE_RESULT_NO,
	OVERWRITE_RESULT_ALL,
	OVERWRITE_RESULT_NONE,
	OVERWRITE_RESULT_RENAME
} OverwriteResult;


typedef enum {
	FILE_OP_COPY,
	FILE_OP_MOVE,
	FILE_OP_DELETE
} FileOp;


typedef void (*OverwriteDoneFunc) (OverwriteResult  result,
				   char            *new_name,
				   gpointer         data);


/* GIO Port: VFS -> GIO error code mapping */

void
set_error_from_vfs_result (GnomeVFSResult     result,
                           GError           **err)
{
	g_assert (*err == NULL);

	switch (result) {
	case GNOME_VFS_OK:
		break;

	case GNOME_VFS_ERROR_INTERRUPTED:
		g_set_error (err,
		             G_FILE_ERROR,
		             G_FILE_ERROR_INTR,
			     _("Operation interrupted"));
		break;

	case GNOME_VFS_ERROR_NOT_FOUND:
		g_set_error (err,
		             G_FILE_ERROR,
		             G_FILE_ERROR_NOENT,
			     _("File not found"));
		break;

	case GNOME_VFS_ERROR_BAD_PARAMETERS:
		g_set_error (err,
			     G_IO_ERROR ,
			     G_IO_ERROR_INVALID_ARGUMENT,
			     _("Invalid argument"));
		break;

	default:
		g_set_error (err,
		             G_FILE_ERROR,
		             G_FILE_ERROR_FAILED,
			     _("Operation failed"));
		break;
	}
}


gboolean
dlg_check_folder (GthWindow  *window,
		  const char *path)
{
	GtkWidget *d;
	char      *dir;
	int        result;

	dir = remove_ending_separator (path);

	if (path_is_dir (dir)) {
		g_free (dir);
		return TRUE;
	}

	d = _gtk_yesno_dialog_new (GTK_WINDOW (window),
				   GTK_DIALOG_MODAL,
				   _("The destination folder does not exist. " "Do you want to create it?"),
				   GTK_STOCK_CANCEL,
				   _("C_reate"));

	result = gtk_dialog_run (GTK_DIALOG (d));
	gtk_widget_destroy (GTK_WIDGET (d));

	if (result != GTK_RESPONSE_YES) {
		g_free (dir);
		return FALSE;
	}

	if (! ensure_dir_exists (dir)) {
		char *utf8_path;
		utf8_path = get_utf8_display_name_from_uri (dir);
		_gtk_error_dialog_run (GTK_WINDOW (window),
				       _("Could not create folder \"%s\": %s."),
				       utf8_path,
				       errno_to_string ());
		g_free (utf8_path);
		g_free (dir);
		return FALSE;
	}

	g_free (dir);
	return TRUE;
}


/**/

static void
dlg_show_error (GthWindow  *window,
		const char *first_line,
		GList      *error_list,
		const char *reason)
{
	const int   max_files = 15; /* FIXME: find a good value */
	GtkWidget  *d;
	GList      *scan;
	char       *msg;
	char       *utf8_msg;
	int         n = 0;

	msg = g_strdup_printf ("%s\n", first_line);
	for (scan = error_list; scan && (n < max_files); scan = scan->next) {
		char *tmp = msg;
		msg = g_strconcat (tmp, "  ", scan->data, "\n", NULL);
		g_free (tmp);
		n++;
	}

	if (scan != NULL) {
		char *tmp = msg;
		msg = g_strconcat (tmp, "...\n", NULL);
		g_free (tmp);
	}

	if (reason != NULL) {
		char *tmp = msg;
		msg = g_strconcat (tmp, reason, NULL);
		g_free (tmp);
	}

	utf8_msg = g_locale_to_utf8 (msg, -1, NULL, NULL, NULL);

	d = _gtk_message_dialog_new (GTK_WINDOW (window),
				     GTK_DIALOG_MODAL,
				     GTK_STOCK_DIALOG_ERROR,
				     utf8_msg,
				     NULL,
				     GTK_STOCK_OK, GTK_RESPONSE_CANCEL,
				     NULL);
	g_free (msg);
	g_free (utf8_msg);

	gtk_dialog_run (GTK_DIALOG (d));
	gtk_widget_destroy (d);
}


/* -- delete -- */


typedef struct {
	GthWindow *window;
	GList     *file_list;
} ConfirmFileDeleteData;

static void
real_files_delete__continue2 (GError   *error,
			      gpointer  data)
{
	ConfirmFileDeleteData *cfddata = data;

	if ((error != NULL) && (!g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_INTR)))
		_gtk_error_dialog_run (GTK_WINDOW (cfddata->window),
				       "%s %s",
				       _("Could not delete the images:"),
				       error->message);

	file_data_list_free (cfddata->file_list);
	g_free (cfddata);
}


/* Display a dialog asking the user if they wish to delete
 * the file(s) if there is no trash avalible.
 */
static void
real_files_delete__no_trash (ConfirmFileDeleteData *cfddata)
{
	int r = GTK_RESPONSE_YES;

	if (eel_gconf_get_boolean (PREF_MSG_CANNOT_MOVE_TO_TRASH, TRUE)) {
		GtkWidget *d;

		d = _gtk_yesno_dialog_with_checkbutton_new (
			    GTK_WINDOW (cfddata->window),
			    GTK_DIALOG_MODAL,
			    _("The images cannot be moved to the Trash. Do you want to delete them permanently?"),
			    GTK_STOCK_CANCEL,
			    GTK_STOCK_DELETE,
			    _("_Do not display this message again"),
			    PREF_MSG_CANNOT_MOVE_TO_TRASH);
		r = gtk_dialog_run (GTK_DIALOG (d));
		gtk_widget_destroy (GTK_WIDGET (d));
	}

	if (r == GTK_RESPONSE_YES) {
		dlg_files_delete (cfddata->window,
				  cfddata->file_list,
				  real_files_delete__continue2,
				  cfddata);
	}
	else {
		file_data_list_free (cfddata->file_list);
		g_free (cfddata);
	}

}


static void
real_files_delete__continue (GError   *error,
			     gpointer  data)
{
	ConfirmFileDeleteData *cfddata = data;

	if ((error != NULL)) {
		_gtk_error_dialog_run (GTK_WINDOW (cfddata->window),
				       "%s %s",
				       _("Could not delete the images:"),
				       error->message);
		file_data_list_free (cfddata->file_list);
		g_free (cfddata);
	} else if (cfddata->file_list != NULL) {
		real_files_delete__no_trash (cfddata);
	} else {
		file_data_list_free (cfddata->file_list);
		g_free (cfddata);
	}
}


static void
real_files_delete (GthWindow *window,
		   GList     *list)
{
	ConfirmFileDeleteData *cfddata;

	cfddata = g_new0 (ConfirmFileDeleteData, 1);
	cfddata->window = window;
	cfddata->file_list = list;

	dlg_files_move_to_trash (window,
				 list,
				 real_files_delete__continue,
				 cfddata);
	return;
}


gboolean
dlg_file_delete__confirm (GthWindow  *window,
			  GList      *list,
			  const char *message)
{
	GtkWidget *dialog;
	int        result;

	if (! eel_gconf_get_boolean (PREF_CONFIRM_DELETION, TRUE)) {
		real_files_delete (window, list);
		return TRUE;
	}

	dialog = _gtk_yesno_dialog_new (GTK_WINDOW (window),
					GTK_DIALOG_MODAL,
					message,
					GTK_STOCK_CANCEL,
					_("_Move"));

	result = gtk_dialog_run (GTK_DIALOG (dialog));
	gtk_widget_destroy (GTK_WIDGET (dialog));
	if (result == GTK_RESPONSE_YES) {
		real_files_delete (window, list);
		return TRUE;
	}

	return FALSE;
}


/* -- move -- */


static void
destroy_cb (GtkWidget *w,
	    GtkWidget *file_sel)
{
	GList *file_list;

	file_list = g_object_get_data (G_OBJECT (file_sel), "list");
	path_list_free (file_list);
}


static void
file_move_ask__continue (GError	       *error,
			 gpointer       data)
{
	GtkWidget *file_sel = data;
	GthWindow *window;

	window = g_object_get_data (G_OBJECT (file_sel), "gthumb_window");
	if (GTH_IS_BROWSER (window) &&
	    gth_folder_selection_get_goto_destination (GTH_FOLDER_SELECTION (file_sel))) {
		GthBrowser *browser = GTH_BROWSER (window);
		gth_browser_go_to_directory (browser, gth_folder_selection_get_folder (GTH_FOLDER_SELECTION (file_sel)));
	}

	gtk_widget_destroy (GTK_WIDGET (file_sel));
}


static void
file_move_response_cb (GtkWidget *w,
		       int        response_id,
		       GtkWidget *file_sel)
{
	GthWindow *window;
	GList     *file_list;
	char      *path;
	int        len;

	if (response_id != GTK_RESPONSE_OK) {
		gtk_widget_destroy (file_sel);
		return;
	}

	window = g_object_get_data (G_OBJECT (file_sel), "gthumb_window");
	file_list = g_object_get_data (G_OBJECT (file_sel), "list");
	path = g_strdup (gth_folder_selection_get_folder (GTH_FOLDER_SELECTION (file_sel)));

	if (path == NULL)
		return;

	/* ignore ending slash. */
	len = strlen (path);
	if ((len > 1) && (path[len - 1] == '/'))
		path[len - 1] = 0;

	if (dlg_check_folder (window, path)) {
		gtk_widget_hide (file_sel);

		dlg_files_copy (window,
				file_list,
				path,
				TRUE,
				TRUE,
				FALSE,
				file_move_ask__continue,
				file_sel);
	}

	g_free (path);
}


void
dlg_file_move__ask_dest (GthWindow  *window,
			 const char *default_dir,
			 GList      *list)
{
	GtkWidget   *file_sel;
	const char  *path;

	file_sel = gth_folder_selection_new (GTK_WINDOW (window), _("File move - choose destination folder"));

	if (default_dir != NULL)
		path = default_dir;
	else
		path = get_home_uri ();

	gth_folder_selection_set_folder (GTH_FOLDER_SELECTION (file_sel), path);

	g_object_set_data (G_OBJECT (file_sel), "list", list);
	g_object_set_data (G_OBJECT (file_sel), "gthumb_window", window);

	g_signal_connect (G_OBJECT (file_sel),
			  "response",
			  G_CALLBACK (file_move_response_cb),
			  file_sel);

	g_signal_connect (G_OBJECT (file_sel),
			  "destroy",
			  G_CALLBACK (destroy_cb),
			  file_sel);

	gtk_window_set_transient_for (GTK_WINDOW (file_sel), GTK_WINDOW (window));
	gtk_window_set_modal (GTK_WINDOW (file_sel), TRUE);
	gtk_widget_show (file_sel);
}


/* -- copy -- */


static void
file_copy_ask__continue (GError	  *error,
			 gpointer  data)
{
	GtkWidget *file_sel = data;
	GthWindow *window;

	window = g_object_get_data (G_OBJECT (file_sel), "gthumb_window");
	if (GTH_IS_BROWSER (window) &&
	    gth_folder_selection_get_goto_destination (GTH_FOLDER_SELECTION (file_sel))) {
		GthBrowser *browser = GTH_BROWSER (window);
		gth_browser_go_to_directory (browser, gth_folder_selection_get_folder (GTH_FOLDER_SELECTION (file_sel)));
	}

	gtk_widget_destroy (GTK_WIDGET (file_sel));
}


static void
file_copy_response_cb (GtkWidget *w,
		       int        response_id,
		       GtkWidget *file_sel)
{
	GthWindow *window;
	GList     *file_list;
	char      *path;
	int        len;

	if (response_id != GTK_RESPONSE_OK) {
		gtk_widget_destroy (file_sel);
		return;
	}

	gtk_widget_hide (file_sel);

	window = g_object_get_data (G_OBJECT (file_sel), "gthumb_window");
	file_list = g_object_get_data (G_OBJECT (file_sel), "list");
	path = g_strdup (gth_folder_selection_get_folder (GTH_FOLDER_SELECTION (file_sel)));

	/* ignore ending slash. */
	len = strlen (path);
	if ((len > 1) && (path[len - 1] == '/'))
		path[len - 1] = 0;

	if (dlg_check_folder (window, path)) {
		gtk_widget_hide (file_sel);

		dlg_files_copy (window,
				file_list,
				path,
				FALSE,
				TRUE,
				FALSE,
				file_copy_ask__continue,
				file_sel);
	}

	g_free (path);
}


void
dlg_file_copy__ask_dest (GthWindow  *window,
			 const char *default_dir,
			 GList      *list)
{
	GtkWidget  *file_sel;
	const char *path;

	file_sel = gth_folder_selection_new (GTK_WINDOW (window), _("File copy - choose the destination folder"));

	if (default_dir != NULL)
		path = default_dir;
	else
		path = get_home_uri ();

	gth_folder_selection_set_folder (GTH_FOLDER_SELECTION (file_sel), path);

	g_object_set_data (G_OBJECT (file_sel), "list", list);
	g_object_set_data (G_OBJECT (file_sel), "gthumb_window", window);

	g_signal_connect (G_OBJECT (file_sel),
			  "response",
			  G_CALLBACK (file_copy_response_cb),
			  file_sel);

	g_signal_connect (G_OBJECT (file_sel),
			  "destroy",
			  G_CALLBACK (destroy_cb),
			  file_sel);

	gtk_window_set_transient_for (GTK_WINDOW (file_sel), GTK_WINDOW (window));
	gtk_window_set_modal (GTK_WINDOW (file_sel), TRUE);
	gtk_widget_show (file_sel);
}


/* -- overwrite dialog -- */


static void
set_filename_labels (GladeXML    *gui,
		     const char  *filename_widget,
		     const char  *filename_eventbox,
		     const char  *size_widget,
		     const char  *time_widget,
		     const char  *filename)
{
	GtkWidget  *label;
	GtkWidget  *eventbox;
	struct tm  *tm;
	char        time_txt[50];
	char       *file_size_txt;
	FileData   *fd;

	fd = file_data_new_from_path (filename);

	label = glade_xml_get_widget (gui, filename_widget);
	eventbox = glade_xml_get_widget (gui, filename_eventbox);

	gtk_label_set_text (GTK_LABEL (label), fd->utf8_name);
	gtk_widget_set_tooltip_text (eventbox, fd->utf8_path);

	label = glade_xml_get_widget (gui, size_widget);
	file_size_txt = g_format_size_for_display (fd->size);
	_gtk_label_set_locale_text (GTK_LABEL (label), file_size_txt);
	g_free (file_size_txt);

	label = glade_xml_get_widget (gui, time_widget);
	tm = localtime (&(fd->mtime));
	strftime (time_txt, 50, _("%d %b %Y, %H:%M"), tm);
	_gtk_label_set_locale_text (GTK_LABEL (label), time_txt);

	file_data_unref (fd);
}


/* -- dlg_overwrite -- */


typedef struct {
	GthWindow         *window;
	GladeXML          *gui;
	char              *destination;

	GtkWidget         *dialog;
	GtkWidget         *overwrite_yes_radiobutton;
	GtkWidget         *overwrite_no_radiobutton;
	GtkWidget         *overwrite_all_radiobutton;
	GtkWidget         *overwrite_none_radiobutton;
	GtkWidget         *overwrite_rename_radiobutton;
	GtkWidget         *overwrite_rename_entry;

	int                overwrite_mode;
	OverwriteDoneFunc  done_func;
	gpointer           done_data;
} DlgOverwriteData;


static void
dlg_overwrite_data_free (DlgOverwriteData *owdata)
{
	gtk_widget_destroy (owdata->dialog);
	g_object_unref (owdata->gui);
	g_free (owdata->destination);
	g_free (owdata);
}


static void
overwrite_rename_radiobutton_toggled_cb (GtkToggleButton  *button,
					 DlgOverwriteData *owdata)
{
	gtk_widget_set_sensitive (owdata->overwrite_rename_entry,
				  gtk_toggle_button_get_active (button));
}


static DlgOverwriteData *
create_overwrite_dialog (GthWindow         *window,
			 int                default_overwrite_mode,
			 const char        *old_filename,
			 const char        *new_filename,
			 gboolean           show_overwrite_all_none,
			 OverwriteDoneFunc  done_func,
			 gpointer           done_data)
{
	DlgOverwriteData *owdata;
	GtkWidget        *old_image_frame;
	GtkWidget        *old_img_zoom_in_button;
	GtkWidget        *old_img_zoom_out_button;
	GtkWidget        *new_image_frame;
	GtkWidget        *new_img_zoom_in_button;
	GtkWidget        *new_img_zoom_out_button;
	GtkWidget        *old_image_viewer, *new_image_viewer;
	GtkWidget        *overwrite_radiobutton;
	ImageViewer      *viewer, *w_viewer;

	owdata = g_new0 (DlgOverwriteData, 1);

	owdata->window = window;
	owdata->gui = glade_xml_new (GTHUMB_GLADEDIR "/" GLADE_FILE, NULL, NULL);
	if (! owdata->gui) {
		g_warning ("Could not find " GLADE_FILE "\n");
		g_free (owdata);
		return NULL;
	}

	owdata->done_func = done_func;
	owdata->done_data = done_data;
	owdata->destination = remove_level_from_path (old_filename);

	/* Get the widgets. */

	owdata->dialog = glade_xml_get_widget (owdata->gui, "overwrite_dialog");

	old_image_frame = glade_xml_get_widget (owdata->gui, "old_image_frame");
	old_img_zoom_in_button = glade_xml_get_widget (owdata->gui, "old_img_zoom_in_button");
	old_img_zoom_out_button = glade_xml_get_widget (owdata->gui, "old_img_zoom_out_button");
	new_image_frame = glade_xml_get_widget (owdata->gui, "new_image_frame");
	new_img_zoom_in_button = glade_xml_get_widget (owdata->gui, "new_img_zoom_in_button");
	new_img_zoom_out_button = glade_xml_get_widget (owdata->gui, "new_img_zoom_out_button");
	owdata->overwrite_yes_radiobutton = glade_xml_get_widget (owdata->gui, "overwrite_yes_radiobutton");
	owdata->overwrite_no_radiobutton = glade_xml_get_widget (owdata->gui, "overwrite_no_radiobutton");
	owdata->overwrite_all_radiobutton = glade_xml_get_widget (owdata->gui, "overwrite_all_radiobutton");
	owdata->overwrite_none_radiobutton = glade_xml_get_widget (owdata->gui, "overwrite_none_radiobutton");
	owdata->overwrite_rename_radiobutton = glade_xml_get_widget (owdata->gui, "overwrite_rename_radiobutton");
	owdata->overwrite_rename_entry = glade_xml_get_widget (owdata->gui, "overwrite_rename_entry");

	/* Set widgets data. */

	/* * set filename labels. */

	set_filename_labels (owdata->gui,
			     "old_image_filename_label",
			     "old_image_filename_eventbox",
			     "old_image_size_label",
			     "old_image_time_label",
			     old_filename);
	set_filename_labels (owdata->gui,
			     "new_image_filename_label",
			     "new_image_filename_eventbox",
			     "new_image_size_label",
			     "new_image_time_label",
			     new_filename);

	/* * set the default overwrite mode. */

	overwrite_radiobutton = owdata->overwrite_no_radiobutton;
	switch (default_overwrite_mode) {
	case OVERWRITE_RESULT_YES:
		overwrite_radiobutton = owdata->overwrite_yes_radiobutton;
		break;

	case OVERWRITE_RESULT_NO:
		overwrite_radiobutton = owdata->overwrite_no_radiobutton;
		break;

	case OVERWRITE_RESULT_ALL:
		overwrite_radiobutton = owdata->overwrite_all_radiobutton;
		break;

	case OVERWRITE_RESULT_NONE:
		overwrite_radiobutton = owdata->overwrite_none_radiobutton;
		break;

	case OVERWRITE_RESULT_RENAME:
		overwrite_radiobutton = owdata->overwrite_rename_radiobutton;
		break;
	}
	gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (overwrite_radiobutton), TRUE);

	if (! show_overwrite_all_none) {
		gtk_widget_hide (owdata->overwrite_all_radiobutton);
		gtk_widget_hide (owdata->overwrite_none_radiobutton);
	}

	gtk_widget_set_sensitive (owdata->overwrite_rename_entry,
				  default_overwrite_mode == OVERWRITE_RESULT_RENAME);
	_gtk_entry_set_filename_text (GTK_ENTRY (owdata->overwrite_rename_entry),
				      file_name_from_path (new_filename));
	if (default_overwrite_mode == OVERWRITE_RESULT_RENAME)
		gtk_widget_grab_focus (owdata->overwrite_rename_entry);

	/* * load images. */

	w_viewer = gth_window_get_image_viewer (window);

	old_image_viewer = image_viewer_new ();
	gtk_container_add (GTK_CONTAINER (old_image_frame), old_image_viewer);

	viewer = IMAGE_VIEWER (old_image_viewer);
	image_viewer_size (viewer, PREVIEW_SIZE, PREVIEW_SIZE);
	image_viewer_set_zoom_quality (viewer, pref_get_zoom_quality ());
	image_viewer_set_check_type (viewer, image_viewer_get_check_type (w_viewer));
	image_viewer_set_check_size (viewer, image_viewer_get_check_size (w_viewer));
	image_viewer_set_transp_type (viewer, image_viewer_get_transp_type (w_viewer));
	image_viewer_set_fit_mode (viewer, GTH_FIT_SIZE_IF_LARGER);
	image_viewer_load_image_from_uri (viewer, old_filename);

	gtk_widget_show (old_image_viewer);

	/**/

	new_image_viewer = image_viewer_new ();
	gtk_container_add (GTK_CONTAINER (new_image_frame), new_image_viewer);

	viewer = IMAGE_VIEWER (new_image_viewer);
	image_viewer_size (viewer, PREVIEW_SIZE, PREVIEW_SIZE);
	image_viewer_set_zoom_quality (viewer, pref_get_zoom_quality ());
	image_viewer_set_check_type (viewer, image_viewer_get_check_type (w_viewer));
	image_viewer_set_check_size (viewer, image_viewer_get_check_size (w_viewer));
	image_viewer_set_transp_type (viewer, image_viewer_get_transp_type (w_viewer));
	image_viewer_set_fit_mode (viewer, GTH_FIT_SIZE_IF_LARGER);
	image_viewer_load_image_from_uri (viewer, new_filename);

	gtk_widget_show (new_image_viewer);

	/* signals. */

	g_signal_connect (G_OBJECT (owdata->overwrite_rename_radiobutton),
			  "toggled",
			  G_CALLBACK (overwrite_rename_radiobutton_toggled_cb),
			  owdata);

	/**/

	gtk_window_set_transient_for (GTK_WINDOW (owdata->dialog),
				      GTK_WINDOW (window));
	gtk_window_set_modal (GTK_WINDOW (owdata->dialog), TRUE);

	return owdata;
}


static void
dlg_overwrite__response_cb (GtkWidget *dialog,
			    int        response_id,
			    gpointer   data)
{
	DlgOverwriteData *owdata   = data;
	int               result   = OVERWRITE_RESULT_NO;
	char             *new_name = NULL;

	if (response_id == GTK_RESPONSE_OK) {
		if (GTK_TOGGLE_BUTTON (owdata->overwrite_yes_radiobutton)->active)
			result = OVERWRITE_RESULT_YES;
		else if (GTK_TOGGLE_BUTTON (owdata->overwrite_no_radiobutton)->active)
			result = OVERWRITE_RESULT_NO;
		else if (GTK_TOGGLE_BUTTON (owdata->overwrite_all_radiobutton)->active)
			result = OVERWRITE_RESULT_ALL;
		else if (GTK_TOGGLE_BUTTON (owdata->overwrite_none_radiobutton)->active)
			result = OVERWRITE_RESULT_NONE;
		else if (GTK_TOGGLE_BUTTON (owdata->overwrite_rename_radiobutton)->active) {
			char *new_path;

			result = OVERWRITE_RESULT_RENAME;
			new_name = _gtk_entry_get_filename_text (GTK_ENTRY (owdata->overwrite_rename_entry));
			if ((new_name == NULL) || (new_name[0] == 0)) {
				_gtk_error_dialog_run (GTK_WINDOW (owdata->window),
						       _("You didn't enter the new name"));
				g_free (new_name);
				return;
			}

			new_path = build_uri (owdata->destination, new_name, NULL);

			if (path_is_file (new_path)) {
				char *utf8_name;

				utf8_name = get_utf8_display_name_from_uri (new_name);
				_gtk_error_dialog_run (GTK_WINDOW (owdata->window),
						       _("The name \"%s\" is already used in this folder. Please use a different name."),
						       utf8_name);
				g_free (utf8_name);
				g_free (new_path);
				g_free (new_name);

				return;
			}
			g_free (new_path);

		} else
			result = OVERWRITE_RESULT_NO;
	}

	if (owdata->done_func != NULL)
		(owdata->done_func) (result, new_name, owdata->done_data);
	dlg_overwrite_data_free (owdata);
}


static GtkWidget *
dlg_overwrite (GthWindow         *window,
	       int                default_overwrite_mode,
	       const char        *old_filename,
	       const char        *new_filename,
	       gboolean           show_overwrite_all_none,
	       OverwriteDoneFunc  done_func,
	       gpointer           done_data)
{
	DlgOverwriteData *owdata;

	owdata = create_overwrite_dialog (window,
					  default_overwrite_mode,
					  old_filename,
					  new_filename,
					  show_overwrite_all_none,
					  done_func,
					  done_data);

	g_signal_connect (G_OBJECT (owdata->dialog),
			  "response",
			  G_CALLBACK (dlg_overwrite__response_cb),
			  owdata);

	return owdata->dialog;
}


static int
dlg_overwrite_run (GthWindow     *window,
		   int            default_overwrite_mode,
		   const char    *old_filename,
		   const char    *new_filename,
		   gboolean       show_overwrite_all_none,
		   char         **new_name)
{
	DlgOverwriteData *owdata;
	int               result;

	owdata = create_overwrite_dialog (window,
					  default_overwrite_mode,
					  old_filename,
					  new_filename,
					  show_overwrite_all_none,
					  NULL,
					  NULL);

 retry:
	*new_name = NULL;
	result = gtk_dialog_run (GTK_DIALOG (owdata->dialog));

	if (result == -1)
		result = OVERWRITE_RESULT_NO;
	else if (GTK_TOGGLE_BUTTON (owdata->overwrite_yes_radiobutton)->active)
		result = OVERWRITE_RESULT_YES;
	else if (GTK_TOGGLE_BUTTON (owdata->overwrite_no_radiobutton)->active)
		result = OVERWRITE_RESULT_NO;
	else if (GTK_TOGGLE_BUTTON (owdata->overwrite_all_radiobutton)->active)
		result = OVERWRITE_RESULT_ALL;
	else if (GTK_TOGGLE_BUTTON (owdata->overwrite_none_radiobutton)->active)
		result = OVERWRITE_RESULT_NONE;
	else if (GTK_TOGGLE_BUTTON (owdata->overwrite_rename_radiobutton)->active) {
		char *new_path;

		result = OVERWRITE_RESULT_RENAME;
		*new_name = _gtk_entry_get_filename_text (GTK_ENTRY (owdata->overwrite_rename_entry));

		if ((*new_name == NULL) || ((*new_name)[0] == 0)) {
			_gtk_error_dialog_run (GTK_WINDOW (owdata->window),
					       _("You didn't enter the new name"));
			g_free (*new_name);
			goto retry;
		}

		new_path = build_uri (owdata->destination, *new_name, NULL);

		if (path_is_file (new_path)) {
			char *utf8_name;

			utf8_name = get_utf8_display_name_from_uri (*new_name);
			_gtk_error_dialog_run (GTK_WINDOW (owdata->window),
					       _("The name \"%s\" is already used in this folder. Please use a different name."),
					       utf8_name);
			g_free (utf8_name);
			g_free (new_path);
			g_free (*new_name);

			goto retry;
		}
		g_free (new_path);

	} else
		result = OVERWRITE_RESULT_NO;

	dlg_overwrite_data_free (owdata);

	return result;
}


void
dlg_file_rename_series (GthWindow *window,
			GList     *old_names,
			GList     *new_names)
{
	GList          *o_scan, *n_scan;
	GList          *error_list = NULL;
	int             overwrite_result = OVERWRITE_RESULT_UNSPECIFIED;
	gboolean        file_exists, show_ow_all_none;
	gboolean        error = FALSE;
	char	       *last_error_message;

	gth_monitor_pause ();

	last_error_message = g_strdup (_("Unknown error"));

	show_ow_all_none = g_list_length (old_names) > 1;
	for (n_scan = new_names, o_scan = old_names; o_scan && n_scan;) {
		char   *old_full_path = o_scan->data;
		char   *new_full_path = n_scan->data;
		char   *new_name = NULL;
		GError *gerror = NULL;

		if (! path_is_file (old_full_path))
			continue;

		if (same_uri (old_full_path, new_full_path)) {
			o_scan = o_scan->next;
			n_scan = n_scan->next;
			continue;
		}

		/* handle overwrite. */

		new_full_path = g_strdup (new_full_path);
		file_exists = path_is_file (new_full_path);

		if ((overwrite_result != OVERWRITE_RESULT_ALL)
		    && (overwrite_result != OVERWRITE_RESULT_NONE))
			overwrite_result = OVERWRITE_RESULT_UNSPECIFIED;

		while ((overwrite_result == OVERWRITE_RESULT_UNSPECIFIED)
		       && file_exists) {

			overwrite_result = dlg_overwrite_run (window,
							      overwrite_result,
							      old_full_path,
							      new_full_path,
							      show_ow_all_none,
							      &new_name);

			if (overwrite_result == OVERWRITE_RESULT_RENAME) {
				char *parent_dir;

				parent_dir = remove_level_from_path (new_full_path);
				new_full_path = build_uri (parent_dir,  new_name, NULL);
				g_free (parent_dir);
				g_free (new_name);

				overwrite_result = OVERWRITE_RESULT_UNSPECIFIED;
				file_exists = path_is_file (new_full_path);
			}
		}

		if (file_exists
		    && ((overwrite_result == OVERWRITE_RESULT_NONE)
			|| (overwrite_result == OVERWRITE_RESULT_NO))) {
			o_scan = o_scan->next;
			n_scan = n_scan->next;
			g_free (new_full_path);
			continue;
		}

		file_move (old_full_path, new_full_path, TRUE, &gerror);
		if (gerror == NULL) {
			cache_move (old_full_path, new_full_path);
			comment_move (old_full_path, new_full_path);
			gth_monitor_notify_file_renamed (old_full_path, new_full_path);
			o_scan = o_scan->next;
		}
		else {
			old_names = g_list_remove_link (old_names, o_scan);
			error_list = g_list_prepend (error_list, o_scan->data);
			g_list_free (o_scan);
			o_scan = old_names;
			error = TRUE;
			g_free (last_error_message);
			last_error_message = g_strdup (gerror->message);
			g_error_free (gerror);
		}

		g_free (new_full_path);
		n_scan = n_scan->next;
	}

	gth_monitor_resume ();

	if (error) {
		const char *msg;

		if (g_list_length (error_list) == 1)
			msg = _("Could not rename the image:");
		else
			msg = _("Could not rename the following images:");

		dlg_show_error (window,
				msg,
				error_list,
				last_error_message);
	}

	path_list_free (error_list);
	path_list_free (old_names);
	path_list_free (new_names);
	g_free (last_error_message);
}


/* -- dlg_files_copy -- */

typedef struct {
	GList               *file_list;
	GList               *current_file;
	GList               *copied_list;
	GList               *created_list;
	char                *destination;
	gboolean             remove_source;
	gboolean             copy_cache;
	GthWindow           *window;

	FileOpDoneFunc       done_func;
	gpointer             done_data;

	GladeXML            *gui;
	GtkWidget           *progress_dialog;
	GtkWidget           *progress_progressbar;
	GtkWidget           *progress_info;
	GtkWidget           *progress_cancel;

	guint                timeout_id;

	GnomeVFSAsyncHandle *handle;
	GError              *error;
	OverwriteResult      overwrite_result;
	gboolean             cache_copied;
	gboolean             copying_cache;

	int                  file_index;
	int                  files_total;
	gboolean             continue_dialog_visible;
	gboolean             overwrite_dialog_visible;
} FileCopyData;


static void
file_copy_data_free (FileCopyData *fcdata)
{
	if (fcdata == NULL)
		return;

	if (fcdata->timeout_id != 0) {
		g_source_remove (fcdata->timeout_id);
		fcdata->timeout_id = 0;
	}

	gtk_widget_destroy (fcdata->progress_dialog);
	g_object_unref (fcdata->gui);

	path_list_free (fcdata->file_list);
	g_list_free (fcdata->copied_list);
	path_list_free (fcdata->created_list);

	g_free (fcdata->destination);

	if (fcdata->error != NULL)
		g_error_free (fcdata->error);

	g_free (fcdata);
}


static int
file_copy__display_progress_dialog (gpointer data)
{
	FileCopyData *fcdata = data;

	if (fcdata->timeout_id != 0) {
		g_source_remove (fcdata->timeout_id);
		fcdata->timeout_id = 0;
	}

	if (! fcdata->continue_dialog_visible
	    && ! fcdata->overwrite_dialog_visible)
		gtk_widget_show (fcdata->progress_dialog);

	return FALSE;
}


static void copy_next_file (FileCopyData *fcdata);


static void
file_copy__continue_or_abort__response_cb (GtkWidget *dialog,
					   int        response_id,
					   gpointer   data)
{
	FileCopyData *fcdata = data;

	if (response_id == GTK_RESPONSE_YES) {
		gtk_widget_show (fcdata->progress_dialog);
		copy_next_file (fcdata);

	} else {
		if (fcdata->done_func != NULL) {
			GError *error = NULL;
			g_set_error (&error,
			             G_FILE_ERROR,
			             G_FILE_ERROR_INTR,
				     _("Operation interrupted"));
			(*fcdata->done_func) (error, fcdata->done_data);
			g_error_free (error);
		}
		file_copy_data_free (fcdata);
	}

	fcdata->continue_dialog_visible = FALSE;
	gtk_widget_destroy (dialog);
}


static void
continue_or_abort_dialog (FileCopyData   *fcdata)
{
	GtkWidget  *d;
	const char *error;
	const char *src_file;
	char       *message;
	char       *utf8_name;
	gboolean    last_file;

	src_file = fcdata->current_file->data;

	if (fcdata->remove_source)
		error = _("Could not move the image:");
	else
		error = _("Could not copy the image:");
	utf8_name = basename_for_display (src_file);
	message = g_strconcat (error,
			       " ",
			       utf8_name,
			       "\n\n",
			       fcdata->error->message,
			       NULL);
	g_free (utf8_name);

	last_file = fcdata->current_file->next == NULL;

	if (last_file)
		d = _gtk_message_dialog_new (GTK_WINDOW (fcdata->window),
					     GTK_DIALOG_MODAL,
					     GTK_STOCK_DIALOG_ERROR,
					     message,
					     NULL,
					     GTK_STOCK_CLOSE, GTK_RESPONSE_CANCEL,
					     NULL);
	else
		d = _gtk_yesno_dialog_new (GTK_WINDOW (fcdata->window),
					   GTK_DIALOG_MODAL,
					   message,
					   _("_Abort"),
					   _("_Continue"));

	g_free (message);

	g_signal_connect (G_OBJECT (d),
			  "response",
			  G_CALLBACK (file_copy__continue_or_abort__response_cb),
			  fcdata);

	fcdata->continue_dialog_visible = TRUE;

	gtk_widget_hide (fcdata->progress_dialog);
	gtk_widget_show (d);
}


static void
files_copy__done (FileCopyData *fcdata)
{
	if (fcdata->remove_source)
		/* FIXME: use gth_monitor_notify_update_gfiles after gfile migration */
		gth_monitor_notify_update_files (GTH_MONITOR_EVENT_DELETED, fcdata->copied_list);
	/* FIXME: use gth_monitor_notify_update_gfiles after gfile migration */
	gth_monitor_notify_update_files (GTH_MONITOR_EVENT_CREATED, fcdata->created_list);
	gth_monitor_resume ();

	if (fcdata->done_func != NULL)
		(*fcdata->done_func) (fcdata->error, fcdata->done_data);

	file_copy_data_free (fcdata);
}


static int
file_progress_update_cb (GnomeVFSAsyncHandle      *handle,
			 GnomeVFSXferProgressInfo *info,
			 gpointer                  data)
{
	FileCopyData *fcdata = data;

	if (info->status != GNOME_VFS_XFER_PROGRESS_STATUS_OK) {
		set_error_from_vfs_result (info->vfs_status, &(fcdata->error));
		return FALSE;

	} else if (info->phase == GNOME_VFS_XFER_PHASE_COMPLETED) {
		if (fcdata->error == NULL) {
			if (! fcdata->copying_cache) { /* do not notify cache data. */
				char *src_file;
				char *dest_file;

				src_file = fcdata->current_file->data;
				dest_file = build_uri (fcdata->destination,
						       file_name_from_path (src_file),
						       NULL);
				fcdata->copied_list = g_list_prepend (fcdata->copied_list, src_file);
				fcdata->created_list = g_list_prepend (fcdata->created_list, dest_file);
				copy_next_file (fcdata);
			} else
				files_copy__done (fcdata);
		} else {
			set_error_from_vfs_result (info->vfs_status, &(fcdata->error));
			continue_or_abort_dialog (fcdata);
		}
	}

	return TRUE;
}


static void
xfer_file (FileCopyData *fcdata,
	   const char   *src_file,
	   const char   *dest_file)
{
	GnomeVFSURI               *src_uri, *dest_uri;
	GList                     *src_list, *dest_list;
	GnomeVFSXferOptions        xfer_options;
	GnomeVFSXferErrorMode      xfer_error_mode;
	GnomeVFSXferOverwriteMode  overwrite_mode;
	GnomeVFSResult             result;

	src_uri = new_uri_from_path (src_file);
	src_list = g_list_append (NULL, src_uri);

	dest_uri = new_uri_from_path (dest_file);
	dest_list = g_list_append (NULL, dest_uri);

	xfer_options    = fcdata->remove_source ? GNOME_VFS_XFER_REMOVESOURCE : 0;
	xfer_error_mode = GNOME_VFS_XFER_ERROR_MODE_QUERY;
	overwrite_mode  = GNOME_VFS_XFER_OVERWRITE_MODE_REPLACE;

	/* Delete thumbnail from cache, if source file is removed */
	if (fcdata->remove_source)
		delete_thumbnail (src_file);

	result = gnome_vfs_async_xfer (&fcdata->handle,
				       src_list,
				       dest_list,
				       xfer_options,
				       xfer_error_mode,
				       overwrite_mode,
				       GNOME_VFS_PRIORITY_DEFAULT,
				       file_progress_update_cb,
				       fcdata,
				       NULL,
				       NULL);

	gnome_vfs_uri_unref (src_uri);
	g_list_free (src_list);

	gnome_vfs_uri_unref (dest_uri);
	g_list_free (dest_list);

	set_error_from_vfs_result (result, &(fcdata->error));

	/**/

	if (fcdata->error != NULL)
		continue_or_abort_dialog (fcdata);
}


static void
copy_current_file__overwrite (OverwriteResult  result,
			      char            *new_name,
			      gpointer         data)
{
	FileCopyData *fcdata = data;
	const char   *src_file;
	char         *dest_file;

	fcdata->overwrite_dialog_visible = FALSE;
	gtk_widget_show (fcdata->progress_dialog);

	fcdata->overwrite_result = result;

	src_file = fcdata->current_file->data;

	switch (fcdata->overwrite_result) {
	case OVERWRITE_RESULT_RENAME:
		if (new_name == NULL) {
			/* FIXME */
			copy_next_file (fcdata);
			break;
		}

		dest_file = build_uri (fcdata->destination,
				       new_name,
				       NULL);
		xfer_file (fcdata, src_file, dest_file);
		g_free (dest_file);
		g_free (new_name);
		break;

	case OVERWRITE_RESULT_YES:
	case OVERWRITE_RESULT_ALL:
		dest_file = build_uri (fcdata->destination,
				       file_name_from_path (src_file),
				       NULL);
		xfer_file (fcdata, src_file, dest_file);
		g_free (dest_file);
		break;

	case OVERWRITE_RESULT_NO:
	case OVERWRITE_RESULT_NONE:
		copy_next_file (fcdata);
		break;

	default:
		break;
	}
}


static void
copy_current_file (FileCopyData *fcdata)
{
	const char   *src_file;
	char         *dest_file;
	char         *message;
	float         fraction;
	GnomeVFSURI  *dest_uri;
	gboolean      skip = FALSE;

	message = g_strdup_printf (_("Copying file %d of %d"),
				   fcdata->file_index,
				   fcdata->files_total);
	gtk_label_set_text (GTK_LABEL (fcdata->progress_info), message);
	g_free (message);

	fraction = (float) fcdata->file_index / fcdata->files_total;
	gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (fcdata->progress_progressbar),
				       CLAMP (fraction, 0.0, 1.0));

	/**/

	fcdata->error = NULL;

	src_file = fcdata->current_file->data;
	dest_file = build_uri (fcdata->destination,
			       file_name_from_path (src_file),
			       NULL);

	if (same_uri (src_file, dest_file)) {
		g_free (dest_file);
		copy_next_file (fcdata);
		return;
	}

	dest_uri = new_uri_from_path (dest_file);

	if (gnome_vfs_uri_exists (dest_uri)) {
		if (fcdata->overwrite_result == OVERWRITE_RESULT_ALL)
			skip = FALSE;
		else if (fcdata->overwrite_result == OVERWRITE_RESULT_NONE)
			skip = TRUE;
		else {
			GtkWidget *d;
			d = dlg_overwrite (fcdata->window,
					   fcdata->overwrite_result,
					   dest_file,
					   src_file,
					   fcdata->current_file->next != NULL,
					   copy_current_file__overwrite,
					   fcdata);
			gtk_widget_show (d);

			fcdata->overwrite_dialog_visible = TRUE;

			g_free (dest_file);
			gnome_vfs_uri_unref (dest_uri);
			return;
		}
	}

	gnome_vfs_uri_unref (dest_uri);

	if (skip) {
		g_free (dest_file);
		copy_next_file (fcdata);

	} else {
		xfer_file (fcdata, src_file, dest_file);
		g_free (dest_file);
	}
}


static void
copy_next_file (FileCopyData *fcdata)
{
	GList                     *src_list = NULL;
	GList                     *dest_list = NULL;
	GnomeVFSXferOptions        xfer_options;
	GnomeVFSXferErrorMode      xfer_error_mode;
	GnomeVFSXferOverwriteMode  overwrite_mode;
	GnomeVFSResult             result;
	GList                     *scan;

	fcdata->current_file = fcdata->current_file->next;
	fcdata->file_index++;

	if (fcdata->current_file != NULL) {
		copy_current_file (fcdata);
		return;
	}

	if (! fcdata->copy_cache || fcdata->cache_copied) {
		files_copy__done (fcdata);
		return;
	}

	/* copy cache */

	fcdata->copying_cache = TRUE;
	fcdata->cache_copied = TRUE;

	for (scan = fcdata->file_list; scan; scan = scan->next) {
		char *src_file = scan->data;
		char *dest_file;
		char *src_cache_file;
		char *dest_cache_file;

		dest_file = add_filename_to_uri (fcdata->destination,
						 file_name_from_path (src_file));

		src_cache_file = comments_get_comment_filename (src_file, TRUE);
		dest_cache_file = comments_get_comment_filename (dest_file, TRUE);

		if (path_is_file (src_cache_file) && (dest_cache_file != NULL)) {
			char *parent_dir;

			parent_dir = remove_level_from_path (dest_cache_file);
			ensure_dir_exists (parent_dir);
			g_free (parent_dir);

			src_list = g_list_append (src_list, new_uri_from_path (src_cache_file));
			dest_list = g_list_append (dest_list, new_uri_from_path (dest_cache_file));
		}

		g_free (src_cache_file);
		g_free (dest_cache_file);

		/**/

		src_cache_file = cache_get_nautilus_cache_name (src_file);
		dest_cache_file = cache_get_nautilus_cache_name (dest_file);

		if (path_is_file (src_cache_file)) {
			char *parent_dir = remove_level_from_path (dest_cache_file);
			ensure_dir_exists (parent_dir);
			g_free (parent_dir);

			src_list = g_list_append (src_list, new_uri_from_path (src_cache_file));
			dest_list = g_list_append (dest_list, new_uri_from_path (dest_cache_file));
		}

		g_free (src_cache_file);
		g_free (dest_cache_file);

		g_free (dest_file);
	}

	if (src_list == NULL) {
		files_copy__done (fcdata);
		return;
	}

	xfer_options    = fcdata->remove_source ? GNOME_VFS_XFER_REMOVESOURCE : 0;
	xfer_error_mode = GNOME_VFS_XFER_ERROR_MODE_QUERY;
	overwrite_mode  = GNOME_VFS_XFER_OVERWRITE_MODE_REPLACE;

	result = gnome_vfs_async_xfer (&fcdata->handle,
				       src_list,
				       dest_list,
				       xfer_options,
				       xfer_error_mode,
				       overwrite_mode,
				       GNOME_VFS_PRIORITY_DEFAULT,
				       file_progress_update_cb,
				       fcdata,
				       NULL,
				       NULL);

	g_list_foreach (src_list, (GFunc) gnome_vfs_uri_unref, NULL);
	g_list_free (src_list);
	g_list_foreach (dest_list, (GFunc) gnome_vfs_uri_unref, NULL);
	g_list_free (dest_list);

	set_error_from_vfs_result (result, &(fcdata->error));

	if (fcdata->error != NULL)
		files_copy__done (fcdata);
}


static void
dlg_files_copy__interrupt (FileCopyData *fcdata)
{
	if (fcdata->handle != NULL)
		gnome_vfs_async_cancel (fcdata->handle);
	files_copy__done (fcdata);
}


static void
dlg_files_copy__interrupt_cb1 (GtkWidget    *caller,
			       GdkEvent     *event,
			       FileCopyData *fcdata)
{
	dlg_files_copy__interrupt (fcdata);
}


static void
dlg_files_copy__interrupt_cb2 (GtkWidget    *caller,
			       FileCopyData *fcdata)
{
	dlg_files_copy__interrupt (fcdata);
}


void
dlg_files_copy (GthWindow      *window,
		GList          *file_list,
		const char     *dest_path,
		gboolean        remove_source,
		gboolean        copy_cache,
		gboolean        overwrite_all,
		FileOpDoneFunc  done_func,
		gpointer        done_data)
{
	FileCopyData *fcdata;

	if (! path_is_dir (dest_path))
		return;

	if (file_list == NULL)
		return;

	g_return_if_fail (window != NULL);

	fcdata = g_new0 (FileCopyData, 1);

	fcdata->window = window;
	fcdata->file_list = path_list_dup (file_list);
	fcdata->copied_list = NULL;
	fcdata->created_list = NULL;
	fcdata->current_file = fcdata->file_list;
	fcdata->destination = g_strdup (dest_path);
	fcdata->remove_source = remove_source;
	fcdata->copy_cache = copy_cache;
	fcdata->done_func = done_func;
	fcdata->done_data = done_data;

	fcdata->error = NULL;

	if (overwrite_all)
		fcdata->overwrite_result = OVERWRITE_RESULT_ALL;
	else
		fcdata->overwrite_result = OVERWRITE_RESULT_NO;
	fcdata->cache_copied = FALSE;
	fcdata->copying_cache = FALSE;

	fcdata->file_index = 1;
	fcdata->files_total = g_list_length (file_list);
	fcdata->continue_dialog_visible = FALSE;
	fcdata->overwrite_dialog_visible = FALSE;

	/**/

	fcdata->gui = glade_xml_new (GTHUMB_GLADEDIR "/" GLADE_PNG_EXP_FILE, NULL, NULL);
	if (! fcdata->gui) {
		file_copy_data_free (fcdata);
		g_warning ("Could not find " GLADE_PNG_EXP_FILE "\n");
		return;
	}

	fcdata->progress_dialog = glade_xml_get_widget (fcdata->gui, "progress_dialog");
	fcdata->progress_progressbar = glade_xml_get_widget (fcdata->gui, "progress_progressbar");
	fcdata->progress_info = glade_xml_get_widget (fcdata->gui, "progress_info");
	fcdata->progress_cancel = glade_xml_get_widget (fcdata->gui, "progress_cancel");

	g_signal_connect (G_OBJECT (fcdata->progress_dialog),
			  "delete_event",
			  G_CALLBACK (dlg_files_copy__interrupt_cb1),
			  fcdata);
	g_signal_connect (G_OBJECT (fcdata->progress_cancel),
			  "clicked",
			  G_CALLBACK (dlg_files_copy__interrupt_cb2),
			  fcdata);

	gtk_window_set_transient_for (GTK_WINDOW (fcdata->progress_dialog),
				      GTK_WINDOW (fcdata->window));
	gtk_window_set_modal (GTK_WINDOW (fcdata->progress_dialog), FALSE);

	fcdata->timeout_id = g_timeout_add (DISPLAY_PROGRESS_DELAY,
					    file_copy__display_progress_dialog,
					    fcdata);

	gth_monitor_pause ();

	copy_current_file (fcdata);
}

/* -- dlg_files_trash -- */

/* FileDeleteData is used by both trash and delete functions */
typedef struct {
	GladeXML            *gui;
	GtkWidget           *progress_dialog;
	GtkWidget           *progress_progressbar;
	GtkWidget           *progress_info;
	GtkWidget           *progress_cancel;

	GthWindow           *window;
	GList               *fd_list;		/*list of FileData elements*/
	GList		    *gfile_list;	/*list of GFile elements*/
	FileOpDoneFunc       done_func;
	gpointer             done_data;

	glong		     total;		/*Total number of files*/
	glong		     count;		/*Current file*/

	GCancellable	     *cancelled;

	guint                timeout_id;
} FileDeleteData;


static int
file_trash__display_progress_dialog (gpointer data)
{
	FileDeleteData *fddata = data;

	if (fddata->timeout_id != 0) {
		g_source_remove (fddata->timeout_id);
		fddata->timeout_id = 0;
	}

	gtk_widget_show (fddata->progress_dialog);

	return FALSE;
}


static void
file_delete_data_free (FileDeleteData *fddata)
{
	if (fddata == NULL)
		return;

	if (fddata->timeout_id != 0) {
		g_source_remove (fddata->timeout_id);
		fddata->timeout_id = 0;
	}

	gtk_widget_destroy (fddata->progress_dialog);
	g_object_unref (fddata->gui);

	if (fddata->fd_list != NULL)
		file_data_list_free (fddata->fd_list);
	if (fddata->gfile_list != NULL)
		gfile_list_free (fddata->gfile_list);
	g_object_unref (fddata->cancelled);

	g_free (fddata);
}


static void
file_trash_progress_update (gpointer                  data)
{
	FileDeleteData *fddata = data;
	char 		*message;
	float            fraction;
	message = g_strdup_printf (_("Moving file %ld of %ld to trash"),
					   fddata->count,
					   fddata->total);

   	if (message != NULL) {
		gtk_label_set_text (GTK_LABEL (fddata->progress_info), message);
		g_free (message);
	}

	if (fddata->count != 0) {
		fraction = (float)fddata->count / fddata->total;
		gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (fddata->progress_progressbar),
					       CLAMP (fraction, 0.0, 1.0));
	}
}


static void
files_trash__done (FileDeleteData *fddata)
{
	if (fddata->gfile_list != NULL)
		gth_monitor_notify_update_gfiles (GTH_MONITOR_EVENT_DELETED, fddata->gfile_list);
	/*gth_monitor_resume (); FIXME*/
	file_delete_data_free (fddata);
}




static void
dlg_files_trash__interrupt (FileDeleteData *fddata)
{
	g_cancellable_cancel (fddata->cancelled);
}


static void
dlg_files_trash__interrupt_cb1 (GtkWidget      *caller,
				 GdkEvent       *event,
				 FileDeleteData *fcdata)
{
	dlg_files_trash__interrupt (fcdata);
}


static void
dlg_files_trash__interrupt_cb2 (GtkWidget      *caller,
				 FileDeleteData *fcdata)
{
	dlg_files_trash__interrupt (fcdata);
}


static gboolean
trash_next_file (gpointer data)
{
	FileDeleteData *fddata = (FileDeleteData*)data;
	GList    *link;
	FileData *fd;
	GFile    *gfile;
	GError   *error = NULL;

	file_trash_progress_update (fddata);

	if (fddata->fd_list == NULL || g_cancellable_is_cancelled (fddata->cancelled))
	{
		ConfirmFileDeleteData *cfddata;
		cfddata = (ConfirmFileDeleteData*)fddata->done_data;
		file_data_list_free (cfddata->file_list);
		cfddata->file_list = NULL;
		(*fddata->done_func) (error, fddata->done_data);
		files_trash__done (fddata);
		return FALSE;
	}

	link = fddata->fd_list;
	fd = (FileData*)link->data;
	gfile = fd->gfile;
	fddata->fd_list = g_list_remove_link (fddata->fd_list, link);

	g_file_trash (gfile, NULL, &error);
	if (error == NULL) {
		fddata->gfile_list = g_list_append (fddata->gfile_list, gfile);
		g_object_ref (gfile);
		file_data_unref (fd);
		g_list_free (link);
		fddata->count++;
		return TRUE;
	}
	else {
		(*fddata->done_func) (error, fddata->done_data);
		files_trash__done (fddata);
		g_error_free (error);
		error = NULL;
		file_data_unref (fd);
		g_list_free (link);
		return FALSE;
	}
	return TRUE;
}


void
dlg_files_move_to_trash (GthWindow *window,
		  GList		 *file_data_list,
		  FileOpDoneFunc  done_func,
		  gpointer        done_data)
{
	FileDeleteData            *fddata;
	g_return_if_fail (file_data_list != NULL);
	fddata = g_new0 (FileDeleteData, 1);

	fddata->window = window;

	fddata->fd_list = file_data_list_dup (file_data_list);
	fddata->gfile_list = NULL;

	fddata->done_func = done_func;
	fddata->done_data = done_data;

	fddata->total = g_list_length (fddata->fd_list);
	fddata->count = 0;

	fddata->cancelled = g_cancellable_new();

	fddata->gui = glade_xml_new (GTHUMB_GLADEDIR "/" GLADE_PNG_EXP_FILE, NULL, NULL);
	if (! fddata->gui) {
		file_delete_data_free (fddata);
		g_warning ("Could not find " GLADE_PNG_EXP_FILE "\n");
		return;
	}

	fddata->progress_dialog = glade_xml_get_widget (fddata->gui, "progress_dialog");
	fddata->progress_progressbar = glade_xml_get_widget (fddata->gui, "progress_progressbar");
	fddata->progress_info = glade_xml_get_widget (fddata->gui, "progress_info");
	fddata->progress_cancel = glade_xml_get_widget (fddata->gui, "progress_cancel");

	g_signal_connect (G_OBJECT (fddata->progress_dialog),
			  "delete_event",
			  G_CALLBACK (dlg_files_trash__interrupt_cb1),
			  fddata);
	g_signal_connect (G_OBJECT (fddata->progress_cancel),
			  "clicked",
			  G_CALLBACK (dlg_files_trash__interrupt_cb2),
			  fddata);

	gtk_window_set_transient_for (GTK_WINDOW (fddata->progress_dialog),
				      GTK_WINDOW (fddata->window));
	gtk_window_set_modal (GTK_WINDOW (fddata->progress_dialog), FALSE);


	/* It seems the only way to test if the Trash is usable is to try and trash a file */
	GError *error=NULL;
	GList *link = fddata->fd_list;
	FileData *fd = (FileData*)link->data;
	GFile *gfile = fd->gfile;
	fddata->fd_list = g_list_remove_link (fddata->fd_list, link);

	g_file_trash (gfile, NULL, &error);
	if (error == NULL) {
		fddata->gfile_list = g_list_append (NULL, gfile);
		fddata->count++;
		file_data_unref (fd);
		g_list_free (link);
	}
	else if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED)) {
		g_error_free (error);
		error = NULL;
		(*fddata->done_func) (NULL, fddata->done_data);
		file_data_unref (fd);
		g_list_free (link);
		files_trash__done (fddata);
		return;
	}
	else {
		(*fddata->done_func) (error, fddata->done_data);
		g_error_free (error);
		error = NULL;
		file_data_unref (fd);
		g_list_free (link);
		files_trash__done (fddata);
		return;
	}

	fddata->timeout_id = g_timeout_add (DISPLAY_PROGRESS_DELAY,
					    file_trash__display_progress_dialog,
					    fddata);
	g_idle_add (trash_next_file, fddata);
}


/* -- dlg_files_delete -- */


static int
file_delete__display_progress_dialog (gpointer data)
{
	FileDeleteData *fddata = data;

	if (fddata->timeout_id != 0) {
		g_source_remove (fddata->timeout_id);
		fddata->timeout_id = 0;
	}

	gtk_widget_show (fddata->progress_dialog);

	return FALSE;
}


static void
files_delete__done (FileDeleteData *fddata)
{
	if (fddata->gfile_list != NULL)
		gth_monitor_notify_update_gfiles (GTH_MONITOR_EVENT_DELETED, fddata->gfile_list);
	/*gth_monitor_resume (); FIXME*/
	file_delete_data_free (fddata);
}


static void
file_delete_progress_update_gfile (gpointer                  data)
{
	FileDeleteData *fddata = data;
	char 		*message;
	float            fraction;
	message = g_strdup_printf (_("Deleting file %ld of %ld"),
					   fddata->count,
					   fddata->total);

   	if (message != NULL) {
		gtk_label_set_text (GTK_LABEL (fddata->progress_info), message);
		g_free (message);
	}

	if (fddata->count != 0) {
		fraction = (float)fddata->count / fddata->total;
		gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (fddata->progress_progressbar),
					       CLAMP (fraction, 0.0, 1.0));
	}
}


static void
dlg_files_delete__interrupt (FileDeleteData *fddata)
{
	g_cancellable_cancel (fddata->cancelled);
}


static void
dlg_files_delete__interrupt_cb1 (GtkWidget      *caller,
				 GdkEvent       *event,
				 FileDeleteData *fcdata)
{
	dlg_files_delete__interrupt (fcdata);
}


static void
dlg_files_delete__interrupt_cb2 (GtkWidget      *caller,
				 FileDeleteData *fcdata)
{
	dlg_files_delete__interrupt (fcdata);
}


static gboolean
delete_next_file (gpointer data)
{
	FileDeleteData *fddata = (FileDeleteData*)data;
	GList    *link;
	FileData *fd;
	GFile    *gfile;
	GError   *error = NULL;

	file_delete_progress_update_gfile (fddata);

	if (fddata->fd_list == NULL || g_cancellable_is_cancelled (fddata->cancelled)) {
		(*fddata->done_func) (NULL, fddata->done_data);
		files_delete__done (fddata);
		return FALSE;
	}

	link = fddata->fd_list;
	fd = (FileData*)link->data;
	gfile = fd->gfile;
	fddata->fd_list = g_list_remove_link (fddata->fd_list, link);

	g_file_delete (gfile, NULL, &error);
	if (error == NULL) {
		fddata->gfile_list = g_list_append (fddata->gfile_list, gfile);
		g_object_ref (gfile);
		fddata->count++;
		file_data_unref (fd);
		g_list_free (link);
		return TRUE;
	} else {
		(*fddata->done_func) (error, fddata->done_data);
		files_delete__done (fddata);
		g_error_free (error);
		error = NULL;
		file_data_unref (fd);
		g_list_free (link);
		return FALSE;
	}
	return TRUE;
}


void
dlg_files_delete (GthWindow *window,
		  GList		 *file_data_list,
		  FileOpDoneFunc  done_func,
		  gpointer        done_data)
{
	FileDeleteData            *fddata;
	g_return_if_fail (file_data_list != NULL);
	fddata = g_new0 (FileDeleteData, 1);

	fddata->window = window;

	fddata->fd_list = file_data_list_dup (file_data_list);
	fddata->gfile_list = NULL;

	fddata->done_func = done_func;
	fddata->done_data = done_data;

	fddata->total = g_list_length (fddata->fd_list);
	fddata->count = 0;

	fddata->cancelled = g_cancellable_new();

	fddata->gui = glade_xml_new (GTHUMB_GLADEDIR "/" GLADE_PNG_EXP_FILE, NULL, NULL);
	if (! fddata->gui) {
		file_delete_data_free (fddata);
		g_warning ("Could not find " GLADE_PNG_EXP_FILE "\n");
		return;
	}

	fddata->progress_dialog = glade_xml_get_widget (fddata->gui, "progress_dialog");
	fddata->progress_progressbar = glade_xml_get_widget (fddata->gui, "progress_progressbar");
	fddata->progress_info = glade_xml_get_widget (fddata->gui, "progress_info");
	fddata->progress_cancel = glade_xml_get_widget (fddata->gui, "progress_cancel");

	g_signal_connect (G_OBJECT (fddata->progress_dialog),
			  "delete_event",
			  G_CALLBACK (dlg_files_delete__interrupt_cb1),
			  fddata);
	g_signal_connect (G_OBJECT (fddata->progress_cancel),
			  "clicked",
			  G_CALLBACK (dlg_files_delete__interrupt_cb2),
			  fddata);

	gtk_window_set_transient_for (GTK_WINDOW (fddata->progress_dialog),
				      GTK_WINDOW (fddata->window));
	gtk_window_set_modal (GTK_WINDOW (fddata->progress_dialog), FALSE);

	fddata->timeout_id = g_timeout_add (DISPLAY_PROGRESS_DELAY,
					    file_delete__display_progress_dialog,
					    fddata);

	g_idle_add (delete_next_file, fddata);
}


/* -- dlg_folder_copy -- */


typedef struct {
	char                *source;
	char                *destination;
	FileOp               file_op;
	gboolean             include_cache;
	GthWindow           *window;

	FileOpDoneFunc       done_func;
	gpointer             done_data;

	guint                timeout_id;

	GladeXML            *gui;
	GtkWidget           *progress_dialog;
	GtkWidget           *progress_progressbar;
	GtkWidget           *progress_info;
	GtkWidget           *progress_cancel;

	GnomeVFSAsyncHandle *handle;
	GError              *error;
} FolderCopyData;


static void
destroy_progress_dialog (FolderCopyData *fcdata)
{
	if (fcdata->timeout_id != 0) {
		g_source_remove (fcdata->timeout_id);
		fcdata->timeout_id = 0;
	}

	if (fcdata->progress_dialog != NULL) {
		gtk_widget_destroy (fcdata->progress_dialog);
		fcdata->progress_dialog = NULL;
	}
}


static void
folder_copy_data_free (FolderCopyData *fcdata)
{
	gth_monitor_resume ();

	if (fcdata == NULL)
		return;

	destroy_progress_dialog (fcdata);
	g_object_unref (fcdata->gui);
	g_free (fcdata->source);
	g_free (fcdata->destination);

	if (fcdata->error != NULL)
		g_error_free (fcdata->error);

	g_free (fcdata);
}


static int
folder_progress_update_cb (GnomeVFSAsyncHandle      *handle,
			   GnomeVFSXferProgressInfo *info,
			   gpointer                  data)
{
	FolderCopyData *fcdata = data;
	int             result = TRUE;
	char           *message = NULL;
	float           fraction;

	if (info->status != GNOME_VFS_XFER_PROGRESS_STATUS_OK) {
		set_error_from_vfs_result (info->vfs_status, &(fcdata->error));
		return FALSE;
	}
	else if (info->phase == GNOME_VFS_XFER_PHASE_INITIAL) {
		/**/
	}
	else if (info->phase == GNOME_VFS_XFER_PHASE_COLLECTING) {
		message = g_strdup (_("Collecting images info"));
	}
	else if (info->phase == GNOME_VFS_XFER_PHASE_COPYING) {
		message = g_strdup_printf (_("Copying file %ld of %ld"),
					   info->file_index,
					   info->files_total);
	}
	else if (info->phase == GNOME_VFS_XFER_PHASE_MOVING) {
		message = g_strdup_printf (_("Moving file %ld of %ld"),
					   info->file_index,
					   info->files_total);
	}
	else if (info->phase == GNOME_VFS_XFER_PHASE_DELETESOURCE) {
		message = g_strdup_printf (_("Deleting file %ld of %ld"),
					   info->file_index,
					   info->files_total);
	}
	else if (info->phase == GNOME_VFS_XFER_PHASE_COMPLETED) {

		if (fcdata->error == NULL) {
			if (fcdata->file_op == FILE_OP_COPY)
                                gth_monitor_notify_update_directory (fcdata->destination, GTH_MONITOR_EVENT_CREATED);

			else if (fcdata->file_op == FILE_OP_MOVE)
				gth_monitor_notify_directory_renamed (fcdata->source,
								     fcdata->destination);
			else if (fcdata->file_op == FILE_OP_DELETE)
				gth_monitor_notify_update_directory (fcdata->source, GTH_MONITOR_EVENT_DELETED);
		}

		destroy_progress_dialog (fcdata);

		if (fcdata->done_func != NULL)
			(*fcdata->done_func) (fcdata->error, fcdata->done_data);
		folder_copy_data_free (fcdata);

		return result;
	}

	if (message != NULL) {
		gtk_label_set_text (GTK_LABEL (fcdata->progress_info), message);
		g_free (message);
	}

	if (info->files_total != 0) {
		fraction = (float) info->file_index / info->files_total;
		gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (fcdata->progress_progressbar),
					       CLAMP (fraction, 0.0, 1.0));
	}

	return result;
}


static int
folder_progress_sync_cb (GnomeVFSXferProgressInfo *info,
			 gpointer                  data)
{
	int ret_val = TRUE;

	if (info->status == GNOME_VFS_XFER_PROGRESS_STATUS_OK) {
		ret_val = TRUE;

	} else if (info->status == GNOME_VFS_XFER_PROGRESS_STATUS_VFSERROR) {
		ret_val = GNOME_VFS_XFER_ERROR_ACTION_SKIP;

	} else if (info->status == GNOME_VFS_XFER_PROGRESS_STATUS_OVERWRITE) {
		ret_val = GNOME_VFS_XFER_OVERWRITE_ACTION_REPLACE_ALL;
	}

	return ret_val;
}


static int
folder_copy__display_progress_dialog (gpointer data)
{
	FolderCopyData *fcdata = data;

	if (fcdata->timeout_id != 0) {
		g_source_remove (fcdata->timeout_id);
		fcdata->timeout_id = 0;
	}

	gtk_widget_show (fcdata->progress_dialog);

	return FALSE;
}


static void
folder_copy__interrupt (FolderCopyData *fcdata)
{
	if (fcdata->handle != NULL)
		gnome_vfs_async_cancel (fcdata->handle);
	folder_copy_data_free (fcdata);
}


static void
folder_copy__interrupt_cb1 (GtkWidget     *caller,
			    GdkEvent       *event,
			    FolderCopyData *fcdata)
{
	folder_copy__interrupt (fcdata);
}


static void
folder_copy__interrupt_cb2 (GtkWidget      *caller,
			    FolderCopyData *fcdata)
{
	folder_copy__interrupt (fcdata);
}


static void
folder_copy (GthWindow      *window,
	     const char     *src_path,
	     const char     *dest_path,
	     FileOp          file_op,
	     gboolean        include_cache,
	     gboolean        overwrite_all,
	     FileOpDoneFunc  done_func,
	     gpointer        done_data)
{
	FolderCopyData            *fcdata;
	GList                     *src_list = NULL;
	GList                     *dest_list = NULL;
	GnomeVFSXferOptions        xfer_options;
	GnomeVFSXferErrorMode      xfer_error_mode;
	GnomeVFSXferOverwriteMode  overwrite_mode;
	GnomeVFSResult             result;

	if (file_op != FILE_OP_DELETE) {
		if ((src_path == NULL) || (dest_path == NULL))
			return;
		if (path_in_path (src_path, dest_path))
			return;
	}

	if (! path_is_dir (src_path))
		return;

	fcdata = g_new0 (FolderCopyData, 1);

	fcdata->window = window;
	fcdata->source = g_strdup (src_path);
	fcdata->destination = g_strdup (dest_path);
	fcdata->file_op = file_op;
	fcdata->include_cache = include_cache;
	fcdata->done_func = done_func;
	fcdata->done_data = done_data;

	fcdata->error = NULL;

	/**/

	fcdata->gui = glade_xml_new (GTHUMB_GLADEDIR "/" GLADE_PNG_EXP_FILE, NULL, NULL);
	if (! fcdata->gui) {
		folder_copy_data_free (fcdata);
		g_warning ("Could not find " GLADE_PNG_EXP_FILE "\n");
		return;
	}

	fcdata->progress_dialog = glade_xml_get_widget (fcdata->gui, "progress_dialog");
	fcdata->progress_progressbar = glade_xml_get_widget (fcdata->gui, "progress_progressbar");
	fcdata->progress_info = glade_xml_get_widget (fcdata->gui, "progress_info");
	fcdata->progress_cancel = glade_xml_get_widget (fcdata->gui, "progress_cancel");

	g_signal_connect (G_OBJECT (fcdata->progress_dialog),
			  "delete_event",
			  G_CALLBACK (folder_copy__interrupt_cb1),
			  fcdata);
	g_signal_connect (G_OBJECT (fcdata->progress_cancel),
			  "clicked",
			  G_CALLBACK (folder_copy__interrupt_cb2),
			  fcdata);

	gtk_window_set_transient_for (GTK_WINDOW (fcdata->progress_dialog),
				      GTK_WINDOW (fcdata->window));
	gtk_window_set_modal (GTK_WINDOW (fcdata->progress_dialog), FALSE);

	fcdata->timeout_id = g_timeout_add (DISPLAY_PROGRESS_DELAY,
					    folder_copy__display_progress_dialog,
					    fcdata);

	/**/

	gth_monitor_pause ();

	src_list = g_list_append (src_list, new_uri_from_path (src_path));

	if (fcdata->file_op != FILE_OP_DELETE)
		dest_list = g_list_append (dest_list, new_uri_from_path (dest_path));

	if (fcdata->include_cache) {
		char *src_folder_comment;

		src_folder_comment = comments_get_comment_filename (src_path, TRUE);

		if (path_is_file (src_folder_comment)) {
			char *dest_folder_comment;
			dest_folder_comment = comments_get_comment_filename (dest_path, TRUE);
			src_list = g_list_append (src_list, new_uri_from_path (src_folder_comment));
			if (fcdata->file_op != FILE_OP_DELETE) {
				if (path_is_file (dest_folder_comment))
					file_unlink (dest_folder_comment);
				dest_list = g_list_append (dest_list, new_uri_from_path (dest_folder_comment));
			}
			g_free (dest_folder_comment);
		}

		g_free (src_folder_comment);
	}

	xfer_options = GNOME_VFS_XFER_RECURSIVE;

	if (fcdata->file_op == FILE_OP_MOVE)
		xfer_options |= GNOME_VFS_XFER_REMOVESOURCE;
	else if (fcdata->file_op == FILE_OP_DELETE)
		xfer_options |= GNOME_VFS_XFER_DELETE_ITEMS;

	xfer_error_mode = GNOME_VFS_XFER_ERROR_MODE_QUERY;
	if (overwrite_all)
		overwrite_mode = GNOME_VFS_XFER_OVERWRITE_MODE_REPLACE;
	else
		overwrite_mode = GNOME_VFS_XFER_OVERWRITE_MODE_QUERY;

	result = gnome_vfs_async_xfer (&fcdata->handle,
				       src_list,
				       dest_list,
				       xfer_options,
				       xfer_error_mode,
				       overwrite_mode,
				       GNOME_VFS_PRIORITY_DEFAULT,
				       folder_progress_update_cb,
				       fcdata,
				       folder_progress_sync_cb,
				       fcdata);

	g_list_foreach (src_list, (GFunc) gnome_vfs_uri_unref, NULL);
	g_list_free (src_list);
	g_list_foreach (dest_list, (GFunc) gnome_vfs_uri_unref, NULL);
	g_list_free (dest_list);

	/**/

	if (result != GNOME_VFS_OK) {
		g_assert (result == GNOME_VFS_ERROR_BAD_PARAMETERS);
		if (done_func != NULL) {
			GError *error = NULL;
			g_set_error (&error,
				     G_IO_ERROR,
				     G_IO_ERROR_INVALID_ARGUMENT,
				     _("Invalid argument"));
			(*done_func) (error, done_data);
			g_error_free (error);
		}
		folder_copy_data_free (fcdata);
	}
}


void
dlg_folder_copy (GthWindow      *window,
		 const char     *src_path,
		 const char     *dest_path,
		 gboolean        remove_source,
		 gboolean        include_cache,
		 gboolean        overwrite_all,
		 FileOpDoneFunc  done_func,
		 gpointer        done_data)
{
	folder_copy (window,
		     src_path,
		     dest_path,
		     remove_source ? FILE_OP_MOVE : FILE_OP_COPY,
		     include_cache,
		     overwrite_all,
		     done_func,
		     done_data);
}


void
dlg_folder_move_to_trash (GthWindow      *window,
			  const char     *folder,
			  FileOpDoneFunc  done_func,
			  gpointer        done_data)
{
	char            *parent_dir;
	GnomeVFSURI     *parent_uri, *trash_uri = NULL;

	parent_dir = remove_level_from_path (folder);
	parent_uri = new_uri_from_path (parent_dir);

	gnome_vfs_find_directory (parent_uri,
				  GNOME_VFS_DIRECTORY_KIND_TRASH,
				  &trash_uri,
				  TRUE,
				  TRUE,
				  0777);

	if (trash_uri != NULL) {
		char *trash_path;
		char *dest_folder;

		trash_path = gnome_vfs_uri_to_string (trash_uri, GNOME_VFS_URI_HIDE_NONE);
		dest_folder = g_strconcat (trash_path, "/", file_name_from_path (folder), NULL);

		folder_copy (window,
			     folder,
			     dest_folder,
			     FILE_OP_MOVE,
			     FALSE,
			     TRUE,
			     done_func,
			     done_data);

		g_free (trash_path);
		g_free (dest_folder);
		gnome_vfs_uri_unref (trash_uri);

	} else if (done_func != NULL) {
		GError *error = NULL;
		g_set_error (&error,
		             G_FILE_ERROR,
		             G_FILE_ERROR_NOENT,
		             _("Could not find the trash folder"));
		(*done_func) (error, done_data);
		g_error_free (error);
	}

	g_free (parent_dir);
	gnome_vfs_uri_unref (parent_uri);
}


void
dlg_folder_delete (GthWindow      *window,
		   const char     *folder,
		   FileOpDoneFunc  done_func,
		   gpointer        done_data)
{
	folder_copy (window,
		     folder,
		     NULL,
		     FILE_OP_DELETE,
		     TRUE,
		     FALSE,
		     done_func,
		     done_data);
}


/* -- dlg_copy_items -- */


typedef struct {
	GthWindow      *window;
	GList          *item_list;
	GList          *current_item;
	GList          *dir_list;
	GList          *file_list;
	char           *destination;
	gboolean        remove_source;
	gboolean        include_cache;
	gboolean        overwrite_all;
	FileOpDoneFunc  done_func;
	gpointer        done_data;
} CopyItemsData;


static void
copy_items_data_free (CopyItemsData *cidata)
{
	path_list_free (cidata->item_list);
	g_list_free (cidata->dir_list);
	g_list_free (cidata->file_list);
	g_free (cidata->destination);
	g_free (cidata);
}


static void
copy_item__continue2 (GError	    *error,
		      gpointer       data)
{
	CopyItemsData *cidata = data;

	if (cidata->done_func != NULL)
		(cidata->done_func) (NULL, cidata->done_data);
	copy_items_data_free (cidata);
}


static void copy_current_item (CopyItemsData *cidata);


static void
item_copy__continue_or_abort__response_cb (GtkWidget *dialog,
					   int        response_id,
					   gpointer   data)
{
	CopyItemsData *cidata = data;

	if (response_id == GTK_RESPONSE_YES) {
		cidata->current_item = cidata->current_item->next;
		copy_current_item (cidata);
	} else {
		if (cidata->done_func != NULL)
			(cidata->done_func) (NULL, cidata->done_data);
		copy_items_data_free (cidata);
	}

	gtk_widget_destroy (dialog);
}


static void
copy_item__continue1 (GError 	    *error,
		      gpointer       data)
{
	CopyItemsData *cidata = data;

	if (error) {
		gboolean    last_item = cidata->current_item->next == NULL;
		const char *format;
		char       *folder = cidata->current_item->data;
		char       *message;
		char       *utf8_name;
		GtkWidget  *d;

		if (cidata->remove_source)
			format = _("Could not move the folder \"%s\": %s");
		else
			format = _("Could not move the folder \"%s\": %s");

		utf8_name = basename_for_display (folder);
		message = g_strdup_printf (format,
					   utf8_name,
					   error->message,
					   NULL);
		g_free (utf8_name);

		if (last_item)
			d = _gtk_message_dialog_new (GTK_WINDOW (cidata->window),
						     GTK_DIALOG_MODAL,
						     GTK_STOCK_DIALOG_ERROR,
						     message,
						     NULL,
						     GTK_STOCK_CLOSE, GTK_RESPONSE_CANCEL,
						     NULL);
		else
			d = _gtk_yesno_dialog_new (GTK_WINDOW (cidata->window),
						   GTK_DIALOG_MODAL,
						   message,
						   _("_Abort"),
						   _("_Continue"));

		g_free (message);

		g_signal_connect (G_OBJECT (d),
				  "response",
				  G_CALLBACK (item_copy__continue_or_abort__response_cb),
				  cidata);

		gtk_widget_show (d);

	} else {
		cidata->current_item = cidata->current_item->next;
		copy_current_item (cidata);
	}
}


static void
copy_current_item (CopyItemsData *cidata)
{
	if (cidata->current_item == NULL) {
		if (cidata->file_list == NULL) {
			if (cidata->done_func != NULL)
				(cidata->done_func) (NULL, cidata->done_data);
			copy_items_data_free (cidata);
		} else
			dlg_files_copy (cidata->window,
					cidata->file_list,
					cidata->destination,
					cidata->remove_source,
					cidata->include_cache,
					cidata->overwrite_all,
					copy_item__continue2,
					cidata);

	} else {
		const char *src_path;
		char       *dest_path;

		src_path = cidata->current_item->data;
		dest_path = g_strconcat (cidata->destination,
					 "/",
					 file_name_from_path (src_path),
					 NULL);

		dlg_folder_copy (cidata->window,
				 src_path,
				 dest_path,
				 cidata->remove_source,
				 cidata->include_cache,
				 cidata->overwrite_all,
				 copy_item__continue1,
				 cidata);

		g_free (dest_path);
	}
}


void
dlg_copy_items (GthWindow      *window,
		GList          *item_list,
		const char     *destination,
		gboolean        remove_source,
		gboolean        include_cache,
		gboolean        overwrite_all,
		FileOpDoneFunc  done_func,
		gpointer        done_data)
{
	GList         *scan;
	CopyItemsData *cidata;

	g_return_if_fail (item_list != NULL);
	if (! path_is_dir (destination))
		return;

	cidata = g_new0 (CopyItemsData, 1);

	cidata->window = window;
	cidata->done_func = done_func;
	cidata->done_data = done_data;
	cidata->item_list = path_list_dup (item_list);
	cidata->destination = g_strdup (destination);
	cidata->remove_source = remove_source;
	cidata->include_cache = include_cache;
	cidata->overwrite_all = overwrite_all;

	for (scan = cidata->item_list; scan; scan = scan->next) {
		char *path = scan->data;

		if (path_is_dir (path))
			cidata->dir_list = g_list_prepend (cidata->dir_list, path);
		else if (path_is_file (path))
			cidata->file_list = g_list_prepend (cidata->file_list, path);
	}

	if ((cidata->dir_list == NULL) && (cidata->file_list == NULL)) {
		if (done_func != NULL)
			(done_func) (NULL, done_data);
		copy_items_data_free (cidata);
		return;
	}

	cidata->current_item = cidata->dir_list;
	copy_current_item (cidata);
}