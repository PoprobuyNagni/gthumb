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
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Street #330, Boston, MA 02111-1307, USA.
 */


#include <config.h>
#include <glib/gi18n.h>
#include <glib-object.h>
#include <gthumb.h>
#include "actions.h"
#include "callbacks.h"
#include "gth-script-file.h"
#include "gth-script-task.h"


#define BROWSER_DATA_KEY "list-tools-browser-data"


static const char *fixed_ui_info =
"<ui>"
/*
"  <popup name='FileListPopup'>"
"    <placeholder name='Open_Actions'>"
"      <menu name='ExecWith' action='ExecWithMenu'>"
"        <placeholder name='Tools'/>"
"        <placeholder name='Scripts'/>"
"        <separator name='ScriptsListSeparator'/>"
"        <menuitem name='EditScripts' action='ListTools_EditScripts'/>"
"      </menu>"
"    </placeholder>"
"  </popup>"
*/
"  <popup name='ListToolsPopup'>"
"    <placeholder name='Tools'/>"
"    <placeholder name='Scripts'/>"
"    <separator name='ScriptsListSeparator'/>"
"    <menuitem name='EditScripts' action='ListTools_EditScripts'/>"
"  </popup>"
"</ui>";


static GtkActionEntry action_entries[] = {
	/*{ "ExecWithMenu", GTK_STOCK_EXECUTE, N_("_Tools") },*/

	{ "ListTools_EditScripts", GTK_STOCK_EDIT,
	  N_("Personalize..."), NULL,
	  NULL,
	  G_CALLBACK (gth_browser_action_list_tools_edit_scripts) }
};


typedef struct {
	GthBrowser     *browser;
	GtkToolItem    *tool_item;
	GtkActionGroup *action_group;
	gulong          scripts_changed_id;
} BrowserData;


static void
browser_data_free (BrowserData *data)
{
	g_signal_handler_disconnect (gth_script_file_get (), data->scripts_changed_id);
	g_free (data);
}


static void
activate_script_menu_item (GtkMenuItem *menuitem,
			   gpointer     user_data)
{
	BrowserData *data = user_data;
	GthScript   *script;

	script = gth_script_file_get_script (gth_script_file_get (), g_object_get_data (G_OBJECT (menuitem), "script_id"));
	if (script != NULL) {
		GList *items;
		GList *file_list;

		items = gth_file_selection_get_selected (GTH_FILE_SELECTION (gth_browser_get_file_list_view (data->browser)));
		file_list = gth_file_list_get_files (GTH_FILE_LIST (gth_browser_get_file_list (data->browser)), items);
		if (file_list != NULL) {
			GthTask *task;

			task = gth_script_task_new (GTK_WINDOW (data->browser), script, file_list);
			gth_browser_exec_task (data->browser, task, FALSE);

			g_object_unref (task);
		}

		_g_object_list_unref (file_list);
		_gtk_tree_path_list_free (items);
	}
}


static GtkWidget *
get_widget_with_prefix (BrowserData *data,
			const char  *prefix,
			const char  *path)
{
	char      *full_path;
	GtkWidget *widget;

	full_path = g_strconcat (prefix, path, NULL);
	widget = gtk_ui_manager_get_widget (gth_browser_get_ui_manager (data->browser), full_path);

	g_free (full_path);

	return widget;
}


static void
_update_scripts_menu (BrowserData *data,
		      const char  *prefix)
{
	GtkWidget *separator1;
	GtkWidget *separator2;
	GtkWidget *menu;
	GList     *script_list;
	GList     *scan;
	int        pos;
	gboolean   script_present = FALSE;

	separator1 = get_widget_with_prefix (data, prefix, "/Tools");
	separator2 = get_widget_with_prefix (data, prefix, "/Scripts");
	menu = gtk_widget_get_parent (separator1);
	_gtk_container_remove_children (GTK_CONTAINER (menu), separator1, separator2);

	script_list = gth_script_file_get_scripts (gth_script_file_get ());
	pos = _gtk_container_get_pos (GTK_CONTAINER (menu), separator2);
	for (scan = script_list; scan; scan = scan->next) {
		GthScript *script = scan->data;
		GtkWidget *menu_item;

		if (! gth_script_is_visible (script))
			continue;

		menu_item = gtk_image_menu_item_new_with_label (gth_script_get_display_name (script));
		gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (menu_item), gtk_image_new_from_stock (GTK_STOCK_EXECUTE, GTK_ICON_SIZE_MENU));
		gtk_widget_show (menu_item);
		gtk_menu_shell_insert (GTK_MENU_SHELL (menu), menu_item, pos++);

		g_object_set_data_full (G_OBJECT (menu_item),
					"script_id",
					g_strdup (gth_script_get_id (script)),
					(GDestroyNotify) g_free);
		g_signal_connect (menu_item,
				  "activate",
				  G_CALLBACK (activate_script_menu_item),
			  	  data);

		script_present = TRUE;
	}

	separator1 = get_widget_with_prefix (data, prefix, "/ScriptsListSeparator");
	if (script_present)
		gtk_widget_show (separator1);
	else
		gtk_widget_hide (separator1);

	list_tools__gth_browser_update_sensitivity_cb (data->browser);

	_g_object_list_unref (script_list);
}


static void
update_scripts_menu (BrowserData *data)
{
	_update_scripts_menu (data, "/ListToolsPopup");
	/*_update_scripts_menu (data, "/FileListPopup/Open_Actions/ExecWith");*/
}


static void
scripts_changed_cb (GthScriptFile *script_file,
		     BrowserData   *data)
{
	update_scripts_menu (data);
}


void
list_tools__gth_browser_construct_cb (GthBrowser *browser)
{
	BrowserData *data;
	GError      *error = NULL;

	g_return_if_fail (GTH_IS_BROWSER (browser));

	data = g_new0 (BrowserData, 1);
	data->browser = browser;
	data->action_group = gtk_action_group_new ("List Tools Manager Actions");
	gtk_action_group_set_translation_domain (data->action_group, NULL);
	gtk_action_group_add_actions (data->action_group,
				      action_entries,
				      G_N_ELEMENTS (action_entries),
				      browser);
	gtk_ui_manager_insert_action_group (gth_browser_get_ui_manager (browser), data->action_group, 0);

	if (! gtk_ui_manager_add_ui_from_string (gth_browser_get_ui_manager (browser), fixed_ui_info, -1, &error)) {
		g_message ("building menus failed: %s", error->message);
		g_clear_error (&error);
	}

	/* tools menu button */

	data->tool_item = gth_toggle_menu_tool_button_new ();
	gtk_tool_button_set_label (GTK_TOOL_BUTTON (data->tool_item), _("Tools"));
	gtk_tool_button_set_icon_name (GTK_TOOL_BUTTON (data->tool_item), GTK_STOCK_EXECUTE);
	gth_toggle_menu_tool_button_set_menu (GTH_TOGGLE_MENU_TOOL_BUTTON (data->tool_item), gtk_ui_manager_get_widget (gth_browser_get_ui_manager (browser), "/ListToolsPopup"));
	gtk_widget_show (GTK_WIDGET (data->tool_item));
	gtk_toolbar_insert (GTK_TOOLBAR (gth_browser_get_browser_toolbar (browser)), data->tool_item, -1);

	g_object_set_data_full (G_OBJECT (browser), BROWSER_DATA_KEY, data, (GDestroyNotify) browser_data_free);

	update_scripts_menu (data);
	data->scripts_changed_id = g_signal_connect (gth_script_file_get (),
				                     "changed",
				                     G_CALLBACK (scripts_changed_cb),
				                     data);
}


static void
_update_sensitivity (GthBrowser *browser,
		     const char *prefix)
{
	BrowserData *data;
	int          n_selected;
	gboolean     sensitive;
	GtkWidget   *separator1;
	GtkWidget   *separator2;
	GtkWidget   *menu;

	data = g_object_get_data (G_OBJECT (browser), BROWSER_DATA_KEY);
	g_return_if_fail (data != NULL);

	n_selected = gth_file_selection_get_n_selected (GTH_FILE_SELECTION (gth_browser_get_file_list_view (browser)));
	sensitive = (n_selected > 0);

	separator1 = get_widget_with_prefix (data, prefix, "/Tools");
	separator2 = get_widget_with_prefix (data, prefix, "/Scripts");
	menu = gtk_widget_get_parent (separator1);
	{
		GList *children;
		GList *scan;

		children = gtk_container_get_children (GTK_CONTAINER (menu));

		if (separator1 != NULL) {
			for (scan = children; scan; scan = scan->next)
				if (scan->data == separator1) {
					scan = scan->next;
					break;
				}
		}
		else
			scan = children;

		for (/* void */; scan && (scan->data != separator2); scan = scan->next)
			gtk_widget_set_sensitive (scan->data, sensitive);
	}
}


void
list_tools__gth_browser_update_sensitivity_cb (GthBrowser *browser)
{
	_update_sensitivity (browser, "/ListToolsPopup");
	/*_update_sensitivity (browser, "/FileListPopup/Open_Actions/ExecWith");*/
}