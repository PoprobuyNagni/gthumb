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

#ifndef GTH_FILE_TOOL_MIRROR_H
#define GTH_FILE_TOOL_MIRROR_H

#include <gthumb.h>

G_BEGIN_DECLS

#define GTH_TYPE_FILE_TOOL_MIRROR (gth_file_tool_mirror_get_type ())
#define GTH_FILE_TOOL_MIRROR(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), GTH_TYPE_FILE_TOOL_MIRROR, GthFileToolMirror))
#define GTH_FILE_TOOL_MIRROR_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST ((klass), GTH_TYPE_FILE_TOOL_MIRROR, GthFileToolMirrorClass))
#define GTH_IS_FILE_TOOL_MIRROR(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GTH_TYPE_FILE_TOOL_MIRROR))
#define GTH_IS_FILE_TOOL_MIRROR_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), GTH_TYPE_FILE_TOOL_MIRROR))
#define GTH_FILE_TOOL_MIRROR_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS ((obj), GTH_TYPE_FILE_TOOL_MIRROR, GthFileToolMirrorClass))

typedef struct _GthFileToolMirror GthFileToolMirror;
typedef struct _GthFileToolMirrorClass GthFileToolMirrorClass;

struct _GthFileToolMirror {
	GthFileTool parent_instance;
};

struct _GthFileToolMirrorClass {
	GthFileToolClass parent_class;
};

GType  gth_file_tool_mirror_get_type  (void);

G_END_DECLS

#endif /* GTH_FILE_TOOL_MIRROR_H */