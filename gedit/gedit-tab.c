/*
 * gedit-tab.c
 * This file is part of gedit
 *
 * Copyright (C) 2005 - Paolo Maggi 
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
 * Modified by the gedit Team, 2005. See the AUTHORS file for a 
 * list of people on the gedit Team.  
 * See the ChangeLog files for a list of changes. 
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <glib/gi18n.h>
#include <gio/gio.h>

#include "gedit-app.h"
#include "gedit-notebook.h"
#include "gedit-tab.h"
#include "gedit-utils.h"
#include "gedit-io-error-info-bar.h"
#include "gedit-print-job.h"
#include "gedit-print-preview.h"
#include "gedit-progress-info-bar.h"
#include "gedit-debug.h"
#include "gedit-enum-types.h"
#include "gedit-settings.h"
#include "gedit-view-frame.h"

#define GEDIT_TAB_GET_PRIVATE(object)(G_TYPE_INSTANCE_GET_PRIVATE ((object), GEDIT_TYPE_TAB, GeditTabPrivate))

#define GEDIT_TAB_KEY "GEDIT_TAB_KEY"

struct _GeditTabPrivate
{
	GSettings	       *editor;
	GeditTabState	        state;

	GeditViewFrame         *frame;

	GtkWidget	       *info_bar;
	GtkWidget	       *print_preview;

	GeditPrintJob          *print_job;

	/* tmp data for saving */
	GFile		       *tmp_save_location;

	/* tmp data for loading */
	gint                    tmp_line_pos;
	gint                    tmp_column_pos;
	const GeditEncoding    *tmp_encoding;
	
	GTimer 		       *timer;
	guint		        times_called;

	GeditDocumentSaveFlags	save_flags;

        gint                    auto_save_interval;
        guint                   auto_save_timeout;
        
	gint	                not_editable : 1;
	gint                    auto_save : 1;

	gint                    ask_if_externally_modified : 1;
};

G_DEFINE_TYPE(GeditTab, gedit_tab, GTK_TYPE_VBOX)

enum
{
	PROP_0,
	PROP_NAME,
	PROP_STATE,
	PROP_AUTO_SAVE,
	PROP_AUTO_SAVE_INTERVAL
};

/* Signals */
enum
{
	DROP_URIS,
	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

static gboolean gedit_tab_auto_save (GeditTab *tab);

static void
install_auto_save_timeout (GeditTab *tab)
{
	gint timeout;

	gedit_debug (DEBUG_TAB);

	g_return_if_fail (tab->priv->auto_save_timeout <= 0);
	g_return_if_fail (tab->priv->auto_save);
	g_return_if_fail (tab->priv->auto_save_interval > 0);
	
	g_return_if_fail (tab->priv->state != GEDIT_TAB_STATE_LOADING);
	g_return_if_fail (tab->priv->state != GEDIT_TAB_STATE_SAVING);
	g_return_if_fail (tab->priv->state != GEDIT_TAB_STATE_REVERTING);
	g_return_if_fail (tab->priv->state != GEDIT_TAB_STATE_LOADING_ERROR);
	g_return_if_fail (tab->priv->state != GEDIT_TAB_STATE_SAVING_ERROR);
	g_return_if_fail (tab->priv->state != GEDIT_TAB_STATE_SAVING_ERROR);
	g_return_if_fail (tab->priv->state != GEDIT_TAB_STATE_REVERTING_ERROR);

	/* Add a new timeout */
	timeout = g_timeout_add_seconds (tab->priv->auto_save_interval * 60,
					 (GSourceFunc) gedit_tab_auto_save,
					 tab);

	tab->priv->auto_save_timeout = timeout;
}

static gboolean
install_auto_save_timeout_if_needed (GeditTab *tab)
{
	GeditDocument *doc;

	gedit_debug (DEBUG_TAB);
	
	g_return_val_if_fail (tab->priv->auto_save_timeout <= 0, FALSE);
	g_return_val_if_fail ((tab->priv->state == GEDIT_TAB_STATE_NORMAL) ||
			      (tab->priv->state == GEDIT_TAB_STATE_SHOWING_PRINT_PREVIEW) ||
			      (tab->priv->state == GEDIT_TAB_STATE_CLOSING), FALSE);

	if (tab->priv->state == GEDIT_TAB_STATE_CLOSING)
		return FALSE;

	doc = gedit_tab_get_document (tab);

 	if (tab->priv->auto_save && 
 	    !gedit_document_is_untitled (doc) &&
 	    !gedit_document_get_readonly (doc))
 	{ 
 		install_auto_save_timeout (tab);
 		
 		return TRUE;
 	}
 	
 	return FALSE;
}

static void
remove_auto_save_timeout (GeditTab *tab)
{
	gedit_debug (DEBUG_TAB);

	/* FIXME: check sugli stati */
	
	g_return_if_fail (tab->priv->auto_save_timeout > 0);
	
	g_source_remove (tab->priv->auto_save_timeout);
	tab->priv->auto_save_timeout = 0;
}

static void
gedit_tab_get_property (GObject    *object,
		        guint       prop_id,
		        GValue     *value,
		        GParamSpec *pspec)
{
	GeditTab *tab = GEDIT_TAB (object);

	switch (prop_id)
	{
		case PROP_NAME:
			g_value_take_string (value,
					     _gedit_tab_get_name (tab));
			break;
		case PROP_STATE:
			g_value_set_enum (value,
					  gedit_tab_get_state (tab));
			break;			
		case PROP_AUTO_SAVE:
			g_value_set_boolean (value,
					     gedit_tab_get_auto_save_enabled (tab));
			break;
		case PROP_AUTO_SAVE_INTERVAL:
			g_value_set_int (value,
					 gedit_tab_get_auto_save_interval (tab));
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
			break;			
	}
}

static void
gedit_tab_set_property (GObject      *object,
		        guint         prop_id,
		        const GValue *value,
		        GParamSpec   *pspec)
{
	GeditTab *tab = GEDIT_TAB (object);

	switch (prop_id)
	{
		case PROP_AUTO_SAVE:
			gedit_tab_set_auto_save_enabled (tab,
							 g_value_get_boolean (value));
			break;
		case PROP_AUTO_SAVE_INTERVAL:
			gedit_tab_set_auto_save_interval (tab,
							  g_value_get_int (value));
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
			break;			
	}
}

static void
gedit_tab_dispose (GObject *object)
{
	GeditTab *tab = GEDIT_TAB (object);

	if (tab->priv->tmp_save_location != NULL)
	{
		g_object_unref (tab->priv->tmp_save_location);
		tab->priv->tmp_save_location = NULL;
	}

	if (tab->priv->editor != NULL)
	{
		g_object_unref (tab->priv->editor);
		tab->priv->editor = NULL;
	}

	G_OBJECT_CLASS (gedit_tab_parent_class)->dispose (object);
}

static void
gedit_tab_finalize (GObject *object)
{
	GeditTab *tab = GEDIT_TAB (object);

	if (tab->priv->timer != NULL)
		g_timer_destroy (tab->priv->timer);

	if (tab->priv->auto_save_timeout > 0)
		remove_auto_save_timeout (tab);

	G_OBJECT_CLASS (gedit_tab_parent_class)->finalize (object);
}

static void
gedit_tab_grab_focus (GtkWidget *widget)
{
	GeditTab *tab = GEDIT_TAB (widget);
	
	GTK_WIDGET_CLASS (gedit_tab_parent_class)->grab_focus (widget);
	
	if (tab->priv->info_bar != NULL)
	{
		gtk_widget_grab_focus (tab->priv->info_bar);
	}
	else
	{
		gtk_widget_grab_focus (GTK_WIDGET (gedit_tab_get_view (tab)));
	}
}

static void 
gedit_tab_class_init (GeditTabClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GtkWidgetClass *gtkwidget_class = GTK_WIDGET_CLASS (klass);

	object_class->dispose = gedit_tab_dispose;
	object_class->finalize = gedit_tab_finalize;
	object_class->get_property = gedit_tab_get_property;
	object_class->set_property = gedit_tab_set_property;
	
	gtkwidget_class->grab_focus = gedit_tab_grab_focus;
	
	g_object_class_install_property (object_class,
					 PROP_NAME,
					 g_param_spec_string ("name",
							      "Name",
							      "The tab's name",
							      NULL,
							      G_PARAM_READABLE |
							      G_PARAM_STATIC_STRINGS));

	g_object_class_install_property (object_class,
					 PROP_STATE,
					 g_param_spec_enum ("state",
							    "State",
							    "The tab's state",
							    GEDIT_TYPE_TAB_STATE,
							    GEDIT_TAB_STATE_NORMAL,
							    G_PARAM_READABLE |
							    G_PARAM_STATIC_STRINGS));

	g_object_class_install_property (object_class,
					 PROP_AUTO_SAVE,
					 g_param_spec_boolean ("autosave",
							       "Autosave",
							       "Autosave feature",
							       TRUE,
							       G_PARAM_READWRITE |
							       G_PARAM_STATIC_STRINGS));

	g_object_class_install_property (object_class,
					 PROP_AUTO_SAVE_INTERVAL,
					 g_param_spec_int ("autosave-interval",
							   "AutosaveInterval",
							   "Time between two autosaves",
							   0,
							   G_MAXINT,
							   0,
							   G_PARAM_READWRITE |
							   G_PARAM_STATIC_STRINGS));

	signals[DROP_URIS] =
		g_signal_new ("drop-uris",
			      G_TYPE_FROM_CLASS (object_class),
			      G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
			      G_STRUCT_OFFSET (GeditTabClass, drop_uris),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__BOXED,
			      G_TYPE_NONE,
			      1,
			      G_TYPE_STRV);

	g_type_class_add_private (object_class, sizeof (GeditTabPrivate));
}

/**
 * gedit_tab_get_state:
 * @tab: a #GeditTab
 *
 * Gets the #GeditTabState of @tab.
 *
 * Returns: the #GeditTabState of @tab
 */
GeditTabState
gedit_tab_get_state (GeditTab *tab)
{
	g_return_val_if_fail (GEDIT_IS_TAB (tab), GEDIT_TAB_STATE_NORMAL);
	
	return tab->priv->state;
}

static void
set_cursor_according_to_state (GtkTextView   *view,
			       GeditTabState  state)
{
	GdkCursor *cursor;
	GdkWindow *text_window;
	GdkWindow *left_window;

	text_window = gtk_text_view_get_window (view, GTK_TEXT_WINDOW_TEXT);
	left_window = gtk_text_view_get_window (view, GTK_TEXT_WINDOW_LEFT);

	if ((state == GEDIT_TAB_STATE_LOADING)          ||
	    (state == GEDIT_TAB_STATE_REVERTING)        ||
	    (state == GEDIT_TAB_STATE_SAVING)           ||
	    (state == GEDIT_TAB_STATE_PRINTING)         ||
	    (state == GEDIT_TAB_STATE_PRINT_PREVIEWING) ||
	    (state == GEDIT_TAB_STATE_CLOSING))
	{
		cursor = gdk_cursor_new_for_display (
				gtk_widget_get_display (GTK_WIDGET (view)),
				GDK_WATCH);

		if (text_window != NULL)
			gdk_window_set_cursor (text_window, cursor);
		if (left_window != NULL)
			gdk_window_set_cursor (left_window, cursor);

		gdk_cursor_unref (cursor);
	}
	else
	{
		cursor = gdk_cursor_new_for_display (
				gtk_widget_get_display (GTK_WIDGET (view)),
				GDK_XTERM);

		if (text_window != NULL)
			gdk_window_set_cursor (text_window, cursor);
		if (left_window != NULL)
			gdk_window_set_cursor (left_window, NULL);

		gdk_cursor_unref (cursor);
	}
}

static void
view_realized (GtkTextView *view,
	       GeditTab    *tab)
{
	set_cursor_according_to_state (view, tab->priv->state);
}

static void
set_view_properties_according_to_state (GeditTab      *tab,
					GeditTabState  state)
{
	GeditView *view;
	gboolean val;
	gboolean hl_current_line;
	
	hl_current_line = g_settings_get_boolean (tab->priv->editor,
						  GEDIT_SETTINGS_HIGHLIGHT_CURRENT_LINE);

	view = gedit_view_frame_get_view (tab->priv->frame);

	val = ((state == GEDIT_TAB_STATE_NORMAL) &&
	       (tab->priv->print_preview == NULL) &&
	       !tab->priv->not_editable);
	gtk_text_view_set_editable (GTK_TEXT_VIEW (view), val);

	val = ((state != GEDIT_TAB_STATE_LOADING) &&
	       (state != GEDIT_TAB_STATE_CLOSING));
	gtk_text_view_set_cursor_visible (GTK_TEXT_VIEW (view), val);

	val = ((state != GEDIT_TAB_STATE_LOADING) &&
	       (state != GEDIT_TAB_STATE_CLOSING) &&
	       (hl_current_line));
	gtk_source_view_set_highlight_current_line (GTK_SOURCE_VIEW (view), val);
}

static void
gedit_tab_set_state (GeditTab      *tab,
		     GeditTabState  state)
{
	g_return_if_fail (GEDIT_IS_TAB (tab));
	g_return_if_fail ((state >= 0) && (state < GEDIT_TAB_NUM_OF_STATES));

	if (tab->priv->state == state)
		return;

	tab->priv->state = state;

	set_view_properties_according_to_state (tab, state);

	if ((state == GEDIT_TAB_STATE_LOADING_ERROR) || /* FIXME: add other states if needed */
	    (state == GEDIT_TAB_STATE_SHOWING_PRINT_PREVIEW))
	{
		gtk_widget_hide (GTK_WIDGET (tab->priv->frame));
	}
	else
	{
		if (tab->priv->print_preview == NULL)
			gtk_widget_show (GTK_WIDGET (tab->priv->frame));
	}

	set_cursor_according_to_state (GTK_TEXT_VIEW (gedit_view_frame_get_view (tab->priv->frame)),
				       state);

	g_object_notify (G_OBJECT (tab), "state");
}

static void 
document_location_notify_handler (GeditDocument *document,
				  GParamSpec    *pspec,
				  GeditTab      *tab)
{
	gedit_debug (DEBUG_TAB);
	
	/* Notify the change in the location */
	g_object_notify (G_OBJECT (tab), "name");
}

static void 
document_shortname_notify_handler (GeditDocument *document,
				   GParamSpec    *pspec,
				   GeditTab      *tab)
{
	gedit_debug (DEBUG_TAB);
	
	/* Notify the change in the shortname */
	g_object_notify (G_OBJECT (tab), "name");
}

static void
document_modified_changed (GtkTextBuffer *document,
			   GeditTab      *tab)
{
	g_object_notify (G_OBJECT (tab), "name");
}

static void
set_info_bar (GeditTab  *tab,
	      GtkWidget *info_bar)
{
	if (tab->priv->info_bar == info_bar)
		return;

	if (tab->priv->info_bar != NULL)
		gtk_widget_destroy (tab->priv->info_bar);

	tab->priv->info_bar = info_bar;

	if (info_bar == NULL)
		return;

	gtk_box_pack_start (GTK_BOX (tab),
			    tab->priv->info_bar,
			    FALSE,
			    FALSE,
			    0);		

	g_object_add_weak_pointer (G_OBJECT (tab->priv->info_bar), 
				   (gpointer *)&tab->priv->info_bar);
}

static void
remove_tab (GeditTab *tab)
{
	GeditNotebook *notebook;

	notebook = GEDIT_NOTEBOOK (gtk_widget_get_parent (GTK_WIDGET (tab)));

	gedit_notebook_remove_tab (notebook, tab);
}

static void 
io_loading_error_info_bar_response (GtkWidget *info_bar,
				    gint       response_id,
				    GeditTab  *tab)
{
	GeditDocument *doc;
	GeditView *view;
	GFile *location;
	const GeditEncoding *encoding;

	doc = gedit_tab_get_document (tab);
	g_return_if_fail (GEDIT_IS_DOCUMENT (doc));

	view = gedit_tab_get_view (tab);
	g_return_if_fail (GEDIT_IS_VIEW (view));

	location = gedit_document_get_location (doc);

	switch (response_id)
	{
		case GTK_RESPONSE_OK:
			g_return_if_fail (location != NULL);

			encoding = gedit_conversion_error_info_bar_get_encoding (
					GTK_WIDGET (info_bar));

			if (encoding != NULL)
			{
				tab->priv->tmp_encoding = encoding;
			}

			set_info_bar (tab, NULL);
			gedit_tab_set_state (tab, GEDIT_TAB_STATE_LOADING);

			g_return_if_fail (tab->priv->auto_save_timeout <= 0);

			gedit_document_load (doc,
					     location,
					     tab->priv->tmp_encoding,
					     tab->priv->tmp_line_pos,
					     tab->priv->tmp_column_pos,
					     FALSE);
			break;
		case GTK_RESPONSE_YES:
			/* This means that we want to edit the document anyway */
			set_info_bar (tab, NULL);
			_gedit_document_set_readonly (doc, FALSE);
			break;
		case GTK_RESPONSE_NO:
			/* We don't want to edit the document just show it */
			set_info_bar (tab, NULL);
			break;
		default:
			if (location != NULL)
			{
				_gedit_recent_remove (GEDIT_WINDOW (gtk_widget_get_toplevel (GTK_WIDGET (tab))), location);
			}

			remove_tab (tab);
			break;
	}

	if (location != NULL)
	{
		g_object_unref (location);
	}
}

static void 
file_already_open_warning_info_bar_response (GtkWidget   *info_bar,
					     gint         response_id,
					     GeditTab    *tab)
{
	GeditView *view;
	
	view = gedit_tab_get_view (tab);
	
	if (response_id == GTK_RESPONSE_YES)
	{
		tab->priv->not_editable = FALSE;
		
		gtk_text_view_set_editable (GTK_TEXT_VIEW (view),
					    TRUE);
	}

	gtk_widget_destroy (info_bar);

	gtk_widget_grab_focus (GTK_WIDGET (view));
}

static void
load_cancelled (GtkWidget        *bar,
                gint              response_id,
                GeditTab         *tab)
{
	g_return_if_fail (GEDIT_IS_PROGRESS_INFO_BAR (tab->priv->info_bar));
	
	g_object_ref (tab);
	gedit_document_load_cancel (gedit_tab_get_document (tab));
	g_object_unref (tab);	
}

static void 
unrecoverable_reverting_error_info_bar_response (GtkWidget        *info_bar,
						 gint              response_id,
						 GeditTab         *tab)
{
	GeditView *view;
	
	gedit_tab_set_state (tab,
			     GEDIT_TAB_STATE_NORMAL);

	set_info_bar (tab, NULL);

	view = gedit_tab_get_view (tab);

	gtk_widget_grab_focus (GTK_WIDGET (view));
	
	install_auto_save_timeout_if_needed (tab);	
}

#define MAX_MSG_LENGTH 100

static void
show_loading_info_bar (GeditTab *tab)
{
	GtkWidget *bar;
	GeditDocument *doc = NULL;
	gchar *name;
	gchar *dirname = NULL;
	gchar *msg = NULL;
	gchar *name_markup;
	gchar *dirname_markup;
	gint len;

	if (tab->priv->info_bar != NULL)
		return;

	gedit_debug (DEBUG_TAB);
		
	doc = gedit_tab_get_document (tab);
	g_return_if_fail (doc != NULL);

	name = gedit_document_get_short_name_for_display (doc);
	len = g_utf8_strlen (name, -1);

	/* if the name is awfully long, truncate it and be done with it,
	 * otherwise also show the directory (ellipsized if needed)
	 */
	if (len > MAX_MSG_LENGTH)
	{
		gchar *str;

		str = gedit_utils_str_middle_truncate (name, MAX_MSG_LENGTH);
		g_free (name);
		name = str;
	}
	else
	{
		GFile *location;

		location = gedit_document_get_location (doc);
		if (location != NULL)
		{
			gchar *str;

			str = gedit_utils_location_get_dirname_for_display (location);
			g_object_unref (location);

			/* use the remaining space for the dir, but use a min of 20 chars
			 * so that we do not end up with a dirname like "(a...b)".
			 * This means that in the worst case when the filename is long 99
			 * we have a title long 99 + 20, but I think it's a rare enough
			 * case to be acceptable. It's justa darn title afterall :)
			 */
			dirname = gedit_utils_str_middle_truncate (str, 
								   MAX (20, MAX_MSG_LENGTH - len));
			g_free (str);
		}
	}

	name_markup = g_markup_printf_escaped ("<b>%s</b>", name);

	if (tab->priv->state == GEDIT_TAB_STATE_REVERTING)
	{
		if (dirname != NULL)
		{
			dirname_markup = g_markup_printf_escaped ("<b>%s</b>", dirname);

			/* Translators: the first %s is a file name (e.g. test.txt) the second one
			   is a directory (e.g. ssh://master.gnome.org/home/users/paolo) */
			msg = g_strdup_printf (_("Reverting %s from %s"),
					       name_markup,
					       dirname_markup);
			g_free (dirname_markup);
		}
		else
		{
			msg = g_strdup_printf (_("Reverting %s"), 
					       name_markup);
		}
		
		bar = gedit_progress_info_bar_new (GTK_STOCK_REVERT_TO_SAVED,
						   msg,
						   TRUE);
	}
	else
	{
		if (dirname != NULL)
		{
			dirname_markup = g_markup_printf_escaped ("<b>%s</b>", dirname);

			/* Translators: the first %s is a file name (e.g. test.txt) the second one
			   is a directory (e.g. ssh://master.gnome.org/home/users/paolo) */
			msg = g_strdup_printf (_("Loading %s from %s"),
					       name_markup,
					       dirname_markup);
			g_free (dirname_markup);
		}
		else
		{
			msg = g_strdup_printf (_("Loading %s"), 
					       name_markup);
		}

		bar = gedit_progress_info_bar_new (GTK_STOCK_OPEN,
						   msg,
						   TRUE);
	}

	g_signal_connect (bar,
			  "response",
			  G_CALLBACK (load_cancelled),
			  tab);
						 
	gtk_widget_show (bar);

	set_info_bar (tab, bar);

	g_free (msg);
	g_free (name);
	g_free (name_markup);
	g_free (dirname);
}

static void
show_saving_info_bar (GeditTab *tab)
{
	GtkWidget *bar;
	GeditDocument *doc = NULL;
	gchar *short_name;
	gchar *from;
	gchar *to = NULL;
	gchar *from_markup;
	gchar *to_markup;
	gchar *msg = NULL;
	gint len;

	g_return_if_fail (tab->priv->tmp_save_location != NULL);
	
	if (tab->priv->info_bar != NULL)
		return;
	
	gedit_debug (DEBUG_TAB);
		
	doc = gedit_tab_get_document (tab);
	g_return_if_fail (doc != NULL);

	short_name = gedit_document_get_short_name_for_display (doc);

	len = g_utf8_strlen (short_name, -1);

	/* if the name is awfully long, truncate it and be done with it,
	 * otherwise also show the directory (ellipsized if needed)
	 */
	if (len > MAX_MSG_LENGTH)
	{
		from = gedit_utils_str_middle_truncate (short_name, 
							MAX_MSG_LENGTH);
		g_free (short_name);
	}
	else
	{
		gchar *str;

		from = short_name;
		to = g_file_get_parse_name (tab->priv->tmp_save_location);
		str = gedit_utils_str_middle_truncate (to, 
						       MAX (20, MAX_MSG_LENGTH - len));
		g_free (to);
			
		to = str;
	}

	from_markup = g_markup_printf_escaped ("<b>%s</b>", from);

	if (to != NULL)
	{
		to_markup = g_markup_printf_escaped ("<b>%s</b>", to);

		/* Translators: the first %s is a file name (e.g. test.txt) the second one
		   is a directory (e.g. ssh://master.gnome.org/home/users/paolo) */
		msg = g_strdup_printf (_("Saving %s to %s"),
				       from_markup,
				       to_markup);
		g_free (to_markup);
	}
	else
	{
		msg = g_strdup_printf (_("Saving %s"), from_markup);
	}

	bar = gedit_progress_info_bar_new (GTK_STOCK_SAVE,
					   msg,
					   FALSE);

	gtk_widget_show (bar);

	set_info_bar (tab, bar);

	g_free (msg);
	g_free (to);
	g_free (from);
	g_free (from_markup);
}

static void
info_bar_set_progress (GeditTab *tab,
			   goffset   size,
			   goffset   total_size)
{
	if (tab->priv->info_bar == NULL)
		return;

	gedit_debug_message (DEBUG_TAB, "%" G_GUINT64_FORMAT "/%" G_GUINT64_FORMAT, size, total_size);

	g_return_if_fail (GEDIT_IS_PROGRESS_INFO_BAR (tab->priv->info_bar));
	
	if (total_size == 0)
	{
		if (size != 0)
			gedit_progress_info_bar_pulse (
					GEDIT_PROGRESS_INFO_BAR (tab->priv->info_bar));	
		else
			gedit_progress_info_bar_set_fraction (
				GEDIT_PROGRESS_INFO_BAR (tab->priv->info_bar),
				0);
	}
	else
	{
		gdouble frac;

		frac = (gdouble)size / (gdouble)total_size;

		gedit_progress_info_bar_set_fraction (
				GEDIT_PROGRESS_INFO_BAR (tab->priv->info_bar),
				frac);		
	}
}

static void
document_loading (GeditDocument *document,
		  goffset        size,
		  goffset        total_size,
		  GeditTab      *tab)
{
	gdouble et;
	gdouble total_time;

	g_return_if_fail ((tab->priv->state == GEDIT_TAB_STATE_LOADING) ||
		 	  (tab->priv->state == GEDIT_TAB_STATE_REVERTING));

	gedit_debug_message (DEBUG_TAB, "%" G_GUINT64_FORMAT "/%" G_GUINT64_FORMAT, size, total_size);

	if (tab->priv->timer == NULL)
	{
		g_return_if_fail (tab->priv->times_called == 0);
		tab->priv->timer = g_timer_new ();
	}

	et = g_timer_elapsed (tab->priv->timer, NULL);

	/* et : total_time = size : total_size */
	total_time = (et * total_size) / size;

	if ((total_time - et) > 3.0)
	{
		show_loading_info_bar (tab);
	}

	info_bar_set_progress (tab, size, total_size);
}

static gboolean
remove_tab_idle (GeditTab *tab)
{
	remove_tab (tab);

	return FALSE;
}

static void
document_loaded (GeditDocument *document,
		 const GError  *error,
		 GeditTab      *tab)
{
	GtkWidget *emsg;
	GFile *location;
	const GeditEncoding *encoding;

	g_return_if_fail ((tab->priv->state == GEDIT_TAB_STATE_LOADING) ||
			  (tab->priv->state == GEDIT_TAB_STATE_REVERTING));
	g_return_if_fail (tab->priv->auto_save_timeout <= 0);

	if (tab->priv->timer != NULL)
	{
		g_timer_destroy (tab->priv->timer);
		tab->priv->timer = NULL;
	}
	tab->priv->times_called = 0;

	set_info_bar (tab, NULL);

	location = gedit_document_get_location (document);

	/* if the error is CONVERSION FALLBACK don't treat it as a normal error */
	if (error != NULL &&
	    (error->domain != GEDIT_DOCUMENT_ERROR || error->code != GEDIT_DOCUMENT_ERROR_CONVERSION_FALLBACK))
	{
		if (tab->priv->state == GEDIT_TAB_STATE_LOADING)
			gedit_tab_set_state (tab, GEDIT_TAB_STATE_LOADING_ERROR);
		else
			gedit_tab_set_state (tab, GEDIT_TAB_STATE_REVERTING_ERROR);

		encoding = gedit_document_get_encoding (document);

		if (error->domain == G_IO_ERROR &&
		    error->code == G_IO_ERROR_CANCELLED)
		{
			/* remove the tab, but in an idle handler, since
			 * we are in the handler of doc loaded and we 
			 * don't want doc and tab to be finalized now.
			 */
			g_idle_add ((GSourceFunc) remove_tab_idle, tab);

			goto end;
		}
		else
		{
			if (location)
			{
				_gedit_recent_remove (GEDIT_WINDOW (gtk_widget_get_toplevel (GTK_WIDGET (tab))), location);
			}

			if (tab->priv->state == GEDIT_TAB_STATE_LOADING_ERROR)
			{
				emsg = gedit_io_loading_error_info_bar_new (location,
										tab->priv->tmp_encoding,
										error);
				g_signal_connect (emsg,
						  "response",
						  G_CALLBACK (io_loading_error_info_bar_response),
						  tab);
			}
			else
			{
				g_return_if_fail (tab->priv->state == GEDIT_TAB_STATE_REVERTING_ERROR);
				
				emsg = gedit_unrecoverable_reverting_error_info_bar_new (location,
											     error);

				g_signal_connect (emsg,
						  "response",
						  G_CALLBACK (unrecoverable_reverting_error_info_bar_response),
						  tab);
			}

			set_info_bar (tab, emsg);
		}

		gtk_info_bar_set_default_response (GTK_INFO_BAR (emsg),
						   GTK_RESPONSE_CANCEL);

		gtk_widget_show (emsg);

		if (location)
		{
			g_object_unref (location);
		}

		return;
	}
	else
	{
		GList *all_documents;
		GList *l;

		if (location != NULL)
		{
			gchar *mime;
			mime = gedit_document_get_mime_type (document);

			_gedit_recent_add (GEDIT_WINDOW (gtk_widget_get_toplevel (GTK_WIDGET (tab))),
					   location,
					   mime);
			g_free (mime);
		}

		if (error &&
		    error->domain == GEDIT_DOCUMENT_ERROR &&
		    error->code == GEDIT_DOCUMENT_ERROR_CONVERSION_FALLBACK)
		{
			GtkWidget *emsg;

			_gedit_document_set_readonly (document, TRUE);

			emsg = gedit_io_loading_error_info_bar_new (location,
									tab->priv->tmp_encoding,
									error);

			set_info_bar (tab, emsg);

			g_signal_connect (emsg,
					  "response",
					  G_CALLBACK (io_loading_error_info_bar_response),
					  tab);

			gtk_info_bar_set_default_response (GTK_INFO_BAR (emsg),
							   GTK_RESPONSE_CANCEL);

			gtk_widget_show (emsg);
		}

		/* Scroll to the cursor when the document is loaded */
		gedit_view_scroll_to_cursor (gedit_view_frame_get_view (tab->priv->frame));

		all_documents = gedit_app_get_documents (gedit_app_get_default ());

		for (l = all_documents; l != NULL; l = g_list_next (l))
		{
			GeditDocument *d = GEDIT_DOCUMENT (l->data);
			
			if (d != document)
			{
				GFile *loc;

				loc = gedit_document_get_location (d);

				if (loc != NULL && location != NULL &&
			    	    g_file_equal (location, loc))
			    	{
			    		GtkWidget *w;

			    		tab->priv->not_editable = TRUE;

			    		w = gedit_file_already_open_warning_info_bar_new (location);

					set_info_bar (tab, w);

					gtk_info_bar_set_default_response (GTK_INFO_BAR (w),
									   GTK_RESPONSE_CANCEL);

					gtk_widget_show (w);

					g_signal_connect (w,
							  "response",
							  G_CALLBACK (file_already_open_warning_info_bar_response),
							  tab);

			    		g_object_unref (loc);
			    		break;
			    	}
			    	
			    	if (loc != NULL)
					g_object_unref (loc);
			}
		}

		g_list_free (all_documents);

		gedit_tab_set_state (tab, GEDIT_TAB_STATE_NORMAL);

		if (location == NULL)
		{
			/* FIXME: hackish */
			gtk_text_buffer_set_modified (GTK_TEXT_BUFFER (gedit_tab_get_document (tab)),
			                              TRUE);
		}

		install_auto_save_timeout_if_needed (tab);

		tab->priv->ask_if_externally_modified = TRUE;
	}

 end:
 	if (location)
 	{
		g_object_unref (location);
	}

	tab->priv->tmp_line_pos = 0;
	tab->priv->tmp_encoding = NULL;
}

static void
document_saving (GeditDocument *document,
		 goffset        size,
		 goffset        total_size,
		 GeditTab      *tab)
{
	gdouble et;
	gdouble total_time;

	g_return_if_fail (tab->priv->state == GEDIT_TAB_STATE_SAVING);

	gedit_debug_message (DEBUG_TAB, "%" G_GUINT64_FORMAT "/%" G_GUINT64_FORMAT, size, total_size);


	if (tab->priv->timer == NULL)
	{
		g_return_if_fail (tab->priv->times_called == 0);
		tab->priv->timer = g_timer_new ();
	}

	et = g_timer_elapsed (tab->priv->timer, NULL);

	/* et : total_time = size : total_size */
	total_time = (et * total_size)/size;

	if ((total_time - et) > 3.0)
	{
		show_saving_info_bar (tab);
	}

	info_bar_set_progress (tab, size, total_size);

	tab->priv->times_called++;
}

static void
end_saving (GeditTab *tab)
{
	/* Reset tmp data for saving */
	if (tab->priv->tmp_save_location)
	{
		g_object_unref (tab->priv->tmp_save_location);
		tab->priv->tmp_save_location = NULL;
	}
	tab->priv->tmp_encoding = NULL;
	
	install_auto_save_timeout_if_needed (tab);
}

static void 
unrecoverable_saving_error_info_bar_response (GtkWidget        *info_bar,
						  gint              response_id,
						  GeditTab         *tab)
{
	GeditView *view;
	
	if (tab->priv->print_preview != NULL)
		gedit_tab_set_state (tab, GEDIT_TAB_STATE_SHOWING_PRINT_PREVIEW);
	else
		gedit_tab_set_state (tab, GEDIT_TAB_STATE_NORMAL);

	end_saving (tab);
	
	set_info_bar (tab, NULL);

	view = gedit_tab_get_view (tab);

	gtk_widget_grab_focus (GTK_WIDGET (view));
}

static void 
no_backup_error_info_bar_response (GtkWidget *info_bar,
				       gint       response_id,
				       GeditTab  *tab)
{
	if (response_id == GTK_RESPONSE_YES)
	{
		GeditDocument *doc;

		doc = gedit_tab_get_document (tab);
		g_return_if_fail (GEDIT_IS_DOCUMENT (doc));

		set_info_bar (tab, NULL);

		g_return_if_fail (tab->priv->tmp_save_location != NULL);
		g_return_if_fail (tab->priv->tmp_encoding != NULL);

		gedit_tab_set_state (tab, GEDIT_TAB_STATE_SAVING);

		/* don't bug the user again with this... */
		tab->priv->save_flags |= GEDIT_DOCUMENT_SAVE_IGNORE_BACKUP;

		g_return_if_fail (tab->priv->auto_save_timeout <= 0);
		
		/* Force saving */
		gedit_document_save (doc, tab->priv->save_flags);
	}
	else
	{
		unrecoverable_saving_error_info_bar_response (info_bar,
								  response_id,
								  tab);
	}
}

static void
externally_modified_error_info_bar_response (GtkWidget *info_bar,
						 gint       response_id,
						 GeditTab  *tab)
{
	if (response_id == GTK_RESPONSE_YES)
	{
		GeditDocument *doc;

		doc = gedit_tab_get_document (tab);
		g_return_if_fail (GEDIT_IS_DOCUMENT (doc));
		
		set_info_bar (tab, NULL);

		g_return_if_fail (tab->priv->tmp_save_location != NULL);
		g_return_if_fail (tab->priv->tmp_encoding != NULL);

		gedit_tab_set_state (tab, GEDIT_TAB_STATE_SAVING);

		g_return_if_fail (tab->priv->auto_save_timeout <= 0);
		
		/* ignore mtime should not be persisted in save flags across saves */

		/* Force saving */
		gedit_document_save (doc, tab->priv->save_flags | GEDIT_DOCUMENT_SAVE_IGNORE_MTIME);
	}
	else
	{		
		unrecoverable_saving_error_info_bar_response (info_bar,
								  response_id,
								  tab);
	}
}

static void 
recoverable_saving_error_info_bar_response (GtkWidget *info_bar,
						gint       response_id,
						GeditTab  *tab)
{
	GeditDocument *doc;

	doc = gedit_tab_get_document (tab);
	g_return_if_fail (GEDIT_IS_DOCUMENT (doc));
	
	if (response_id == GTK_RESPONSE_OK)
	{
		const GeditEncoding *encoding;
		gchar *tmp_uri;
		
		encoding = gedit_conversion_error_info_bar_get_encoding (
									GTK_WIDGET (info_bar));

		g_return_if_fail (encoding != NULL);

		set_info_bar (tab, NULL);

		g_return_if_fail (tab->priv->tmp_save_location != NULL);
				
		gedit_tab_set_state (tab, GEDIT_TAB_STATE_SAVING);
			
		tab->priv->tmp_encoding = encoding;

		tmp_uri = g_file_get_uri (tab->priv->tmp_save_location);
		gedit_debug_message (DEBUG_TAB, "Force saving with URI '%s'", tmp_uri);
		g_free (tmp_uri);
			 
		g_return_if_fail (tab->priv->auto_save_timeout <= 0);
		
		gedit_document_save_as (doc,
					tab->priv->tmp_save_location,
					tab->priv->tmp_encoding,
					gedit_document_get_newline_type (doc),
					gedit_document_get_compression_type (doc),
					tab->priv->save_flags);
	}
	else
	{		
		unrecoverable_saving_error_info_bar_response (info_bar,
								  response_id,
								  tab);
	}
}

static void
document_saved (GeditDocument *document,
		const GError  *error,
		GeditTab      *tab)
{
	GtkWidget *emsg;

	g_return_if_fail (tab->priv->state == GEDIT_TAB_STATE_SAVING);

	g_return_if_fail (tab->priv->tmp_save_location != NULL);
	g_return_if_fail (tab->priv->tmp_encoding != NULL);
	g_return_if_fail (tab->priv->auto_save_timeout <= 0);
	
	g_timer_destroy (tab->priv->timer);
	tab->priv->timer = NULL;
	tab->priv->times_called = 0;
	
	set_info_bar (tab, NULL);
	
	if (error != NULL)
	{
		gedit_tab_set_state (tab, GEDIT_TAB_STATE_SAVING_ERROR);
		
		if (error->domain == GEDIT_DOCUMENT_ERROR &&
		    error->code == GEDIT_DOCUMENT_ERROR_EXTERNALLY_MODIFIED)
		{
			/* This error is recoverable */
			emsg = gedit_externally_modified_saving_error_info_bar_new (
							tab->priv->tmp_save_location,
							error);
			g_return_if_fail (emsg != NULL);

			set_info_bar (tab, emsg);

			g_signal_connect (emsg,
					  "response",
					  G_CALLBACK (externally_modified_error_info_bar_response),
					  tab);
		}
		else if ((error->domain == GEDIT_DOCUMENT_ERROR &&
			  error->code == GEDIT_DOCUMENT_ERROR_CANT_CREATE_BACKUP) ||
		         (error->domain == G_IO_ERROR &&
			  error->code == G_IO_ERROR_CANT_CREATE_BACKUP))
		{
			/* This error is recoverable */
			emsg = gedit_no_backup_saving_error_info_bar_new (
							tab->priv->tmp_save_location,
							error);
			g_return_if_fail (emsg != NULL);

			set_info_bar (tab, emsg);

			g_signal_connect (emsg,
					  "response",
					  G_CALLBACK (no_backup_error_info_bar_response),
					  tab);
		}
		else if (error->domain == GEDIT_DOCUMENT_ERROR ||
			 (error->domain == G_IO_ERROR &&
			  error->code != G_IO_ERROR_INVALID_DATA &&
			  error->code != G_IO_ERROR_PARTIAL_INPUT))
		{
			/* These errors are _NOT_ recoverable */
			_gedit_recent_remove  (GEDIT_WINDOW (gtk_widget_get_toplevel (GTK_WIDGET (tab))),
					       tab->priv->tmp_save_location);

			emsg = gedit_unrecoverable_saving_error_info_bar_new (tab->priv->tmp_save_location,
								  error);
			g_return_if_fail (emsg != NULL);
	
			set_info_bar (tab, emsg);

			g_signal_connect (emsg,
					  "response",
					  G_CALLBACK (unrecoverable_saving_error_info_bar_response),
					  tab);
		}
		else
		{
			/* This error is recoverable */
			g_return_if_fail (error->domain == G_CONVERT_ERROR ||
			                  error->domain == G_IO_ERROR);

			emsg = gedit_conversion_error_while_saving_info_bar_new (
									tab->priv->tmp_save_location,
									tab->priv->tmp_encoding,
									error);

			set_info_bar (tab, emsg);

			g_signal_connect (emsg,
					  "response",
					  G_CALLBACK (recoverable_saving_error_info_bar_response),
					  tab);
		}

		gtk_info_bar_set_default_response (GTK_INFO_BAR (emsg),
						   GTK_RESPONSE_CANCEL);

		gtk_widget_show (emsg);
	}
	else
	{
		gchar *mime = gedit_document_get_mime_type (document);

		_gedit_recent_add (GEDIT_WINDOW (gtk_widget_get_toplevel (GTK_WIDGET (tab))),
				   tab->priv->tmp_save_location,
				   mime);
		g_free (mime);

		if (tab->priv->print_preview != NULL)
			gedit_tab_set_state (tab, GEDIT_TAB_STATE_SHOWING_PRINT_PREVIEW);
		else
			gedit_tab_set_state (tab, GEDIT_TAB_STATE_NORMAL);

		tab->priv->ask_if_externally_modified = TRUE;
			
		end_saving (tab);
	}
}

static void 
externally_modified_notification_info_bar_response (GtkWidget *info_bar,
							gint       response_id,
							GeditTab  *tab)
{
	GeditView *view;

	set_info_bar (tab, NULL);
	view = gedit_tab_get_view (tab);

	if (response_id == GTK_RESPONSE_OK)
	{
		_gedit_tab_revert (tab);
	}
	else
	{
		tab->priv->ask_if_externally_modified = FALSE;

		/* go back to normal state */
		gedit_tab_set_state (tab, GEDIT_TAB_STATE_NORMAL);
	}

	gtk_widget_grab_focus (GTK_WIDGET (view));
}

static void
display_externally_modified_notification (GeditTab *tab)
{
	GtkWidget *info_bar;
	GeditDocument *doc;
	GFile *location;
	gboolean document_modified;

	doc = gedit_tab_get_document (tab);
	g_return_if_fail (GEDIT_IS_DOCUMENT (doc));

	/* we're here because the file we're editing changed on disk */
	location = gedit_document_get_location (doc);
	g_return_if_fail (location != NULL);

	document_modified = gtk_text_buffer_get_modified (GTK_TEXT_BUFFER(doc));
	info_bar = gedit_externally_modified_info_bar_new (location, document_modified);
	g_object_unref (location);

	tab->priv->info_bar = NULL;
	set_info_bar (tab, info_bar);
	gtk_widget_show (info_bar);

	g_signal_connect (info_bar,
			  "response",
			  G_CALLBACK (externally_modified_notification_info_bar_response),
			  tab);
}

static gboolean
view_focused_in (GtkWidget     *widget,
                 GdkEventFocus *event,
                 GeditTab      *tab)
{
	GeditDocument *doc;

	g_return_val_if_fail (GEDIT_IS_TAB (tab), FALSE);

	/* we try to detect file changes only in the normal state */
	if (tab->priv->state != GEDIT_TAB_STATE_NORMAL)
	{
		return FALSE;
	}

	/* we already asked, don't bug the user again */
	if (!tab->priv->ask_if_externally_modified)
	{
		return FALSE;
	}

	doc = gedit_tab_get_document (tab);

	/* If file was never saved or is remote we do not check */
	if (!gedit_document_is_local (doc))
	{
		return FALSE;
	}

	if (_gedit_document_check_externally_modified (doc))
	{
		gedit_tab_set_state (tab, GEDIT_TAB_STATE_EXTERNALLY_MODIFIED_NOTIFICATION);

		display_externally_modified_notification (tab);

		return FALSE;
	}

	return FALSE;
}

static void
on_drop_uris (GeditView *view,
	      gchar    **uri_list,
	      GeditTab  *tab)
{
	g_signal_emit (G_OBJECT (tab), signals[DROP_URIS], 0, uri_list);
}

static void
gedit_tab_init (GeditTab *tab)
{
	GeditLockdownMask lockdown;
	gboolean auto_save;
	gint auto_save_interval;
	GeditDocument *doc;
	GeditView *view;

	tab->priv = GEDIT_TAB_GET_PRIVATE (tab);

	tab->priv->editor = g_settings_new ("org.gnome.gedit.preferences.editor");

	tab->priv->state = GEDIT_TAB_STATE_NORMAL;

	tab->priv->not_editable = FALSE;

	tab->priv->save_flags = 0;

	tab->priv->ask_if_externally_modified = TRUE;

	/* Manage auto save data */
	auto_save = g_settings_get_boolean (tab->priv->editor,
					    GEDIT_SETTINGS_AUTO_SAVE);
	g_settings_get (tab->priv->editor, GEDIT_SETTINGS_AUTO_SAVE_INTERVAL,
			"u", &auto_save_interval);
	
	lockdown = gedit_app_get_lockdown (gedit_app_get_default ());
	tab->priv->auto_save = auto_save &&
			       !(lockdown & GEDIT_LOCKDOWN_SAVE_TO_DISK);
	tab->priv->auto_save = (tab->priv->auto_save != FALSE);

	tab->priv->auto_save_interval = auto_save_interval;

	/* Create the frame */
	tab->priv->frame = gedit_view_frame_new ();
	gtk_widget_show (GTK_WIDGET (tab->priv->frame));

	gtk_box_pack_end (GTK_BOX (tab), GTK_WIDGET (tab->priv->frame),
	                  TRUE, TRUE, 0);

	doc = gedit_view_frame_get_document (tab->priv->frame);
	g_object_set_data (G_OBJECT (doc), GEDIT_TAB_KEY, tab);

	view = gedit_view_frame_get_view (tab->priv->frame);
	g_object_set_data (G_OBJECT (view), GEDIT_TAB_KEY, tab);

	g_signal_connect (doc,
			  "notify::location",
			  G_CALLBACK (document_location_notify_handler),
			  tab);
	g_signal_connect (doc,
			  "notify::shortname",
			  G_CALLBACK (document_shortname_notify_handler),
			  tab);
	g_signal_connect (doc,
			  "modified_changed",
			  G_CALLBACK (document_modified_changed),
			  tab);
	g_signal_connect (doc,
			  "loading",
			  G_CALLBACK (document_loading),
			  tab);
	g_signal_connect (doc,
			  "loaded",
			  G_CALLBACK (document_loaded),
			  tab);
	g_signal_connect (doc,
			  "saving",
			  G_CALLBACK (document_saving),
			  tab);
	g_signal_connect (doc,
			  "saved",
			  G_CALLBACK (document_saved),
			  tab);

	g_signal_connect_after (view,
				"focus-in-event",
				G_CALLBACK (view_focused_in),
				tab);

	g_signal_connect_after (view,
				"realize",
				G_CALLBACK (view_realized),
				tab);

	g_signal_connect (view,
			  "drop-uris",
			  G_CALLBACK (on_drop_uris),
			  tab);
}

GtkWidget *
_gedit_tab_new (void)
{
	return GTK_WIDGET (g_object_new (GEDIT_TYPE_TAB, NULL));
}

/* Whether create is TRUE, creates a new empty document if location does 
   not refer to an existing location */
GtkWidget *
_gedit_tab_new_from_location (GFile               *location,
			      const GeditEncoding *encoding,
			      gint                 line_pos,
			      gint                 column_pos,
			      gboolean             create)
{
	GeditTab *tab;

	g_return_val_if_fail (G_IS_FILE (location), NULL);

	tab = GEDIT_TAB (_gedit_tab_new ());

	_gedit_tab_load (tab,
			 location,
			 encoding,
			 line_pos,
			 column_pos,
			 create);

	return GTK_WIDGET (tab);
}

GtkWidget *
_gedit_tab_new_from_stream (GInputStream        *stream,
                            const GeditEncoding *encoding,
                            gint                 line_pos,
                            gint                 column_pos)
{
	GeditTab *tab;

	g_return_val_if_fail (G_IS_INPUT_STREAM (stream), NULL);

	tab = GEDIT_TAB (_gedit_tab_new ());

	_gedit_tab_load_stream (tab,
	                        stream,
	                        encoding,
	                        line_pos,
	                        column_pos);

	return GTK_WIDGET (tab);

}

/**
 * gedit_tab_get_view:
 * @tab: a #GeditTab
 *
 * Gets the #GeditView inside @tab.
 *
 * Returns: (transfer none): the #GeditView inside @tab
 */
GeditView *
gedit_tab_get_view (GeditTab *tab)
{
	g_return_val_if_fail (GEDIT_IS_TAB (tab), NULL);

	return gedit_view_frame_get_view (tab->priv->frame);
}

/**
 * gedit_tab_get_document:
 * @tab: a #GeditTab
 *
 * Gets the #GeditDocument associated to @tab.
 *
 * Returns: (transfer none): the #GeditDocument associated to @tab
 */
GeditDocument *
gedit_tab_get_document (GeditTab *tab)
{
	g_return_val_if_fail (GEDIT_IS_TAB (tab), NULL);

	return gedit_view_frame_get_document (tab->priv->frame);
}

#define MAX_DOC_NAME_LENGTH 40

gchar *
_gedit_tab_get_name (GeditTab *tab)
{
	GeditDocument *doc;
	gchar *name;
	gchar *docname;
	gchar *tab_name;

	g_return_val_if_fail (GEDIT_IS_TAB (tab), NULL);

	doc = gedit_tab_get_document (tab);

	name = gedit_document_get_short_name_for_display (doc);

	/* Truncate the name so it doesn't get insanely wide. */
	docname = gedit_utils_str_middle_truncate (name, MAX_DOC_NAME_LENGTH);

	if (gtk_text_buffer_get_modified (GTK_TEXT_BUFFER (doc)))
	{
		tab_name = g_strdup_printf ("*%s", docname);
	} 
	else 
	{
 #if 0		
		if (gedit_document_get_readonly (doc)) 
		{
			tab_name = g_strdup_printf ("%s [%s]", docname, 
						/*Read only*/ _("RO"));
		} 
		else 
		{
			tab_name = g_strdup_printf ("%s", docname);
		}
#endif
		tab_name = g_strdup (docname);
	}
	
	g_free (docname);
	g_free (name);

	return tab_name;
}

gchar *
_gedit_tab_get_tooltip (GeditTab *tab)
{
	GeditDocument *doc;
	gchar *tip;
	gchar *uri;
	gchar *ruri;
	gchar *ruri_markup;

	g_return_val_if_fail (GEDIT_IS_TAB (tab), NULL);

	doc = gedit_tab_get_document (tab);

	uri = gedit_document_get_uri_for_display (doc);
	g_return_val_if_fail (uri != NULL, NULL);

	ruri = 	gedit_utils_replace_home_dir_with_tilde (uri);
	g_free (uri);

	ruri_markup = g_markup_printf_escaped ("<i>%s</i>", ruri);

	switch (tab->priv->state)
	{
		gchar *content_type;
		gchar *mime_type;
		gchar *content_description;
		gchar *content_full_description; 
		gchar *encoding;
		const GeditEncoding *enc;

		case GEDIT_TAB_STATE_LOADING_ERROR:
			tip = g_strdup_printf (_("Error opening file %s"),
					       ruri_markup);
			break;

		case GEDIT_TAB_STATE_REVERTING_ERROR:
			tip = g_strdup_printf (_("Error reverting file %s"),
					       ruri_markup);
			break;			

		case GEDIT_TAB_STATE_SAVING_ERROR:
			tip =  g_strdup_printf (_("Error saving file %s"),
						ruri_markup);
			break;			
		default:
			content_type = gedit_document_get_content_type (doc);
			mime_type = gedit_document_get_mime_type (doc);
			content_description = g_content_type_get_description (content_type);

			if (content_description == NULL)
				content_full_description = g_strdup (mime_type);
			else
				content_full_description = g_strdup_printf ("%s (%s)", 
						content_description, mime_type);

			g_free (content_type);
			g_free (mime_type);
			g_free (content_description);

			enc = gedit_document_get_encoding (doc);

			if (enc == NULL)
				encoding = g_strdup (_("Unicode (UTF-8)"));
			else
				encoding = gedit_encoding_to_string (enc);

			tip =  g_markup_printf_escaped ("<b>%s</b> %s\n\n"
						        "<b>%s</b> %s\n"
						        "<b>%s</b> %s",
						        _("Name:"), ruri,
						        _("MIME Type:"), content_full_description,
						        _("Encoding:"), encoding);

			g_free (encoding);
			g_free (content_full_description);

			break;
	}

	g_free (ruri);	
	g_free (ruri_markup);
	
	return tip;
}

static GdkPixbuf *
resize_icon (GdkPixbuf *pixbuf,
	     gint       size)
{
	gint width, height;

	width = gdk_pixbuf_get_width (pixbuf); 
	height = gdk_pixbuf_get_height (pixbuf);

	/* if the icon is larger than the nominal size, scale down */
	if (MAX (width, height) > size) 
	{
		GdkPixbuf *scaled_pixbuf;
		
		if (width > height) 
		{
			height = height * size / width;
			width = size;
		} 
		else 
		{
			width = width * size / height;
			height = size;
		}
		
		scaled_pixbuf = gdk_pixbuf_scale_simple	(pixbuf, 
							 width, 
							 height, 
							 GDK_INTERP_BILINEAR);
		g_object_unref (pixbuf);
		pixbuf = scaled_pixbuf;
	}

	return pixbuf;
}

static GdkPixbuf *
get_stock_icon (GtkIconTheme *theme, 
		const gchar  *stock,
		gint          size)
{
	GdkPixbuf *pixbuf;
	
	pixbuf = gtk_icon_theme_load_icon (theme, stock, size, 0, NULL);
	if (pixbuf == NULL)
		return NULL;
		
	return resize_icon (pixbuf, size);
}

static GdkPixbuf *
get_icon (GtkIconTheme *theme, 
	  GFile        *location,
	  gint          size)
{
	GdkPixbuf *pixbuf;
	GtkIconInfo *icon_info;
	GFileInfo *info;
	GIcon *gicon;

	if (location == NULL)
		return get_stock_icon (theme, GTK_STOCK_FILE, size);

	/* FIXME: Doing a sync stat is bad, this should be fixed */
	info = g_file_query_info (location,
	                          G_FILE_ATTRIBUTE_STANDARD_ICON, 
	                          G_FILE_QUERY_INFO_NONE, 
	                          NULL, 
	                          NULL);
	if (info == NULL)
		return get_stock_icon (theme, GTK_STOCK_FILE, size);

	gicon = g_file_info_get_icon (info);

	if (gicon == NULL)
	{
		g_object_unref (info);
		return get_stock_icon (theme, GTK_STOCK_FILE, size);
	}

	icon_info = gtk_icon_theme_lookup_by_gicon (theme, gicon, size, 0);
	g_object_unref (info);	
	
	if (icon_info == NULL)
		return get_stock_icon (theme, GTK_STOCK_FILE, size);
	
	pixbuf = gtk_icon_info_load_icon (icon_info, NULL);
	gtk_icon_info_free (icon_info);
	
	if (pixbuf == NULL)
		return get_stock_icon (theme, GTK_STOCK_FILE, size);
		
	return resize_icon (pixbuf, size);
}

/* FIXME: add support for theme changed. I think it should be as easy as
   call g_object_notify (tab, "name") when the icon theme changes */
GdkPixbuf *
_gedit_tab_get_icon (GeditTab *tab)
{
	GdkPixbuf *pixbuf;
	GtkIconTheme *theme;
	GdkScreen *screen;
	gint icon_size;

	g_return_val_if_fail (GEDIT_IS_TAB (tab), NULL);

	screen = gtk_widget_get_screen (GTK_WIDGET (tab));

	theme = gtk_icon_theme_get_for_screen (screen);
	g_return_val_if_fail (theme != NULL, NULL);

	gtk_icon_size_lookup_for_settings (gtk_widget_get_settings (GTK_WIDGET (tab)),
					   GTK_ICON_SIZE_MENU, 
					   NULL,
					   &icon_size);

	switch (tab->priv->state)
	{
		case GEDIT_TAB_STATE_LOADING:
			pixbuf = get_stock_icon (theme, 
						 GTK_STOCK_OPEN, 
						 icon_size);
			break;

		case GEDIT_TAB_STATE_REVERTING:
			pixbuf = get_stock_icon (theme, 
						 GTK_STOCK_REVERT_TO_SAVED, 
						 icon_size);						 
			break;

		case GEDIT_TAB_STATE_SAVING:
			pixbuf = get_stock_icon (theme, 
						 GTK_STOCK_SAVE, 
						 icon_size);
			break;

		case GEDIT_TAB_STATE_PRINTING:
			pixbuf = get_stock_icon (theme, 
						 GTK_STOCK_PRINT, 
						 icon_size);
			break;

		case GEDIT_TAB_STATE_PRINT_PREVIEWING:
		case GEDIT_TAB_STATE_SHOWING_PRINT_PREVIEW:		
			pixbuf = get_stock_icon (theme, 
						 GTK_STOCK_PRINT_PREVIEW, 
						 icon_size);
			break;

		case GEDIT_TAB_STATE_LOADING_ERROR:
		case GEDIT_TAB_STATE_REVERTING_ERROR:
		case GEDIT_TAB_STATE_SAVING_ERROR:
		case GEDIT_TAB_STATE_GENERIC_ERROR:
			pixbuf = get_stock_icon (theme, 
						 GTK_STOCK_DIALOG_ERROR, 
						 icon_size);
			break;

		case GEDIT_TAB_STATE_EXTERNALLY_MODIFIED_NOTIFICATION:
			pixbuf = get_stock_icon (theme,
						 GTK_STOCK_DIALOG_WARNING,
						 icon_size);
			break;

		default:
		{
			GFile *location;
			GeditDocument *doc;

			doc = gedit_tab_get_document (tab);

			location = gedit_document_get_location (doc);
			pixbuf = get_icon (theme, location, icon_size);

			if (location)
				g_object_unref (location);
		}
	}

	return pixbuf;
}

/**
 * gedit_tab_get_from_document:
 * @doc: a #GeditDocument
 *
 * Gets the #GeditTab associated with @doc.
 *
 * Returns: the #GeditTab associated with @doc
 */
GeditTab *
gedit_tab_get_from_document (GeditDocument *doc)
{
	gpointer res;
	
	g_return_val_if_fail (GEDIT_IS_DOCUMENT (doc), NULL);
	
	res = g_object_get_data (G_OBJECT (doc), GEDIT_TAB_KEY);
	
	return (res != NULL) ? GEDIT_TAB (res) : NULL;
}

void
_gedit_tab_load (GeditTab            *tab,
		 GFile               *location,
		 const GeditEncoding *encoding,
		 gint                 line_pos,
		 gint                 column_pos,
		 gboolean             create)
{
	GeditDocument *doc;

	g_return_if_fail (GEDIT_IS_TAB (tab));
	g_return_if_fail (G_IS_FILE (location));
	g_return_if_fail (tab->priv->state == GEDIT_TAB_STATE_NORMAL);

	doc = gedit_tab_get_document (tab);
	g_return_if_fail (GEDIT_IS_DOCUMENT (doc));

	gedit_tab_set_state (tab, GEDIT_TAB_STATE_LOADING);

	tab->priv->tmp_line_pos = line_pos;
	tab->priv->tmp_column_pos = column_pos;
	tab->priv->tmp_encoding = encoding;

	if (tab->priv->auto_save_timeout > 0)
		remove_auto_save_timeout (tab);

	gedit_document_load (doc,
			     location,
			     encoding,
			     line_pos,
			     column_pos,
			     create);
}

void
_gedit_tab_load_stream (GeditTab            *tab,
                        GInputStream        *stream,
                        const GeditEncoding *encoding,
                        gint                 line_pos,
                        gint                 column_pos)
{
	GeditDocument *doc;

	g_return_if_fail (GEDIT_IS_TAB (tab));
	g_return_if_fail (G_IS_INPUT_STREAM (stream));
	g_return_if_fail (tab->priv->state == GEDIT_TAB_STATE_NORMAL);

	doc = gedit_tab_get_document (tab);
	g_return_if_fail (GEDIT_IS_DOCUMENT (doc));

	gedit_tab_set_state (tab, GEDIT_TAB_STATE_LOADING);

	tab->priv->tmp_line_pos = line_pos;
	tab->priv->tmp_column_pos = column_pos;
	tab->priv->tmp_encoding = encoding;

	if (tab->priv->auto_save_timeout > 0)
	{
		remove_auto_save_timeout (tab);
	}

	gedit_document_load_stream (doc,
	                            stream,
	                            encoding,
	                            line_pos,
	                            column_pos);

}

void
_gedit_tab_revert (GeditTab *tab)
{
	GeditDocument *doc;
	GFile *location;

	g_return_if_fail (GEDIT_IS_TAB (tab));
	g_return_if_fail ((tab->priv->state == GEDIT_TAB_STATE_NORMAL) ||
			  (tab->priv->state == GEDIT_TAB_STATE_EXTERNALLY_MODIFIED_NOTIFICATION));

	if (tab->priv->state == GEDIT_TAB_STATE_EXTERNALLY_MODIFIED_NOTIFICATION)
	{
		set_info_bar (tab, NULL);
	}

	doc = gedit_tab_get_document (tab);
	g_return_if_fail (GEDIT_IS_DOCUMENT (doc));

	gedit_tab_set_state (tab, GEDIT_TAB_STATE_REVERTING);

	location = gedit_document_get_location (doc);
	g_return_if_fail (location != NULL);

	tab->priv->tmp_line_pos = 0;
	tab->priv->tmp_encoding = gedit_document_get_encoding (doc);

	if (tab->priv->auto_save_timeout > 0)
		remove_auto_save_timeout (tab);

	gedit_document_load (doc,
			     location,
			     tab->priv->tmp_encoding,
			     0,
			     0,
			     FALSE);

	g_object_unref (location);
}

void
_gedit_tab_save (GeditTab *tab)
{
	GeditDocument *doc;
	GeditDocumentSaveFlags save_flags;

	g_return_if_fail (GEDIT_IS_TAB (tab));
	g_return_if_fail ((tab->priv->state == GEDIT_TAB_STATE_NORMAL) ||
			  (tab->priv->state == GEDIT_TAB_STATE_EXTERNALLY_MODIFIED_NOTIFICATION) ||
			  (tab->priv->state == GEDIT_TAB_STATE_SHOWING_PRINT_PREVIEW));
	g_return_if_fail (tab->priv->tmp_save_location == NULL);
	g_return_if_fail (tab->priv->tmp_encoding == NULL);

	doc = gedit_tab_get_document (tab);
	g_return_if_fail (GEDIT_IS_DOCUMENT (doc));
	g_return_if_fail (!gedit_document_is_untitled (doc));

	if (tab->priv->state == GEDIT_TAB_STATE_EXTERNALLY_MODIFIED_NOTIFICATION)
	{
		/* We already told the user about the external
		 * modification: hide the message bar and set
		 * the save flag.
		 */

		set_info_bar (tab, NULL);
		save_flags = tab->priv->save_flags | GEDIT_DOCUMENT_SAVE_IGNORE_MTIME;
	}
	else
	{
		save_flags = tab->priv->save_flags;
	}

	gedit_tab_set_state (tab, GEDIT_TAB_STATE_SAVING);

	/* uri used in error messages, will be freed in document_saved */
	tab->priv->tmp_save_location = gedit_document_get_location (doc);
	tab->priv->tmp_encoding = gedit_document_get_encoding (doc); 

	if (tab->priv->auto_save_timeout > 0)
		remove_auto_save_timeout (tab);
		
	gedit_document_save (doc, save_flags);
}

static gboolean
gedit_tab_auto_save (GeditTab *tab)
{
	GeditDocument *doc;

	gedit_debug (DEBUG_TAB);
	
	g_return_val_if_fail (tab->priv->tmp_save_location == NULL, FALSE);
	g_return_val_if_fail (tab->priv->tmp_encoding == NULL, FALSE);
	
	doc = gedit_tab_get_document (tab);
	
	g_return_val_if_fail (!gedit_document_is_untitled (doc), FALSE);
	g_return_val_if_fail (!gedit_document_get_readonly (doc), FALSE);

	g_return_val_if_fail (tab->priv->auto_save_timeout > 0, FALSE);
	g_return_val_if_fail (tab->priv->auto_save, FALSE);
	g_return_val_if_fail (tab->priv->auto_save_interval > 0, FALSE);

	if (!gtk_text_buffer_get_modified (GTK_TEXT_BUFFER(doc)))
	{
		gedit_debug_message (DEBUG_TAB, "Document not modified");

		return TRUE;
	}
			
	if ((tab->priv->state != GEDIT_TAB_STATE_NORMAL) &&
	    (tab->priv->state != GEDIT_TAB_STATE_SHOWING_PRINT_PREVIEW))
	{
		/* Retry after 30 seconds */
		guint timeout;

		gedit_debug_message (DEBUG_TAB, "Retry after 30 seconds");

		/* Add a new timeout */
		timeout = g_timeout_add_seconds (30,
						 (GSourceFunc) gedit_tab_auto_save,
						 tab);

		tab->priv->auto_save_timeout = timeout;

	    	/* Returns FALSE so the old timeout is "destroyed" */
		return FALSE;
	}
	
	gedit_tab_set_state (tab, GEDIT_TAB_STATE_SAVING);

	/* uri used in error messages, will be freed in document_saved */
	tab->priv->tmp_save_location = gedit_document_get_location (doc);
	tab->priv->tmp_encoding = gedit_document_get_encoding (doc); 

	/* Set auto_save_timeout to 0 since the timeout is going to be destroyed */
	tab->priv->auto_save_timeout = 0;

	/* Since we are autosaving, we need to preserve the backup that was produced
	   the last time the user "manually" saved the file. In the case a recoverable
	   error happens while saving, the last backup is not preserved since the user
	   expressed his willing of saving the file */
	gedit_document_save (doc, tab->priv->save_flags | GEDIT_DOCUMENT_SAVE_PRESERVE_BACKUP);
	
	gedit_debug_message (DEBUG_TAB, "Done");
	
	/* Returns FALSE so the old timeout is "destroyed" */
	return FALSE;
}

void
_gedit_tab_save_as (GeditTab                     *tab,
                    GFile                        *location,
                    const GeditEncoding          *encoding,
                    GeditDocumentNewlineType      newline_type,
                    GeditDocumentCompressionType  compression_type)
{
	GeditDocument *doc;
	GeditDocumentSaveFlags save_flags;

	g_return_if_fail (GEDIT_IS_TAB (tab));
	g_return_if_fail ((tab->priv->state == GEDIT_TAB_STATE_NORMAL) ||
			  (tab->priv->state == GEDIT_TAB_STATE_EXTERNALLY_MODIFIED_NOTIFICATION) ||
			  (tab->priv->state == GEDIT_TAB_STATE_SHOWING_PRINT_PREVIEW));
	g_return_if_fail (G_IS_FILE (location));
	g_return_if_fail (encoding != NULL);

	g_return_if_fail (tab->priv->tmp_save_location == NULL);
	g_return_if_fail (tab->priv->tmp_encoding == NULL);

	doc = gedit_tab_get_document (tab);
	g_return_if_fail (GEDIT_IS_DOCUMENT (doc));

	/* reset the save flags, when saving as */
	tab->priv->save_flags = 0;

	if (tab->priv->state == GEDIT_TAB_STATE_EXTERNALLY_MODIFIED_NOTIFICATION)
	{
		/* We already told the user about the external
		 * modification: hide the message bar and set
		 * the save flag.
		 */

		set_info_bar (tab, NULL);
		save_flags = tab->priv->save_flags | GEDIT_DOCUMENT_SAVE_IGNORE_MTIME;
	}
	else
	{
		save_flags = tab->priv->save_flags;
	}

	gedit_tab_set_state (tab, GEDIT_TAB_STATE_SAVING);

	/* uri used in error messages... strdup because errors are async
	 * and the string can go away, will be freed in document_saved */
	tab->priv->tmp_save_location = g_file_dup (location);
	tab->priv->tmp_encoding = encoding;

	if (tab->priv->auto_save_timeout > 0)
		remove_auto_save_timeout (tab);

	gedit_document_save_as (doc,
	                        location,
	                        encoding,
	                        newline_type,
	                        compression_type,
	                        tab->priv->save_flags);
}

#define GEDIT_PAGE_SETUP_KEY "gedit-page-setup-key"
#define GEDIT_PRINT_SETTINGS_KEY "gedit-print-settings-key"

static GtkPageSetup *
get_page_setup (GeditTab *tab)
{
	gpointer data;
	GeditDocument *doc;

	doc = gedit_tab_get_document (tab);

	data = g_object_get_data (G_OBJECT (doc),
				  GEDIT_PAGE_SETUP_KEY);

	if (data == NULL)
	{
		return _gedit_app_get_default_page_setup (gedit_app_get_default());
	}
	else
	{
		return gtk_page_setup_copy (GTK_PAGE_SETUP (data));
	}
}

static GtkPrintSettings *
get_print_settings (GeditTab *tab)
{
	gpointer data;
	GeditDocument *doc;

	doc = gedit_tab_get_document (tab);

	data = g_object_get_data (G_OBJECT (doc),
				  GEDIT_PRINT_SETTINGS_KEY);

	if (data == NULL)
	{
		return _gedit_app_get_default_print_settings (gedit_app_get_default());
	}
	else
	{
		return gtk_print_settings_copy (GTK_PRINT_SETTINGS (data));
	}
}

/* FIXME: show the info bar only if the operation will be "long" */
static void
printing_cb (GeditPrintJob       *job,
	     GeditPrintJobStatus  status,
	     GeditTab            *tab)
{
	g_return_if_fail (GEDIT_IS_PROGRESS_INFO_BAR (tab->priv->info_bar));	

	gtk_widget_show (tab->priv->info_bar);

	gedit_progress_info_bar_set_text (GEDIT_PROGRESS_INFO_BAR (tab->priv->info_bar),
					      gedit_print_job_get_status_string (job));

	gedit_progress_info_bar_set_fraction (GEDIT_PROGRESS_INFO_BAR (tab->priv->info_bar),
						  gedit_print_job_get_progress (job));
}

static void
store_print_settings (GeditTab      *tab,
		      GeditPrintJob *job)
{
	GeditDocument *doc;
	GtkPrintSettings *settings;
	GtkPageSetup *page_setup;

	doc = gedit_tab_get_document (tab);

	settings = gedit_print_job_get_print_settings (job);

	/* clear n-copies settings since we do not want to
	 * persist that one */
	gtk_print_settings_unset (settings,
				  GTK_PRINT_SETTINGS_N_COPIES);

	/* remember settings for this document */
	g_object_set_data_full (G_OBJECT (doc),
				GEDIT_PRINT_SETTINGS_KEY,
				g_object_ref (settings),
				(GDestroyNotify)g_object_unref);

	/* make them the default */
	_gedit_app_set_default_print_settings (gedit_app_get_default (),
					       settings);

	page_setup = gedit_print_job_get_page_setup (job);

	/* remember page setup for this document */
	g_object_set_data_full (G_OBJECT (doc),
				GEDIT_PAGE_SETUP_KEY,
				g_object_ref (page_setup),
				(GDestroyNotify)g_object_unref);

	/* make it the default */
	_gedit_app_set_default_page_setup (gedit_app_get_default (),
					   page_setup);
}

static void
done_printing_cb (GeditPrintJob       *job,
		  GeditPrintJobResult  result,
		  const GError        *error,
		  GeditTab            *tab)
{
	GeditView *view;

	g_return_if_fail (tab->priv->state == GEDIT_TAB_STATE_PRINT_PREVIEWING ||
			  tab->priv->state == GEDIT_TAB_STATE_SHOWING_PRINT_PREVIEW ||
			  tab->priv->state == GEDIT_TAB_STATE_PRINTING);

	if (tab->priv->state == GEDIT_TAB_STATE_SHOWING_PRINT_PREVIEW)
	{
		/* print preview has been destroyed... */
		tab->priv->print_preview = NULL;
	}
	else
	{
		g_return_if_fail (GEDIT_IS_PROGRESS_INFO_BAR (tab->priv->info_bar));

		set_info_bar (tab, NULL); /* destroy the info bar */
	}

	/* TODO: check status and error */

	if (result ==  GEDIT_PRINT_JOB_RESULT_OK)
	{
		store_print_settings (tab, job);
	}

#if 0
	if (tab->priv->print_preview != NULL)
	{
		/* If we were printing while showing the print preview,
		   see bug #352658 */
		gtk_widget_destroy (tab->priv->print_preview);
		g_return_if_fail (tab->priv->state == GEDIT_TAB_STATE_PRINTING);
	}
#endif

	gedit_tab_set_state (tab, GEDIT_TAB_STATE_NORMAL);

	view = gedit_tab_get_view (tab);
	gtk_widget_grab_focus (GTK_WIDGET (view));

 	g_object_unref (tab->priv->print_job);
	tab->priv->print_job = NULL;
}

#if 0
static void
print_preview_destroyed (GtkWidget *preview,
			 GeditTab  *tab)
{
	tab->priv->print_preview = NULL;

	if (tab->priv->state == GEDIT_TAB_STATE_SHOWING_PRINT_PREVIEW)
	{
		GeditView *view;

		gedit_tab_set_state (tab, GEDIT_TAB_STATE_NORMAL);

		view = gedit_tab_get_view (tab);
		gtk_widget_grab_focus (GTK_WIDGET (view));
	}
	else
	{
		/* This should happen only when printing while showing the print
		 * preview. In this case let us continue whithout changing
		 * the state and show the document. See bug #352658 */
		gtk_widget_show (tab->priv->view_scrolled_window);
		
		g_return_if_fail (tab->priv->state == GEDIT_TAB_STATE_PRINTING);
	}	
}
#endif

static void
show_preview_cb (GeditPrintJob       *job,
		 GeditPrintPreview   *preview,
		 GeditTab            *tab)
{
	/* g_return_if_fail (tab->priv->state == GEDIT_TAB_STATE_PRINT_PREVIEWING); */
	g_return_if_fail (tab->priv->print_preview == NULL);

	set_info_bar (tab, NULL); /* destroy the info bar */

	tab->priv->print_preview = GTK_WIDGET (preview);
	gtk_box_pack_end (GTK_BOX (tab),
			  tab->priv->print_preview,
			  TRUE,
			  TRUE,
			  0);
	gtk_widget_show (tab->priv->print_preview);
	gtk_widget_grab_focus (tab->priv->print_preview);

/* when the preview gets destroyed we get "done" signal
	g_signal_connect (tab->priv->print_preview,
			  "destroy",
			  G_CALLBACK (print_preview_destroyed),
			  tab);	
*/
	gedit_tab_set_state (tab, GEDIT_TAB_STATE_SHOWING_PRINT_PREVIEW);
}

#if 0

static void
set_print_preview (GeditTab  *tab,
		   GtkWidget *print_preview)
{
	if (tab->priv->print_preview == print_preview)
		return;
		
	if (tab->priv->print_preview != NULL)
		gtk_widget_destroy (tab->priv->print_preview);

	tab->priv->print_preview = print_preview;

	gtk_box_pack_end (GTK_BOX (tab),
			  tab->priv->print_preview,
			  TRUE,
			  TRUE,
			  0);		

	gtk_widget_grab_focus (tab->priv->print_preview);

	g_signal_connect (tab->priv->print_preview,
			  "destroy",
			  G_CALLBACK (print_preview_destroyed),
			  tab);
}

static void
preview_finished_cb (GtkSourcePrintJob *pjob, GeditTab *tab)
{
	GnomePrintJob *gjob;
	GtkWidget *preview = NULL;

	g_return_if_fail (GEDIT_IS_PROGRESS_INFO_BAR (tab->priv->info_bar));
	set_info_bar (tab, NULL); /* destroy the info bar */
	
	gjob = gtk_source_print_job_get_print_job (pjob);

	preview = gedit_print_job_preview_new (gjob);	
 	g_object_unref (gjob);
	
	set_print_preview (tab, preview);
	
	gtk_widget_show (preview);
	g_object_unref (pjob);
	
	gedit_tab_set_state (tab, GEDIT_TAB_STATE_SHOWING_PRINT_PREVIEW);
}


#endif

static void
print_cancelled (GtkWidget        *bar,
                 gint              response_id,
                 GeditTab         *tab)
{
	g_return_if_fail (GEDIT_IS_PROGRESS_INFO_BAR (tab->priv->info_bar));

	gedit_print_job_cancel (tab->priv->print_job);

	g_debug ("print_cancelled");
}

static void
show_printing_info_bar (GeditTab *tab, gboolean preview)
{
	GtkWidget *bar;

	if (preview)
	{
		bar = gedit_progress_info_bar_new (GTK_STOCK_PRINT_PREVIEW,
							"",
							TRUE);
	}
	else
	{
		bar = gedit_progress_info_bar_new (GTK_STOCK_PRINT,
							"",
							TRUE);
	}

	g_signal_connect (bar,
			  "response",
			  G_CALLBACK (print_cancelled),
			  tab);
	  
	set_info_bar (tab, bar);
}

static void
gedit_tab_print_or_print_preview (GeditTab                *tab,
				  GtkPrintOperationAction  print_action)
{
	GeditView *view;
	gboolean is_preview;
	GtkPageSetup *setup;
	GtkPrintSettings *settings;
	GtkPrintOperationResult res;
	GError *error = NULL;

	g_return_if_fail (tab->priv->print_job == NULL);
	g_return_if_fail (tab->priv->state == GEDIT_TAB_STATE_NORMAL);

	view = gedit_tab_get_view (tab);

	is_preview = (print_action == GTK_PRINT_OPERATION_ACTION_PREVIEW);

	tab->priv->print_job = gedit_print_job_new (view);
	g_object_add_weak_pointer (G_OBJECT (tab->priv->print_job), 
				   (gpointer *) &tab->priv->print_job);

	show_printing_info_bar (tab, is_preview);

	g_signal_connect (tab->priv->print_job,
			  "printing",
			  G_CALLBACK (printing_cb),
			  tab);
	g_signal_connect (tab->priv->print_job,
			  "show-preview",
			  G_CALLBACK (show_preview_cb),
			  tab);
	g_signal_connect (tab->priv->print_job,
			  "done",
			  G_CALLBACK (done_printing_cb),
			  tab);

	if (is_preview)
		gedit_tab_set_state (tab, GEDIT_TAB_STATE_PRINT_PREVIEWING);
	else
		gedit_tab_set_state (tab, GEDIT_TAB_STATE_PRINTING);

	setup = get_page_setup (tab);
	settings = get_print_settings (tab);

	res = gedit_print_job_print (tab->priv->print_job,
				     print_action,
				     setup,
				     settings,
				     GTK_WINDOW (gtk_widget_get_toplevel (GTK_WIDGET (tab))),
				     &error);

	/* TODO: manage res in the correct way */
	if (res == GTK_PRINT_OPERATION_RESULT_ERROR)
	{
		/* FIXME: go in error state */
		gedit_tab_set_state (tab, GEDIT_TAB_STATE_NORMAL);
		g_warning ("Async print preview failed (%s)", error->message);
		g_object_unref (tab->priv->print_job);
		g_error_free (error);
	}
}

void 
_gedit_tab_print (GeditTab *tab)
{
	g_return_if_fail (GEDIT_IS_TAB (tab));

	/* FIXME: currently we can have just one printoperation going on
	 * at a given time, so before starting the print we close the preview.
	 * Would be nice to handle it properly though */
	if (tab->priv->state == GEDIT_TAB_STATE_SHOWING_PRINT_PREVIEW)
	{
		gtk_widget_destroy (tab->priv->print_preview);
	}

	gedit_tab_print_or_print_preview (tab,
					  GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG);
}

void
_gedit_tab_print_preview (GeditTab *tab)
{
	g_return_if_fail (GEDIT_IS_TAB (tab));

	gedit_tab_print_or_print_preview (tab,
					  GTK_PRINT_OPERATION_ACTION_PREVIEW);
}

void 
_gedit_tab_mark_for_closing (GeditTab *tab)
{
	g_return_if_fail (GEDIT_IS_TAB (tab));
	g_return_if_fail (tab->priv->state == GEDIT_TAB_STATE_NORMAL);
	
	gedit_tab_set_state (tab, GEDIT_TAB_STATE_CLOSING);
}

gboolean
_gedit_tab_can_close (GeditTab *tab)
{
	GeditDocument *doc;
	GeditTabState  ts;

	g_return_val_if_fail (GEDIT_IS_TAB (tab), FALSE);

	ts = gedit_tab_get_state (tab);

	/* if we are loading or reverting, the tab can be closed */
	if ((ts == GEDIT_TAB_STATE_LOADING)       ||
	    (ts == GEDIT_TAB_STATE_LOADING_ERROR) ||
	    (ts == GEDIT_TAB_STATE_REVERTING)     ||
	    (ts == GEDIT_TAB_STATE_REVERTING_ERROR)) /* CHECK: I'm not sure this is the right behavior for REVERTING ERROR */
	{
		return TRUE;
	}

	/* Do not close tab with saving errors */
	if (ts == GEDIT_TAB_STATE_SAVING_ERROR)
		return FALSE;
		
	doc = gedit_tab_get_document (tab);

	/* TODO: we need to save the file also if it has been externally
	   modified - Paolo (Oct 10, 2005) */

	return (!gtk_text_buffer_get_modified (GTK_TEXT_BUFFER (doc)) &&
		!gedit_document_get_deleted (doc));
}

/**
 * gedit_tab_get_auto_save_enabled:
 * @tab: a #GeditTab
 * 
 * Gets the current state for the autosave feature
 * 
 * Return value: %TRUE if the autosave is enabled, else %FALSE
 **/
gboolean
gedit_tab_get_auto_save_enabled	(GeditTab *tab)
{
	gedit_debug (DEBUG_TAB);

	g_return_val_if_fail (GEDIT_IS_TAB (tab), FALSE);

	return tab->priv->auto_save;
}

/**
 * gedit_tab_set_auto_save_enabled:
 * @tab: a #GeditTab
 * @enable: enable (%TRUE) or disable (%FALSE) auto save
 * 
 * Enables or disables the autosave feature. It does not install an
 * autosave timeout if the document is new or is read-only
 **/
void
gedit_tab_set_auto_save_enabled	(GeditTab *tab, 
				 gboolean  enable)
{
	GeditDocument *doc = NULL;
	GeditLockdownMask lockdown;
	
	gedit_debug (DEBUG_TAB);

	g_return_if_fail (GEDIT_IS_TAB (tab));

	/* Force disabling when lockdown is active */
	lockdown = gedit_app_get_lockdown (gedit_app_get_default());
	if (lockdown & GEDIT_LOCKDOWN_SAVE_TO_DISK)
		enable = FALSE;
	
	doc = gedit_tab_get_document (tab);

	if (tab->priv->auto_save == enable)
		return;

	tab->priv->auto_save = enable;

 	if (enable && 
 	    (tab->priv->auto_save_timeout <=0) &&
 	    !gedit_document_is_untitled (doc) &&
 	    !gedit_document_get_readonly (doc))
 	{
 		if ((tab->priv->state != GEDIT_TAB_STATE_LOADING) &&
		    (tab->priv->state != GEDIT_TAB_STATE_SAVING) &&
		    (tab->priv->state != GEDIT_TAB_STATE_REVERTING) &&
		    (tab->priv->state != GEDIT_TAB_STATE_LOADING_ERROR) &&
		    (tab->priv->state != GEDIT_TAB_STATE_SAVING_ERROR) &&
		    (tab->priv->state != GEDIT_TAB_STATE_REVERTING_ERROR))
		{
			install_auto_save_timeout (tab);
		}
		/* else: the timeout will be installed when loading/saving/reverting
		         will terminate */
		
		return;
	}
 		
 	if (!enable && (tab->priv->auto_save_timeout > 0))
 	{
		remove_auto_save_timeout (tab);
		
 		return; 
 	} 

 	g_return_if_fail ((!enable && (tab->priv->auto_save_timeout <= 0)) ||  
 			  gedit_document_is_untitled (doc) || gedit_document_get_readonly (doc)); 
}

/**
 * gedit_tab_get_auto_save_interval:
 * @tab: a #GeditTab
 * 
 * Gets the current interval for the autosaves
 * 
 * Return value: the value of the autosave
 **/
gint 
gedit_tab_get_auto_save_interval (GeditTab *tab)
{
	gedit_debug (DEBUG_TAB);

	g_return_val_if_fail (GEDIT_IS_TAB (tab), 0);

	return tab->priv->auto_save_interval;
}

/**
 * gedit_tab_set_auto_save_interval:
 * @tab: a #GeditTab
 * @interval: the new interval
 * 
 * Sets the interval for the autosave feature. It does nothing if the
 * interval is the same as the one already present. It removes the old
 * interval timeout and adds a new one with the autosave passed as
 * argument.
 **/
void 
gedit_tab_set_auto_save_interval (GeditTab *tab, 
				  gint      interval)
{
	GeditDocument *doc = NULL;
	
	gedit_debug (DEBUG_TAB);
	
	g_return_if_fail (GEDIT_IS_TAB (tab));

	doc = gedit_tab_get_document(tab);

	g_return_if_fail (GEDIT_IS_DOCUMENT (doc));
	g_return_if_fail (interval > 0);

	if (tab->priv->auto_save_interval == interval)
		return;

	tab->priv->auto_save_interval = interval;
		
	if (!tab->priv->auto_save)
		return;

	if (tab->priv->auto_save_timeout > 0)
	{
		g_return_if_fail (!gedit_document_is_untitled (doc));
		g_return_if_fail (!gedit_document_get_readonly (doc));

		remove_auto_save_timeout (tab);

		install_auto_save_timeout (tab);
	}
}

void
gedit_tab_set_info_bar (GeditTab  *tab,
                        GtkWidget *info_bar)
{
	g_return_if_fail (GEDIT_IS_TAB (tab));
	g_return_if_fail (info_bar == NULL || GTK_IS_WIDGET (info_bar));

	/* FIXME: this can cause problems with the tab state machine */
	set_info_bar (tab, info_bar);
}

GtkWidget *
_gedit_tab_get_view_frame (GeditTab *tab)
{
	return GTK_WIDGET (tab->priv->frame);
}

/* ex:ts=8:noet: */
