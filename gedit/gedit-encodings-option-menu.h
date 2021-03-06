/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * gedit-encodings-option-menu.h
 * This file is part of gedit
 *
 * Copyright (C) 2003-2005 - Paolo Maggi
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, 
 * Boston, MA 02111-1307, USA.
 */
 
/*
 * Modified by the gedit Team, 2003-2005. See the AUTHORS file for a 
 * list of people on the gedit Team.  
 * See the ChangeLog files for a list of changes. 
 *
 * $Id$
 */

#ifndef __GEDIT_ENCODINGS_OPTION_MENU_H__
#define __GEDIT_ENCODINGS_OPTION_MENU_H__

#include <gtk/gtk.h>
#include <gedit/gedit-encodings.h>

G_BEGIN_DECLS

#define GEDIT_TYPE_ENCODINGS_OPTION_MENU             (gedit_encodings_option_menu_get_type ())
#define GEDIT_ENCODINGS_OPTION_MENU(obj)             (G_TYPE_CHECK_INSTANCE_CAST ((obj), GEDIT_TYPE_ENCODINGS_OPTION_MENU, GeditEncodingsOptionMenu))
#define GEDIT_ENCODINGS_OPTION_MENU_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST ((klass), GEDIT_TYPE_ENCODINGS_OPTION_MENU, GeditEncodingsOptionMenuClass))
#define GEDIT_IS_ENCODINGS_OPTION_MENU(obj)          (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GEDIT_TYPE_ENCODINGS_OPTION_MENU))
#define GEDIT_IS_ENCODINGS_OPTION_MENU_CLASS(klass)  (G_TYPE_CHECK_CLASS_TYPE ((klass), GEDIT_TYPE_ENCODINGS_OPTION_MENU))
#define GEDIT_ENCODINGS_OPTION_MENU_GET_CLASS(obj)   (G_TYPE_INSTANCE_GET_CLASS ((obj), GEDIT_TYPE_ENCODINGS_OPTION_MENU, GeditEncodingsOptionMenuClass))


typedef struct _GeditEncodingsOptionMenu 	GeditEncodingsOptionMenu;
typedef struct _GeditEncodingsOptionMenuClass 	GeditEncodingsOptionMenuClass;

typedef struct _GeditEncodingsOptionMenuPrivate	GeditEncodingsOptionMenuPrivate;

struct _GeditEncodingsOptionMenu
{
	GtkOptionMenu			 parent;

	GeditEncodingsOptionMenuPrivate	*priv;
};

struct _GeditEncodingsOptionMenuClass
{
	GtkOptionMenuClass		 parent_class;
};

GType		     gedit_encodings_option_menu_get_type		(void) G_GNUC_CONST;

/* Constructor */
GtkWidget 	    *gedit_encodings_option_menu_new 			(gboolean save_mode);

const GeditEncoding *gedit_encodings_option_menu_get_selected_encoding	(GeditEncodingsOptionMenu *menu);
void		     gedit_encodings_option_menu_set_selected_encoding	(GeditEncodingsOptionMenu *menu,
									 const GeditEncoding      *encoding);

G_END_DECLS

#endif /* __GEDIT_ENCODINGS_OPTION_MENU_H__ */


