/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 *  GThumb
 *
 *  Copyright (C) 2001-2008 The Free Software Foundation, Inc.
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

#ifndef GTH_FILE_LIST_H
#define GTH_FILE_LIST_H

#include <gtk/gtk.h>
#include "gth-file-data.h"
#include "gth-file-source.h"
#include "gth-file-store.h"
#include "gth-test.h"

G_BEGIN_DECLS

#define GTH_TYPE_FILE_LIST            (gth_file_list_get_type ())
#define GTH_FILE_LIST(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), GTH_TYPE_FILE_LIST, GthFileList))
#define GTH_FILE_LIST_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), GTH_TYPE_FILE_LIST, GthFileListClass))
#define GTH_IS_FILE_LIST(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GTH_TYPE_FILE_LIST))
#define GTH_IS_FILE_LIST_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), GTH_TYPE_FILE_LIST))
#define GTH_FILE_LIST_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS((obj), GTH_TYPE_FILE_LIST, GthFileListClass))

typedef struct _GthFileList             GthFileList;
typedef struct _GthFileListClass        GthFileListClass;
typedef struct _GthFileListPrivateData  GthFileListPrivateData;

struct _GthFileList {
	GtkVBox __parent;
	GthFileListPrivateData *priv;
};

struct _GthFileListClass {
	GtkVBoxClass __parent;
};

GType          gth_file_list_get_type       (void);
GtkWidget *    gth_file_list_new            (void);
void           gth_file_list_cancel         (GthFileList          *file_list,
					     DoneFunc              done_func,
					     gpointer              user_data);
void           gth_file_list_set_files      (GthFileList          *file_list,
					     GthFileSource        *file_source,
					     GList                *list);
GList *        gth_file_list_get_files      (GthFileList          *file_list,
					     GList                *tree_path_list);
void           gth_file_list_clear          (GthFileList          *file_list,
					     const char           *message);
void           gth_file_list_add_files      (GthFileList          *file_list,
					     GList                *list /* GthFileData */);
void           gth_file_list_delete_files   (GthFileList          *file_list,
					     GList                *list /* GFile */);
void           gth_file_list_update_files   (GthFileList          *file_list,
					     GList                *list /* GthFileData */);
void           gth_file_list_rename_file    (GthFileList          *file_list,
					     GFile                *file,
					     GthFileData          *file_data);
void           gth_file_list_set_filter     (GthFileList          *file_list,
					     GthTest              *filter);
void           gth_file_list_set_sort_func  (GthFileList          *file_list,
					     GthFileDataCompFunc   cmp_func,
					     gboolean              inverse_sort);
void           gth_file_list_enable_thumbs  (GthFileList          *file_list,
					     gboolean              enable);
void           gth_file_list_set_thumb_size (GthFileList          *file_list,
					     int                   size);
void           gth_file_list_set_caption    (GthFileList          *file_list,
					     const char           *attribute);
GtkWidget *    gth_file_list_get_view       (GthFileList          *file_list);
int            gth_file_list_first_file     (GthFileList          *file_list,
					     gboolean              skip_broken,
					     gboolean              only_selected);
int            gth_file_list_last_file      (GthFileList          *file_list,
					     gboolean              skip_broken,
					     gboolean              only_selected);
int            gth_file_list_next_file      (GthFileList          *file_list,
					     int                   pos,
					     gboolean              skip_broken,
					     gboolean              only_selected,
					     gboolean              wrap);
int            gth_file_list_prev_file      (GthFileList          *file_list,
					     int                   pos,
					     gboolean              skip_broken,
					     gboolean              only_selected,
					     gboolean              wrap);

G_END_DECLS

#endif /* GTH_FILE_LIST_H */