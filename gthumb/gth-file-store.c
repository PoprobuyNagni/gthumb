/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 *  GThumb
 *
 *  Copyright (C) 2008-2009 Free Software Foundation, Inc.
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
#include "glib-utils.h"
#include "gth-file-store.h"


#undef DEBUG_FILE_STORE
#define VALID_ITER(iter, store) (((iter) != NULL) && ((iter)->stamp == (store)->priv->stamp) && ((iter)->user_data != NULL))
#define REALLOC_STEP 32


enum {
	CHECK_CHANGED,
	VISIBILITY_CHANGED,
	LAST_SIGNAL
};


static GType column_type[GTH_FILE_STORE_N_COLUMNS] = { G_TYPE_INVALID, };
static guint gth_file_store_signals[LAST_SIGNAL] = { 0 };


typedef struct {
	GthFileData *file_data;
	GdkPixbuf   *thumbnail;
	gboolean     is_icon;
	char        *metadata;
	gboolean     checked;

	/*< private >*/

	guint        pos;
	guint        abs_pos;
	gboolean     visible;
	gboolean     changed;
} GthFileRow;


struct _GthFileStorePrivate
{
	GthFileRow         **all_rows;
	GthFileRow         **rows;
	guint                size;
	guint                tot_rows;
	guint                num_rows;
	int                  stamp;
	gboolean             load_thumbs;
	GthTest             *filter;
	GList               *queue;
	int                  queue_size;
	GthFileDataCompFunc  cmp_func;
	gboolean             inverse_sort;
	gboolean             update_filter;
	gboolean             check_changed;
};


static GObjectClass *parent_class = NULL;


static GthFileRow *
_gth_file_row_new (void)
{
	return g_new0 (GthFileRow, 1);
}


static void
_gth_file_row_set_file (GthFileRow  *row,
			GthFileData *file)
{
	if (file != NULL) {
		g_object_ref (file);
		if (row->file_data != NULL)
			g_object_unref (row->file_data);
		row->file_data = file;
	}
}


static void
_gth_file_row_set_thumbnail (GthFileRow *row,
			     GdkPixbuf  *thumbnail)
{
	if (thumbnail != NULL) {
		g_object_ref (thumbnail);
		if (row->thumbnail != NULL)
			g_object_unref (row->thumbnail);
		row->thumbnail = thumbnail;
	}
}


static void
_gth_file_row_set_metadata (GthFileRow *row,
			    const char *metadata)
{
	if (metadata == NULL)
		return;

	if (metadata == row->metadata)
		return;

	g_free (row->metadata);
	row->metadata = g_strdup (metadata);
}


GthFileRow *
_gth_file_row_copy (GthFileRow *row)
{
	GthFileRow *row2;

	row2 = _gth_file_row_new ();
	_gth_file_row_set_file (row2, row->file_data);
	_gth_file_row_set_thumbnail (row2, row->thumbnail);
	_gth_file_row_set_metadata (row2, row->metadata);
	row2->is_icon = row->is_icon;
	row2->checked = row->checked;
	row2->pos = row->pos;
	row2->abs_pos = row->abs_pos;
	row2->visible = row->visible;
	row2->changed = row->changed;

	return row2;
}


static void
_gth_file_row_free (GthFileRow *row)
{
	if (row->file_data != NULL)
		g_object_unref (row->file_data);
	if (row->thumbnail != NULL)
		g_object_unref (row->thumbnail);
	g_free (row->metadata);
	g_free (row);
}


static void
_gth_file_store_clear_queue (GthFileStore *file_store)
{
	g_list_free (file_store->priv->queue);
	file_store->priv->queue = NULL;
	file_store->priv->queue_size = 0;
}


static void
_gth_file_store_free_rows (GthFileStore *file_store)
{
	int i;

	for (i = 0; i < file_store->priv->tot_rows; i++)
		_gth_file_row_free (file_store->priv->all_rows[i]);
	g_free (file_store->priv->all_rows);
	file_store->priv->all_rows = NULL;
	file_store->priv->tot_rows = 0;

	g_free (file_store->priv->rows);
	file_store->priv->rows = NULL;
	file_store->priv->num_rows = 0;

	file_store->priv->size = 0;

	_gth_file_store_clear_queue (file_store);
}


static void
gth_file_store_finalize (GObject *object)
{
	GthFileStore *file_store;

	file_store = GTH_FILE_STORE (object);

	if (file_store->priv != NULL) {
		_gth_file_store_free_rows (file_store);
		if (file_store->priv->filter != NULL)
			g_object_unref (file_store->priv->filter);
		g_free (file_store->priv);
		file_store->priv = NULL;
	}

	G_OBJECT_CLASS (parent_class)->finalize (object);
}


static void
gth_file_store_class_init (GthFileStoreClass *class)
{
	GObjectClass *object_class;

	parent_class = g_type_class_peek_parent (class);

	object_class = (GObjectClass*) class;
	object_class->finalize = gth_file_store_finalize;

	gth_file_store_signals[CHECK_CHANGED] =
		g_signal_new ("check_changed",
			      G_TYPE_FROM_CLASS (class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (GthFileStoreClass, check_changed),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__VOID,
			      G_TYPE_NONE,
			      0);
	gth_file_store_signals[VISIBILITY_CHANGED] =
		g_signal_new ("visibility_changed",
			      G_TYPE_FROM_CLASS (class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (GthFileStoreClass, visibility_changed),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__VOID,
			      G_TYPE_NONE,
			      0);
}


static void
gth_file_store_init (GthFileStore *file_store)
{
	file_store->priv = g_new0 (GthFileStorePrivate, 1);
	file_store->priv->rows = NULL;
	file_store->priv->size = 0;
	file_store->priv->num_rows = 0;
	file_store->priv->stamp = g_random_int ();
	file_store->priv->filter = gth_test_new ();

	if (column_type[0] == G_TYPE_INVALID) {
		column_type[GTH_FILE_STORE_FILE_DATA_COLUMN] = GTH_TYPE_FILE_DATA;
		column_type[GTH_FILE_STORE_THUMBNAIL_COLUMN] = GDK_TYPE_PIXBUF;
		column_type[GTH_FILE_STORE_IS_ICON_COLUMN] = G_TYPE_BOOLEAN;
		column_type[GTH_FILE_STORE_FILENAME_COLUMN] = G_TYPE_STRING;
		column_type[GTH_FILE_STORE_METADATA_COLUMN] = G_TYPE_STRING;
		column_type[GTH_FILE_STORE_CHECKED_COLUMN] = G_TYPE_BOOLEAN;
	}
}


static GtkTreeModelFlags
gth_file_store_get_flags (GtkTreeModel *tree_model)
{
	return GTK_TREE_MODEL_LIST_ONLY /*| GTK_TREE_MODEL_ITERS_PERSIST*/;
}


static gint
gth_file_store_get_n_columns (GtkTreeModel *tree_model)
{
	return GTH_FILE_STORE_N_COLUMNS;
}


static GType
gth_file_store_get_column_type (GtkTreeModel *tree_model,
				int           index)
{
	g_return_val_if_fail ((index >= 0) && (index < GTH_FILE_STORE_N_COLUMNS), G_TYPE_INVALID);

	return column_type[index];
}


static gboolean
gth_file_store_get_iter (GtkTreeModel *tree_model,
			 GtkTreeIter  *iter,
			 GtkTreePath  *path)
{
	GthFileStore *file_store;
	GthFileRow   *row;
	int          *indices, n;

	g_return_val_if_fail (path != NULL, FALSE);

	file_store = GTH_FILE_STORE (tree_model);

	indices = gtk_tree_path_get_indices (path);
	n = indices[0];
	if ((n < 0) || (n >= file_store->priv->num_rows))
		return FALSE;

	row = file_store->priv->rows[n];
	g_return_val_if_fail (row != NULL, FALSE);
	g_return_val_if_fail (row->pos == n, FALSE);

	iter->stamp = file_store->priv->stamp;
	iter->user_data = row;

	return TRUE;
}


static GtkTreePath *
gth_file_store_get_path (GtkTreeModel *tree_model,
			 GtkTreeIter  *iter)
{
	GthFileStore *file_store;
	GthFileRow   *row;
	GtkTreePath  *path;

	g_return_val_if_fail (iter != NULL, NULL);
	g_return_val_if_fail (iter->user_data != NULL, NULL);

	file_store = GTH_FILE_STORE (tree_model);

	g_return_val_if_fail (VALID_ITER (iter, file_store), NULL);

	row = (GthFileRow*) iter->user_data;

	path = gtk_tree_path_new ();
	gtk_tree_path_append_index (path, row->pos);

	return path;
}


static void
gth_file_store_get_value (GtkTreeModel *tree_model,
			  GtkTreeIter  *iter,
			  int           column,
			  GValue       *value)
{
	GthFileStore *file_store;
	GthFileRow   *row;

	g_return_if_fail ((column >= 0) && (column < GTH_FILE_STORE_N_COLUMNS));

	file_store = GTH_FILE_STORE (tree_model);

	g_return_if_fail (VALID_ITER (iter, file_store));

	row = (GthFileRow*) iter->user_data;

	switch (column) {
	case GTH_FILE_STORE_FILE_DATA_COLUMN:
		g_value_init (value, GTH_TYPE_FILE_DATA);
		g_value_set_object (value, row->file_data);
		break;
	case GTH_FILE_STORE_THUMBNAIL_COLUMN:
		g_value_init (value, GDK_TYPE_PIXBUF);
		g_value_set_object (value, row->thumbnail);
		break;
	case GTH_FILE_STORE_IS_ICON_COLUMN:
		g_value_init (value, G_TYPE_BOOLEAN);
		g_value_set_boolean (value, row->is_icon);
		break;
	case GTH_FILE_STORE_FILENAME_COLUMN:
		g_value_init (value, G_TYPE_STRING);
		g_value_set_string (value, g_file_info_get_display_name (row->file_data->info));
		break;
	case GTH_FILE_STORE_METADATA_COLUMN:
		g_value_init (value, G_TYPE_STRING);
		g_value_set_string (value, row->metadata);
		break;
	case GTH_FILE_STORE_CHECKED_COLUMN:
		g_value_init (value, G_TYPE_BOOLEAN);
		g_value_set_boolean (value, row->checked);
		break;
	}
}


static gboolean
gth_file_store_iter_next (GtkTreeModel  *tree_model,
			  GtkTreeIter   *iter)
{
	GthFileStore *file_store;
	GthFileRow   *row;

	if ((iter == NULL) || (iter->user_data == NULL))
		return FALSE;

	file_store = GTH_FILE_STORE (tree_model);

	g_return_val_if_fail (VALID_ITER (iter, file_store), FALSE);

  	row = (GthFileRow*) iter->user_data;
	if ((row->pos + 1) >= file_store->priv->num_rows)
		return FALSE;

	iter->stamp = file_store->priv->stamp;
	iter->user_data = file_store->priv->rows[row->pos + 1];

	return TRUE;
}


static gboolean
gth_file_store_iter_children (GtkTreeModel *tree_model,
			      GtkTreeIter  *iter,
			      GtkTreeIter  *parent)
{
	GthFileStore *file_store;

	if (parent != NULL)
		return FALSE;

	g_return_val_if_fail (parent->user_data != NULL, FALSE);

	file_store = GTH_FILE_STORE (tree_model);

	if (file_store->priv->num_rows == 0)
		return FALSE;

	iter->stamp = file_store->priv->stamp;
	iter->user_data = file_store->priv->rows[0];

	return TRUE;
}


static gboolean
gth_file_store_iter_has_child (GtkTreeModel *tree_model,
			       GtkTreeIter  *iter)
{
	return FALSE;
}


static gint
gth_file_store_iter_n_children (GtkTreeModel *tree_model,
				GtkTreeIter  *iter)
{
	GthFileStore *file_store;

	file_store = GTH_FILE_STORE (tree_model);

	if (iter == NULL)
		return file_store->priv->num_rows;

	g_return_val_if_fail (VALID_ITER (iter, file_store), 0);

  	return 0;
}


static gboolean
gth_file_store_iter_nth_child (GtkTreeModel *tree_model,
			       GtkTreeIter  *iter,
			       GtkTreeIter  *parent,
			       int           n)
{
	GthFileStore *file_store;

  	file_store = GTH_FILE_STORE (tree_model);

	if (parent != NULL) {
		g_return_val_if_fail (VALID_ITER (parent, file_store), FALSE);
		return FALSE;
	}

	if (n >= file_store->priv->num_rows)
		return FALSE;

	iter->stamp = file_store->priv->stamp;
	iter->user_data = file_store->priv->rows[n];

	return TRUE;
}


static gboolean
gth_file_store_iter_parent (GtkTreeModel *tree_model,
			    GtkTreeIter  *iter,
			    GtkTreeIter  *child)
{
	GthFileStore *file_store;

	g_return_val_if_fail (child == NULL, FALSE);

	file_store = GTH_FILE_STORE (tree_model);

	g_return_val_if_fail (VALID_ITER (child, file_store), FALSE);

	return FALSE;
}


static gboolean
gth_file_store_row_draggable (GtkTreeDragSource *drag_source,
                              GtkTreePath       *path)
{
	return TRUE;
}


static gboolean
gth_file_store_drag_data_get (GtkTreeDragSource *drag_source,
                              GtkTreePath       *path,
                              GtkSelectionData  *selection_data)
{
	gboolean      retval = FALSE;
	GthFileStore *file_store;
	int          *indices, n;
	GthFileRow   *row;

	g_return_val_if_fail (path != NULL, FALSE);

	file_store = GTH_FILE_STORE (drag_source);

	indices = gtk_tree_path_get_indices (path);
	n = indices[0];
	if ((n < 0) || (n >= file_store->priv->num_rows))
		return FALSE;

	row = file_store->priv->rows[n];
	g_return_val_if_fail (row != NULL, FALSE);
	g_return_val_if_fail (row->pos == n, FALSE);

	if (gtk_selection_data_targets_include_uri (selection_data)) {
		char **uris;

		uris = g_new (char *, 2);
		uris[0] = g_file_get_uri (row->file_data->file);
		uris[1] = NULL;
		gtk_selection_data_set_uris (selection_data, uris);
		retval = TRUE;

		g_strfreev (uris);
	}
	else if (gtk_selection_data_targets_include_text (selection_data)) {
		char *parse_name;

		parse_name = g_file_get_parse_name (row->file_data->file);
		gtk_selection_data_set_text (selection_data, parse_name, -1);
		retval = TRUE;

		g_free (parse_name);
	}

	return retval;
}

static gboolean
gth_file_store_drag_data_delete (GtkTreeDragSource *drag_source,
                                 GtkTreePath       *path)
{
	GthFileStore *file_store;
	GtkTreeIter   iter;

	g_return_val_if_fail (path != NULL, FALSE);

	file_store = GTH_FILE_STORE (drag_source);
	if (! gtk_tree_model_get_iter (GTK_TREE_MODEL (file_store), &iter, path))
		return FALSE;
	gth_file_store_remove (file_store, &iter);

	return TRUE;
}


static void
gtk_tree_model_iface_init (GtkTreeModelIface *iface)
{
	iface->get_flags       = gth_file_store_get_flags;
	iface->get_n_columns   = gth_file_store_get_n_columns;
	iface->get_column_type = gth_file_store_get_column_type;
	iface->get_iter        = gth_file_store_get_iter;
	iface->get_path        = gth_file_store_get_path;
	iface->get_value       = gth_file_store_get_value;
	iface->iter_next       = gth_file_store_iter_next;
	iface->iter_children   = gth_file_store_iter_children;
	iface->iter_has_child  = gth_file_store_iter_has_child;
	iface->iter_n_children = gth_file_store_iter_n_children;
	iface->iter_nth_child  = gth_file_store_iter_nth_child;
	iface->iter_parent     = gth_file_store_iter_parent;
}


static void
gtk_tree_drag_source_iface_init (GtkTreeDragSourceIface *iface)
{
	iface->row_draggable = gth_file_store_row_draggable;
	iface->drag_data_get = gth_file_store_drag_data_get;
	iface->drag_data_delete = gth_file_store_drag_data_delete;
}


GType
gth_file_store_get_type (void)
{
	static GType type = 0;

	if (! type) {
		GTypeInfo type_info = {
			sizeof (GthFileStoreClass),
			NULL,
			NULL,
			(GClassInitFunc) gth_file_store_class_init,
			NULL,
			NULL,
			sizeof (GthFileStore),
			0,
			(GInstanceInitFunc) gth_file_store_init
		};
		static const GInterfaceInfo gtk_tree_model_info = {
			(GInterfaceInitFunc) gtk_tree_model_iface_init,
			NULL,
			NULL
		};
		static const GInterfaceInfo gtk_tree_drag_source_info = {
			(GInterfaceInitFunc) gtk_tree_drag_source_iface_init,
			NULL,
			NULL
		};

		type = g_type_register_static (G_TYPE_OBJECT, "GthFileStore", &type_info, 0);
		g_type_add_interface_static (type, GTK_TYPE_TREE_MODEL, &gtk_tree_model_info);
		g_type_add_interface_static (type, GTK_TYPE_TREE_DRAG_SOURCE, &gtk_tree_drag_source_info);
	}

	return type;
}


GthFileStore *
gth_file_store_new (void)
{
	return (GthFileStore*) g_object_new (GTH_TYPE_FILE_STORE, NULL);
}


int
gth_file_store_get_abs_pos (GthFileStore *file_store,
			    int           pos)
{
	g_return_val_if_fail ((pos >= 0) && (pos < file_store->priv->num_rows), -1);

	return file_store->priv->rows[pos]->abs_pos;
}


static void
_gth_file_store_increment_stamp (GthFileStore *file_store)
{
	do {
		file_store->priv->stamp++;
	}
	while (file_store->priv->stamp == 0);
}


G_GNUC_UNUSED
static GList *
_gth_file_store_get_files (GthFileStore *file_store)
{
	GList *files = NULL;
	int    i;

	for (i = 0; i < file_store->priv->tot_rows; i++) {
		GthFileRow *row = file_store->priv->all_rows[i];
		files = g_list_prepend (files, g_object_ref (row->file_data));
	}

	return  g_list_reverse (files);
}


static void
_gth_file_store_row_changed (GthFileStore *file_store,
			     GthFileRow   *row)
{
	GtkTreePath *path;
	GtkTreeIter  iter;

	path = gtk_tree_path_new ();
	gtk_tree_path_append_index (path, row->pos);
	gth_file_store_get_iter (GTK_TREE_MODEL (file_store), &iter, path);

	gtk_tree_model_row_changed (GTK_TREE_MODEL (file_store), path, &iter);

	gtk_tree_path_free (path);
}


static int
campare_row_func (gconstpointer a,
		  gconstpointer b,
		  gpointer      user_data)
{
	GthFileStore *file_store = user_data;
	GthFileRow   *row_a = *((GthFileRow **) a);
	GthFileRow   *row_b = *((GthFileRow **) b);
	int           result;

	if (file_store->priv->cmp_func == NULL)
		result = -1 /*strcmp (gth_file_data_get_filename_sort_key (row_a->file),
			       gth_file_data_get_filename_sort_key (row_b->file))*/;
	else
		result = file_store->priv->cmp_func (row_a->file_data, row_b->file_data);

	if (file_store->priv->inverse_sort)
		result = result * -1;

	return result;
}


static void
_gth_file_store_sort (GthFileStore *file_store,
		      gconstpointer pbase,
		      gint          total_elems)
{
	if (file_store->priv->cmp_func == NULL)
		return;

	g_qsort_with_data (pbase,
			   total_elems,
			   (gsize) sizeof (GthFileRow *),
			   campare_row_func,
			   file_store);
}


static int
campare_by_pos (gconstpointer a,
		gconstpointer b,
		gpointer      user_data)
{
	GthFileRow *row_a = *((GthFileRow **) a);
	GthFileRow *row_b = *((GthFileRow **) b);

	if (row_a->pos == row_b->pos)
		return 0;
	else if (row_a->pos > row_b->pos)
		return 1;
	else
		return -1;
}


static void
_gth_file_store_compact_rows (GthFileStore *file_store)
{
	int i, j;

	for (i = 0, j = 0; i < file_store->priv->tot_rows; i++)
		if (file_store->priv->all_rows[i] != NULL) {
			file_store->priv->all_rows[j] = file_store->priv->all_rows[i];
			file_store->priv->all_rows[j]->abs_pos = j;
			j++;
		}
	file_store->priv->tot_rows = j;

	for (i = 0, j = 0; i < file_store->priv->num_rows; i++)
		if (file_store->priv->rows[i] != NULL) {
			file_store->priv->rows[j] = file_store->priv->rows[i];
			file_store->priv->rows[j]->pos = j;
			j++;
		}
	file_store->priv->num_rows = j;
}


static void
_gth_file_store_hide_row (GthFileStore *file_store,
			  GthFileRow   *row)
{
	int          k;
	GtkTreePath *path;

	file_store->priv->rows[row->pos] = NULL;
	for (k = row->pos; k < file_store->priv->num_rows - 1;  k++) {
		file_store->priv->rows[k] = file_store->priv->rows[k + 1];
		file_store->priv->rows[k]->pos--;
	}
	file_store->priv->num_rows--;

	path = gtk_tree_path_new_from_indices (row->pos, -1);
	gtk_tree_model_row_deleted (GTK_TREE_MODEL (file_store), path);
	gtk_tree_path_free (path);
}


static void
_gth_file_store_update_visibility (GthFileStore *file_store,
				   GList        *add_queue)
{
	GthFileRow **all_rows = NULL;
	guint        all_rows_n = 0;
	GthFileRow **old_rows = NULL;
	guint        old_rows_n = 0;
	GthFileRow **new_rows = NULL;
	guint        new_rows_n = 0;
	int          i;
	GList       *files;
	GList       *scan;
	GthFileData *file;
	int          j, k;
	gboolean     row_deleted;
	gboolean     row_inserted;


#ifdef DEBUG_FILE_STORE
g_print ("UPDATE VISIBILITY\n");
#endif

	/* store the current state */

	all_rows_n = file_store->priv->tot_rows + g_list_length (add_queue);
	all_rows = g_new (GthFileRow *, all_rows_n);
	for (i = 0; i < file_store->priv->tot_rows; i++)
		all_rows[i] = _gth_file_row_copy (file_store->priv->all_rows[i]);

	for (scan = add_queue; scan; scan = scan->next)
		all_rows[i++] = (GthFileRow *) scan->data;

	old_rows_n = file_store->priv->num_rows;
	old_rows = g_new (GthFileRow *, old_rows_n);
	for (i = 0, j = 0; i < all_rows_n; i++) {
		if (all_rows[i]->visible) {
			old_rows[j] = all_rows[i];
			old_rows[j]->visible = FALSE;
			j++;
		}
	}

	/* reorder and filter */

	_gth_file_store_sort (file_store, all_rows, all_rows_n);

	files = NULL;
	for (i = 0; i < all_rows_n; i++) {
		GthFileRow *row = all_rows[i];

		row->abs_pos = i;
		files = g_list_prepend (files, g_object_ref (row->file_data));
	}
	files = g_list_reverse (files);

	new_rows_n = 0;
	gth_test_set_file_list (file_store->priv->filter, files);
	while ((file = gth_test_get_next (file_store->priv->filter)) != NULL) {
		GthFileRow *row = NULL;

		for (i = 0; i < all_rows_n; i++) {
			row = all_rows[i];
			if (row->file_data == file)
				break;
		}

		g_assert (i < all_rows_n);

		row->visible = TRUE;
		new_rows_n++;
	}

	_g_object_list_unref (files);

	/* create the new visible rows array */

	new_rows = g_new (GthFileRow *, new_rows_n);
	for (i = 0, j = 0; i < all_rows_n; i++) {
		GthFileRow *row = all_rows[i];

		if (! row->visible)
			continue;

		row->pos = j++;
		new_rows[row->pos] = row;
	}

	/* emit the signals required to go from the old state to the new state */

	/* hide filtered out files */

	row_deleted = FALSE;
	for (i = old_rows_n - 1; i >= 0; i--) {
		/* search old_rows[i] in new_rows */

		for (j = 0; j < new_rows_n; j++)
			if (old_rows[i]->file_data == new_rows[j]->file_data)
				break;

		if (j < new_rows_n)
			continue;

		/* old_rows[i] is not present in new_rows, emit a deleted
		 * signal. */

#ifdef DEBUG_FILE_STORE
g_print ("  DELETE: %d\n", old_rows[i]->pos);
#endif

		_gth_file_store_hide_row (file_store, old_rows[i]);

		old_rows[i] = NULL;
		row_deleted = TRUE;
	}

	/* compact the old visible rows array if needed */

	if (row_deleted) {
		for (i = 0, j = 0; i < old_rows_n; i++) {
			if (old_rows[i] != NULL) {
				old_rows[j] = old_rows[i];
				/*old_rows[j]->abs_pos = j; FIXME: check if this is correct */
				j++;
			}
		}
		old_rows_n = j;
	}

	/* Both old_rows and file_store->priv->rows now contain the files
	 * visible before and after the filtering.
	 * Reorder the two arrays according to the new_rows order */

	if (old_rows_n > 0) {
		gboolean    order_changed;
		int        *new_order;
		GthFileRow *tmp_row;

		order_changed = FALSE;
		new_order = g_new0 (int, old_rows_n);
		for (i = 0, k = 0; i < new_rows_n; i++) {
			/* search new_rows[i] in old_rows */

			for (j = 0; j < old_rows_n; j++)
				if (new_rows[i]->file_data == old_rows[j]->file_data)
					break;

			if (j >= old_rows_n)
				continue;

			/* old_rows[j] == new_rows[i] */

			/* k is the new position of old_rows[j] in new_rows
			 * without considering the new elements, that is
			 * the elements in new_rows not present in old_rows */

			new_order[k] = j;
			old_rows[j]->pos = k;

			/* swap the position of j and k in file_store->priv->rows
			 * old_rows can't be reordered now because we need
			 * to know the old positions, it will be ordered
			 * later with g_qsort_with_data */

			if (k != j) {
				tmp_row = file_store->priv->rows[j];
				file_store->priv->rows[j] = file_store->priv->rows[k];
				file_store->priv->rows[j]->pos = j;
				file_store->priv->rows[k] = tmp_row;
				file_store->priv->rows[k]->pos = k;

				order_changed = TRUE;
			}

			k++;
		}
		if (order_changed) {

#ifdef DEBUG_FILE_STORE
g_print ("  REORDER: ");
for (i = 0; i < old_rows_n; i++)
	g_print ("%d ", new_order[i]);
g_print ("\n");
#endif

			gtk_tree_model_rows_reordered (GTK_TREE_MODEL (file_store), NULL, NULL, new_order);
		}

		g_free (new_order);
	}

	/* set the new state */

	_gth_file_store_increment_stamp (file_store);

	for (i = 0; i < file_store->priv->tot_rows; i++)
		_gth_file_row_free (file_store->priv->all_rows[i]);
	g_free (file_store->priv->all_rows);
	file_store->priv->all_rows = all_rows;
	file_store->priv->tot_rows = all_rows_n;

	g_qsort_with_data (old_rows,
			   old_rows_n,
			   (gsize) sizeof (GthFileRow *),
			   campare_by_pos,
			   NULL);

	g_free (file_store->priv->rows);
	file_store->priv->rows = g_new (GthFileRow *, new_rows_n);
	for (i = 0; i < old_rows_n; i++)
		file_store->priv->rows[i] = old_rows[i];

	g_assert (file_store->priv->num_rows == old_rows_n);

	/* add the new files */

	row_inserted = FALSE;
	for (i = 0; i < new_rows_n; i++) {
		GtkTreePath *path;
		GtkTreeIter  iter;

		if ((i < file_store->priv->num_rows) && (new_rows[i]->file_data == file_store->priv->rows[i]->file_data))
			continue;

#ifdef DEBUG_FILE_STORE
g_print ("  INSERT: %d\n", i);
#endif

		/* add the file to the visible rows */

		for (k = file_store->priv->num_rows; k > i; k--) {
			file_store->priv->rows[k] = file_store->priv->rows[k - 1];
			file_store->priv->rows[k]->pos++;
		}
		file_store->priv->rows[i] = new_rows[i];
		file_store->priv->rows[i]->pos = i;
		file_store->priv->num_rows++;

		path = gtk_tree_path_new ();
		gtk_tree_path_append_index (path, i);
		gth_file_store_get_iter (GTK_TREE_MODEL (file_store), &iter, path);
		gtk_tree_model_row_inserted (GTK_TREE_MODEL (file_store), path, &iter);
		gtk_tree_path_free (path);

		row_inserted = TRUE;
	}

	g_signal_emit (file_store, gth_file_store_signals[VISIBILITY_CHANGED], 0);
	g_signal_emit (file_store, gth_file_store_signals[CHECK_CHANGED], 0);

	g_free (new_rows);
	g_free (old_rows);
}


void
gth_file_store_set_filter (GthFileStore *file_store,
			   GthTest      *filter)
{
	if (file_store->priv->filter != NULL) {
		g_object_unref (file_store->priv->filter);
		file_store->priv->filter = NULL;
	}

	if (filter != NULL)
		file_store->priv->filter = g_object_ref (filter);
	else
		file_store->priv->filter = gth_test_new ();

	_gth_file_store_update_visibility (file_store, NULL);
}


static void
_gth_file_store_reorder (GthFileStore *file_store)
{
	int *new_order;
	int  i, j;

	_gth_file_store_sort (file_store, file_store->priv->all_rows, file_store->priv->tot_rows);

	/* compute the new position for each row */

	new_order = g_new (int, file_store->priv->num_rows);
	for (i = 0, j = -1; i < file_store->priv->tot_rows; i++) {
		GthFileRow *row = file_store->priv->all_rows[i];
		if (row->visible) {
			j = j + 1;
			file_store->priv->rows[j] = row;
			new_order[j] = row->pos;
			row->pos = j;
		}
		row->abs_pos = i;
	}
	if (new_order != NULL)
		gtk_tree_model_rows_reordered (GTK_TREE_MODEL (file_store), NULL, NULL, new_order);
	g_free (new_order);
}


void
gth_file_store_set_sort_func (GthFileStore        *file_store,
			      GthFileDataCompFunc  cmp_func,
			      gboolean             inverse_sort)
{
	if ((cmp_func == file_store->priv->cmp_func) && (inverse_sort == file_store->priv->inverse_sort))
		return;

	file_store->priv->cmp_func = cmp_func;
	file_store->priv->inverse_sort = inverse_sort;
	_gth_file_store_reorder (file_store);
}


GList *
gth_file_store_get_all (GthFileStore *file_store)
{
	GList *list = NULL;
	int    i;

	for (i = 0; i < file_store->priv->tot_rows; i++)
		list = g_list_prepend (list, g_object_ref (file_store->priv->all_rows[i]->file_data));

	return g_list_reverse (list);
}


int
gth_file_store_n_files (GthFileStore *file_store)
{
	return file_store->priv->tot_rows;
}


GList *
gth_file_store_get_visibles (GthFileStore *file_store)
{
	GList *list = NULL;
	int    i;

	for (i = 0; i < file_store->priv->num_rows; i++)
		list = g_list_prepend (list, g_object_ref (file_store->priv->rows[i]->file_data));

	return g_list_reverse (list);
}


int
gth_file_store_n_visibles (GthFileStore *file_store)
{
	return file_store->priv->num_rows;
}


GList *
gth_file_store_get_checked (GthFileStore *file_store)
{
	GList *list = NULL;
	int    i;

	for (i = 0; i < file_store->priv->num_rows; i++) {
		GthFileRow *row = file_store->priv->rows[i];

		if ((row != NULL) && row->checked)
			list = g_list_prepend (list, g_object_ref (file_store->priv->rows[i]->file_data));
	}

	return g_list_reverse (list);
}


int
gth_file_store_get_n_checked (GthFileStore *file_store)
{
	int n = 0;
	int i;

	for (i = 0; i < file_store->priv->num_rows; i++) {
		GthFileRow *row = file_store->priv->rows[i];

		if ((row != NULL) && row->checked)
			n++;
	}

	return n;
}


GthFileData *
gth_file_store_get_file (GthFileStore *file_store,
			 GtkTreeIter  *iter)
{
	g_return_val_if_fail (VALID_ITER (iter, file_store), NULL);

	return ((GthFileRow *) iter->user_data)->file_data;
}


gboolean
gth_file_store_find (GthFileStore *file_store,
		     GFile        *file,
		     GtkTreeIter  *iter)
{
	gboolean found = FALSE;
	int      i;

	for (i = 0; i < file_store->priv->tot_rows; i++) {
		GthFileRow *row = file_store->priv->all_rows[i];

		if (row == NULL)
			continue;

		if (g_file_equal (row->file_data->file, file)) {
			if (iter != NULL) {
				iter->stamp = file_store->priv->stamp;
				iter->user_data = row;
			}
			found = TRUE;
			break;
		}
	}

	return found;
}


gboolean
gth_file_store_find_visible (GthFileStore *file_store,
			     GFile        *file,
			     GtkTreeIter  *iter)
{
	gboolean found = FALSE;
	int      i;

	for (i = 0; i < file_store->priv->num_rows; i++) {
		GthFileRow *row = file_store->priv->rows[i];

		if (row == NULL)
			continue;

		if (g_file_equal (row->file_data->file, file)) {
			if (iter != NULL) {
				iter->stamp = file_store->priv->stamp;
				iter->user_data = row;
			}
			found = TRUE;
			break;
		}
	}

	return found;
}


int
gth_file_store_get_pos (GthFileStore *file_store,
			GFile        *file)
{
	GtkTreeIter iter;
	GthFileRow *row;

	if (! gth_file_store_find_visible (file_store, file, &iter))
		return -1;

	row = iter.user_data;

	return row->pos;
}


gboolean
gth_file_store_get_nth (GthFileStore *file_store,
		        int           n,
		        GtkTreeIter  *iter)
{
	GthFileRow *row;

	if (file_store->priv->tot_rows <= n)
		return FALSE;

	row = file_store->priv->all_rows[n];
	g_return_val_if_fail (row != NULL, FALSE);

	if (iter != NULL) {
		iter->stamp = file_store->priv->stamp;
		iter->user_data = row;
	}

	return TRUE;
}


gboolean
gth_file_store_get_next (GthFileStore *file_store,
			 GtkTreeIter  *iter)
{
	GthFileRow *row;

	if ((iter == NULL) || (iter->user_data == NULL))
		return FALSE;

	g_return_val_if_fail (VALID_ITER (iter, file_store), FALSE);

  	row = (GthFileRow*) iter->user_data;
	if (row->abs_pos + 1 >= file_store->priv->tot_rows)
		return FALSE;

	if (iter != NULL) {
		iter->stamp = file_store->priv->stamp;
		iter->user_data = file_store->priv->all_rows[row->abs_pos + 1];
	}

	return TRUE;
}


gboolean
gth_file_store_get_nth_visible (GthFileStore *file_store,
				int           n,
				GtkTreeIter  *iter)
{
	GthFileRow *row;

	if (file_store->priv->num_rows <= n)
		return FALSE;

	row = file_store->priv->rows[n];
	g_return_val_if_fail (row != NULL, FALSE);

	if (iter != NULL) {
		iter->stamp = file_store->priv->stamp;
		iter->user_data = row;
	}

	return TRUE;
}


gboolean
gth_file_store_get_next_visible (GthFileStore *file_store,
				 GtkTreeIter  *iter)
{
	GthFileRow *row;

	if ((iter == NULL) || (iter->user_data == NULL))
		return FALSE;

	g_return_val_if_fail (VALID_ITER (iter, file_store), FALSE);

  	row = (GthFileRow*) iter->user_data;
	if (row->pos + 1 >= file_store->priv->num_rows)
		return FALSE;

	if (iter != NULL) {
		iter->stamp = file_store->priv->stamp;
		iter->user_data = file_store->priv->rows[row->pos + 1];
	}

	return TRUE;
}


gboolean
gth_file_store_get_prev_visible (GthFileStore *file_store,
				 GtkTreeIter  *iter)
{
	GthFileRow *row;

	if ((iter == NULL) || (iter->user_data == NULL))
		return FALSE;

	g_return_val_if_fail (VALID_ITER (iter, file_store), FALSE);

  	row = (GthFileRow*) iter->user_data;
	if (row->pos == 0)
		return FALSE;

	if (iter != NULL) {
		iter->stamp = file_store->priv->stamp;
		iter->user_data = file_store->priv->rows[row->pos - 1];
	}

	return TRUE;
}


void
gth_file_store_add (GthFileStore *file_store,
		    GthFileData  *file,
		    GdkPixbuf    *thumbnail,
		    gboolean      is_icon,
		    const char   *metadata,
		    gboolean      checked)
{
	gth_file_store_queue_add (file_store, file, thumbnail, is_icon, metadata, checked);
	gth_file_store_exec_add (file_store);
}


void
gth_file_store_queue_add (GthFileStore *file_store,
			  GthFileData  *file,
			  GdkPixbuf    *thumbnail,
			  gboolean      is_icon,
			  const char   *metadata,
			  gboolean      checked)
{
	GthFileRow *row;

	g_return_if_fail (file != NULL);

	row = _gth_file_row_new ();
	_gth_file_row_set_file (row, file);
	_gth_file_row_set_thumbnail (row, thumbnail);
	_gth_file_row_set_metadata (row, metadata);
	row->is_icon = is_icon;
	row->checked = checked;
	file_store->priv->queue = g_list_prepend (file_store->priv->queue, row);
}


void
gth_file_store_exec_add (GthFileStore *file_store)
{
	_gth_file_store_update_visibility (file_store, file_store->priv->queue);
	_gth_file_store_clear_queue (file_store);
}


static void
gth_file_store_queue_set_valist (GthFileStore *file_store,
			         GtkTreeIter  *iter,
			         va_list       var_args)
{
	GthFileRow  *row;
	int          column;

	g_return_if_fail (VALID_ITER (iter, file_store));

  	row = (GthFileRow*) iter->user_data;

  	column = va_arg (var_args, int);
  	while (column != -1) {
  		GthFileData *file_data;
  		GdkPixbuf   *thumbnail;
  		const char  *metadata;

  		switch (column) {
  		case GTH_FILE_STORE_FILE_DATA_COLUMN:
  			file_data = va_arg (var_args, GthFileData *);
  			g_return_if_fail (GTH_IS_FILE_DATA (file_data));
  			_gth_file_row_set_file (row, file_data);
  			row->changed = TRUE;
  			file_store->priv->update_filter = TRUE;
  			break;
  		case GTH_FILE_STORE_THUMBNAIL_COLUMN:
  			thumbnail = va_arg (var_args, GdkPixbuf *);
  			g_return_if_fail (GDK_IS_PIXBUF (thumbnail));
  			_gth_file_row_set_thumbnail (row, thumbnail);
  			row->changed = TRUE;
  			break;
  		case GTH_FILE_STORE_IS_ICON_COLUMN:
  			row->is_icon = va_arg (var_args, gboolean);
  			row->changed = TRUE;
  			break;
  		case GTH_FILE_STORE_METADATA_COLUMN:
  			metadata = va_arg (var_args, const char *);
  			_gth_file_row_set_metadata (row, metadata);
  			row->changed = TRUE;
  			break;
  		case GTH_FILE_STORE_CHECKED_COLUMN:
  			row->checked = va_arg (var_args, gboolean);
  			row->changed = TRUE;
  			file_store->priv->check_changed = TRUE;
  			break;
  		default:
  			g_warning ("%s: Invalid column number %d added to iter (remember to end your list of columns with a -1)", G_STRLOC, column);
  			break;
  		}

  		column = va_arg (var_args, int);
  	}
}


void
gth_file_store_set (GthFileStore *file_store,
		    GtkTreeIter  *iter,
		    ...)
{
	va_list var_args;

	va_start (var_args, iter);
	gth_file_store_queue_set_valist (file_store, iter, var_args);
	va_end (var_args);

	gth_file_store_exec_set (file_store);
}


void
gth_file_store_queue_set (GthFileStore *file_store,
			  GtkTreeIter  *iter,
			  ...)
{
	va_list var_args;

	va_start (var_args, iter);
	gth_file_store_queue_set_valist (file_store, iter, var_args);
	va_end (var_args);
}


static void
_gth_file_store_list_changed (GthFileStore *file_store)
{
	int i;

	for (i = 0; i < file_store->priv->num_rows; i++) {
		GthFileRow *row = file_store->priv->rows[i];

		if (row->visible && row->changed)
			_gth_file_store_row_changed (file_store, row);
		row->changed = FALSE;
	}
}


void
gth_file_store_exec_set (GthFileStore *file_store)
{
	_gth_file_store_list_changed (file_store);
	_gth_file_store_clear_queue (file_store);
	if (file_store->priv->update_filter) {
		_gth_file_store_update_visibility (file_store, NULL);
		file_store->priv->update_filter = FALSE;
	}
	else if (file_store->priv->check_changed) {
		g_signal_emit (file_store, gth_file_store_signals[CHECK_CHANGED], 0);
		file_store->priv->check_changed = FALSE;
	}
}


void
gth_file_store_remove (GthFileStore *file_store,
		       GtkTreeIter  *iter)
{
	gth_file_store_queue_remove (file_store, iter);
	gth_file_store_exec_remove (file_store);
}


void
gth_file_store_queue_remove (GthFileStore *file_store,
			     GtkTreeIter  *iter)
{
	GthFileRow  *row;

	g_return_if_fail (VALID_ITER (iter, file_store));

  	row = (GthFileRow*) iter->user_data;

	file_store->priv->queue = g_list_prepend (file_store->priv->queue, file_store->priv->all_rows[row->abs_pos]);
}


void
gth_file_store_exec_remove (GthFileStore *file_store)
{
	GList *scan;

	if (file_store->priv->queue == NULL)
		return;

	file_store->priv->queue = g_list_reverse (file_store->priv->queue);
	for (scan = file_store->priv->queue; scan; scan = scan->next) {
		GthFileRow *row = scan->data;

		if (row->visible)
			_gth_file_store_hide_row (file_store, row);

		file_store->priv->all_rows[row->abs_pos] = NULL;
		_gth_file_row_free (row);
	}
	_gth_file_store_compact_rows (file_store);
	_gth_file_store_clear_queue (file_store);

	_gth_file_store_update_visibility (file_store, NULL);
}


void
gth_file_store_clear (GthFileStore *file_store)
{
	int i;

	for (i = file_store->priv->num_rows - 1; i >= 0; i--) {
		GthFileRow  *row = file_store->priv->rows[i];
		GtkTreePath *path;

		path = gtk_tree_path_new ();
		gtk_tree_path_append_index (path, row->pos);
		gtk_tree_model_row_deleted (GTK_TREE_MODEL (file_store), path);
		gtk_tree_path_free (path);
	}

	_gth_file_store_free_rows (file_store);
	_gth_file_store_increment_stamp (file_store);
}


static int
reorder_sort_func (gconstpointer a,
		   gconstpointer b,
		   gpointer      user_data)
{
	GHashTable *new_positions = user_data;
	int         apos = GPOINTER_TO_INT (g_hash_table_lookup (new_positions, * (GthFileRow **) a));
	int         bpos = GPOINTER_TO_INT (g_hash_table_lookup (new_positions, * (GthFileRow **) b));

	if (apos < bpos)
		return -1;
	if (apos > bpos)
		return 1;
	return 0;
}


void
gth_file_store_reorder (GthFileStore *file_store,
			int          *new_order)
{
	int         *order;
	GHashTable  *new_positions;
	int          i;

	order = g_new (int, file_store->priv->num_rows);
	for (i = 0; i < file_store->priv->num_rows; i++)
		order[new_order[i]] = i;

	new_positions = g_hash_table_new (g_direct_hash, g_direct_equal);
	for (i = 0; i < file_store->priv->num_rows; i++)
		g_hash_table_insert (new_positions, file_store->priv->rows[i], GINT_TO_POINTER (order[i]));

	g_qsort_with_data (file_store->priv->rows,
			   file_store->priv->num_rows,
			   (gsize) sizeof (GthFileRow *),
			   reorder_sort_func,
			   new_positions);

	for (i = 0; i < file_store->priv->num_rows; i++)
		file_store->priv->rows[i]->pos = i;

	gtk_tree_model_rows_reordered (GTK_TREE_MODEL (file_store), NULL, NULL, new_order);

	g_hash_table_destroy (new_positions);
	g_free (order);
}