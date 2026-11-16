/*
 * Remmina - The GTK+ Remote Desktop Client
 * Copyright (C) 2025 lRemote contributors
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 * You must obey the GNU General Public License in all respects
 * for all of the code used other than OpenSSL. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you
 * do not wish to do so, delete this exception statement from your
 * version. If you delete this exception statement from all source
 * files in the program, then also delete it here.
 *
 */

/*
 * lRemote main window, laid out like mRemoteNG:
 *
 *   +-------------------------------------------------------------+
 *   | menu bar                                                    |
 *   | quick connect toolbar                                       |
 *   +---------------------+---------------------------------------+
 *   | search              |                                       |
 *   | connection tree     |  start page / selected connection     |
 *   | (folders + profiles)|                                       |
 *   +---------------------+                                       |
 *   | Config (properties) |                                       |
 *   +---------------------+---------------------------------------+
 *   | status bar                                                  |
 *   +-------------------------------------------------------------+
 *
 * Folders are the "group" setting of each profile ("A/B/C"). Empty
 * folders are remembered in the lremote_folders preference. Sessions
 * open as tabs of a single connection window.
 */

#include "config.h"
#include <string.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include "lremote_window.h"
#include "lremote_mremoteng.h"
#include "lremote_update.h"
#include "remmina_icon.h"
#include "rcw.h"
#include "remmina_about.h"
#include "remmina_backup.h"
#include "remmina_exec.h"
#include "remmina_file.h"
#include "remmina_file_editor.h"
#include "remmina_file_manager.h"
#include "remmina_plugin_manager.h"
#include "remmina_pref.h"
#include "remmina_pref_dialog.h"
#include "remmina_public.h"
#include "remmina_theme.h"
#include "remmina_widget_pool.h"
#include "remmina/remmina_trace_calls.h"

enum {
	COL_ICON,	/* icon name */
	COL_NAME,
	COL_FILE,	/* profile path, NULL for folders */
	COL_PATH,	/* folder path, or the profile's group */
	COL_IS_FOLDER,
	COL_SERVER,
	COL_PROTOCOL,
	COL_WEIGHT,
	N_COLS
};

enum {
	RES_ROW_WINDOW = 0,	/* initial window size, dynamic */
	RES_ROW_CLIENT = 1,	/* client resolution */
	RES_ROW_CUSTOM = 2,
};

typedef struct {
	GtkWindow *	window;
	GtkTreeStore *	store;
	GtkTreeModel *	filter;
	GtkTreeView *	tree;
	GtkSearchEntry *search;
	GtkPaned *	hpaned;
	GtkPaned *	vpaned;
	GtkLabel *	status;

	/* quick connect */
	GtkComboBoxText *quick_proto;
	GtkEntry *	quick_server;

	/* Config panel */
	GtkWidget *	props_grid;
	GtkLabel *	props_title;
	GtkEntry *	p_name;
	GtkComboBoxText *p_folder;
	GtkComboBoxText *p_protocol;
	GtkEntry *	p_server;
	GtkEntry *	p_username;
	GtkEntry *	p_password;
	GtkEntry *	p_domain;
	GtkComboBoxText *p_resolution;
	GtkEntry *	p_res_custom;
	GtkComboBoxText *p_colordepth;
	GtkTextView *	p_notes;
	GtkWidget *	p_buttons;

	/* start page */
	GtkLabel *	start_title;
	GtkLabel *	start_detail;
	GtkWidget *	start_connect;

	/* right side: start page or the embedded session tabs */
	GtkStack *	right;
	GtkWidget *	sessions;

	gchar *		selected_file;
	gchar *		selected_folder;
	gint		n_connections;
	gint		n_folders;
	GHashTable *	folder_iters;	/* path -> GtkTreeIter* in store */
	gboolean	loading;
} LremoteWindow;

static LremoteWindow *lw = NULL;

static void lw_update_start_page(RemminaFile *f);
static void lw_connect_selected_window(GtkWidget *w, gpointer data);

static const gint colordepths[] = { 0, 8, 15, 16, 24, 32 };

/* ------------------------------------------------------------------ */
/* helpers                                                             */

static void lw_message(GtkMessageType type, const gchar *primary, const gchar *secondary)
{
	GtkWidget *d = gtk_message_dialog_new(lw ? lw->window : NULL, GTK_DIALOG_MODAL, type, GTK_BUTTONS_OK, "%s", primary);
	if (secondary)
		gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", secondary);
	gtk_dialog_run(GTK_DIALOG(d));
	gtk_widget_destroy(d);
}

static gboolean lw_confirm(const gchar *primary, const gchar *secondary, const gchar *ok_label)
{
	GtkWidget *d = gtk_message_dialog_new(lw->window, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "%s", primary);
	gint r;
	if (secondary)
		gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", secondary);
	gtk_dialog_add_buttons(GTK_DIALOG(d), _("_Cancel"), GTK_RESPONSE_CANCEL, ok_label, GTK_RESPONSE_OK, NULL);
	gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL);
	r = gtk_dialog_run(GTK_DIALOG(d));
	gtk_widget_destroy(d);
	return r == GTK_RESPONSE_OK;
}

/* Asks for one line of text; returns NULL on cancel */
static gchar *lw_ask_text(const gchar *title, const gchar *label, const gchar *initial, gboolean secret)
{
	GtkWidget *d, *box, *entry, *lbl;
	gchar *res = NULL;

	d = gtk_dialog_new_with_buttons(title, lw->window, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
					_("_Cancel"), GTK_RESPONSE_CANCEL, _("_OK"), GTK_RESPONSE_OK, NULL);
	gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
	box = gtk_dialog_get_content_area(GTK_DIALOG(d));
	gtk_container_set_border_width(GTK_CONTAINER(box), 12);
	gtk_box_set_spacing(GTK_BOX(box), 8);
	lbl = gtk_label_new(label);
	gtk_label_set_xalign(GTK_LABEL(lbl), 0);
	gtk_label_set_line_wrap(GTK_LABEL(lbl), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(lbl), 50);
	entry = gtk_entry_new();
	gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
	gtk_entry_set_visibility(GTK_ENTRY(entry), !secret);
	if (initial)
		gtk_entry_set_text(GTK_ENTRY(entry), initial);
	gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);
	gtk_widget_show_all(d);
	if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK)
		res = g_strdup(gtk_entry_get_text(GTK_ENTRY(entry)));
	gtk_widget_destroy(d);
	return res;
}

static gchar *lw_clean_folder(const gchar *path)
{
	gchar **parts, *res;
	GPtrArray *keep = g_ptr_array_new();
	gint i;

	if (!path)
		return g_strdup("");
	parts = g_strsplit(path, "/", -1);
	for (i = 0; parts[i]; i++) {
		g_strstrip(parts[i]);
		if (*parts[i])
			g_ptr_array_add(keep, parts[i]);
	}
	g_ptr_array_add(keep, NULL);
	res = g_strjoinv("/", (gchar **)keep->pdata);
	g_ptr_array_free(keep, TRUE);
	g_strfreev(parts);
	return res;
}

/* Extra (possibly empty) folders kept in the preferences */
static GPtrArray *lw_saved_folders(void)
{
	GPtrArray *a = g_ptr_array_new_with_free_func(g_free);
	gchar *v = remmina_pref_get_value("lremote_folders");
	gchar **parts;
	gint i;

	if (v && *v) {
		parts = g_strsplit(v, ";", -1);
		for (i = 0; parts[i]; i++)
			if (*parts[i])
				g_ptr_array_add(a, g_strdup(parts[i]));
		g_strfreev(parts);
	}
	g_free(v);
	return a;
}

static void lw_store_folders(GPtrArray *a)
{
	gchar *v;
	g_ptr_array_add(a, NULL);
	v = g_strjoinv(";", (gchar **)a->pdata);
	g_ptr_array_remove_index(a, a->len - 1);
	remmina_pref_set_value("lremote_folders", v);
	g_free(v);
}

static void lw_folder_remember(const gchar *path)
{
	GPtrArray *a = lw_saved_folders();
	guint i;
	for (i = 0; i < a->len; i++)
		if (g_strcmp0(g_ptr_array_index(a, i), path) == 0) {
			g_ptr_array_free(a, TRUE);
			return;
		}
	g_ptr_array_add(a, g_strdup(path));
	lw_store_folders(a);
	g_ptr_array_free(a, TRUE);
}

/* Replaces the prefix old_path by new_path (NULL drops the folder and below) */
static void lw_folder_rewrite_saved(const gchar *old_path, const gchar *new_path)
{
	GPtrArray *a = lw_saved_folders(), *b = g_ptr_array_new_with_free_func(g_free);
	gsize ol = strlen(old_path);
	guint i;

	for (i = 0; i < a->len; i++) {
		const gchar *f = g_ptr_array_index(a, i);
		if (g_strcmp0(f, old_path) == 0 || (g_str_has_prefix(f, old_path) && f[ol] == '/')) {
			if (new_path)
				g_ptr_array_add(b, g_strconcat(new_path, f + ol, NULL));
		} else {
			g_ptr_array_add(b, g_strdup(f));
		}
	}
	lw_store_folders(b);
	g_ptr_array_free(a, TRUE);
	g_ptr_array_free(b, TRUE);
}

static const gchar *lw_protocol_icon(const gchar *protocol)
{
	RemminaProtocolPlugin *p;
	if (!protocol)
		return "network-server-symbolic";
	p = (RemminaProtocolPlugin *)remmina_plugin_manager_get_plugin(REMMINA_PLUGIN_TYPE_PROTOCOL, protocol);
	return (p && p->icon_name) ? p->icon_name : "network-server-symbolic";
}

/* ------------------------------------------------------------------ */
/* tree                                                                */

static GtkTreeIter *lw_ensure_folder(const gchar *path)
{
	GtkTreeIter *it, *parent = NULL;
	gchar **parts;
	GString *cur;
	gint i;

	if (!path || !*path)
		return NULL;
	it = g_hash_table_lookup(lw->folder_iters, path);
	if (it)
		return it;

	parts = g_strsplit(path, "/", -1);
	cur = g_string_new("");
	for (i = 0; parts[i]; i++) {
		if (cur->len)
			g_string_append_c(cur, '/');
		g_string_append(cur, parts[i]);
		it = g_hash_table_lookup(lw->folder_iters, cur->str);
		if (!it) {
			it = g_new0(GtkTreeIter, 1);
			gtk_tree_store_append(lw->store, it, parent);
			gtk_tree_store_set(lw->store, it,
					   COL_ICON, "folder-symbolic",
					   COL_NAME, parts[i],
					   COL_FILE, NULL,
					   COL_PATH, cur->str,
					   COL_IS_FOLDER, TRUE,
					   COL_WEIGHT, PANGO_WEIGHT_BOLD,
					   -1);
			g_hash_table_insert(lw->folder_iters, g_strdup(cur->str), it);
			lw->n_folders++;
		}
		parent = it;
	}
	g_string_free(cur, TRUE);
	g_strfreev(parts);
	return parent;
}

static void lw_add_profile(gpointer data, gpointer user_data)
{
	RemminaFile *f = data;
	GtkTreeIter it, *parent;
	gchar *group = lw_clean_folder(remmina_file_get_string(f, "group"));
	const gchar *proto = remmina_file_get_string(f, "protocol");

	parent = lw_ensure_folder(group);
	gtk_tree_store_append(lw->store, &it, parent);
	gtk_tree_store_set(lw->store, &it,
			   COL_ICON, lw_protocol_icon(proto),
			   COL_NAME, remmina_file_get_string(f, "name"),
			   COL_FILE, remmina_file_get_filename(f),
			   COL_PATH, group,
			   COL_IS_FOLDER, FALSE,
			   COL_SERVER, remmina_file_get_string(f, "server"),
			   COL_PROTOCOL, proto,
			   COL_WEIGHT, PANGO_WEIGHT_NORMAL,
			   -1);
	lw->n_connections++;
	g_free(group);
}

static gint lw_sort(GtkTreeModel *m, GtkTreeIter *a, GtkTreeIter *b, gpointer data)
{
	gboolean fa, fb;
	gchar *na, *nb;
	gint r;

	gtk_tree_model_get(m, a, COL_IS_FOLDER, &fa, COL_NAME, &na, -1);
	gtk_tree_model_get(m, b, COL_IS_FOLDER, &fb, COL_NAME, &nb, -1);
	if (fa != fb)
		r = fa ? -1 : 1;
	else
		r = g_utf8_collate(na ? na : "", nb ? nb : "");
	g_free(na);
	g_free(nb);
	return r;
}

static gboolean lw_text_matches(const gchar *hay, const gchar *needle)
{
	gchar *h;
	gboolean r;
	if (!hay)
		return FALSE;
	h = g_utf8_casefold(hay, -1);
	r = strstr(h, needle) != NULL;
	g_free(h);
	return r;
}

static gboolean lw_row_matches(GtkTreeModel *m, GtkTreeIter *it, const gchar *needle)
{
	gchar *name, *server;
	gboolean r;
	GtkTreeIter child;

	gtk_tree_model_get(m, it, COL_NAME, &name, COL_SERVER, &server, -1);
	r = lw_text_matches(name, needle) || lw_text_matches(server, needle);
	g_free(name);
	g_free(server);
	if (r)
		return TRUE;
	if (gtk_tree_model_iter_children(m, &child, it)) {
		do {
			if (lw_row_matches(m, &child, needle))
				return TRUE;
		} while (gtk_tree_model_iter_next(m, &child));
	}
	return FALSE;
}

static gboolean lw_visible(GtkTreeModel *m, GtkTreeIter *it, gpointer data)
{
	const gchar *text = lw ? gtk_entry_get_text(GTK_ENTRY(lw->search)) : NULL;
	gchar *needle;
	gboolean r;

	if (!text || !*text)
		return TRUE;
	needle = g_utf8_casefold(text, -1);
	r = lw_row_matches(m, it, needle);
	g_free(needle);
	return r;
}

static void lw_collect_expanded(GtkTreeView *tv, GtkTreePath *path, gpointer data)
{
	GPtrArray *a = data;
	GtkTreeIter it;
	gchar *p = NULL;
	if (gtk_tree_model_get_iter(gtk_tree_view_get_model(tv), &it, path)) {
		gtk_tree_model_get(gtk_tree_view_get_model(tv), &it, COL_PATH, &p, -1);
		if (p)
			g_ptr_array_add(a, p);
	}
}

static gboolean lw_filter_iter_for(const gchar *file, const gchar *folder, GtkTreeIter *out)
{
	GtkTreeIter it;
	GtkTreeModel *m = GTK_TREE_MODEL(lw->store);
	GtkTreeIter fit;

	/* depth-first walk over the store */
	GQueue q = G_QUEUE_INIT;
	GtkTreeIter *start;

	if (!gtk_tree_model_get_iter_first(m, &it))
		return FALSE;
	start = g_new(GtkTreeIter, 1);
	*start = it;
	g_queue_push_tail(&q, start);
	while (!g_queue_is_empty(&q)) {
		GtkTreeIter *cur = g_queue_pop_head(&q);
		do {
			gchar *f = NULL, *p = NULL;
			gboolean is_folder;
			GtkTreeIter child;
			gtk_tree_model_get(m, cur, COL_FILE, &f, COL_PATH, &p, COL_IS_FOLDER, &is_folder, -1);
			if ((file && g_strcmp0(f, file) == 0) || (!file && folder && is_folder && g_strcmp0(p, folder) == 0)) {
				g_free(f);
				g_free(p);
				if (gtk_tree_model_filter_convert_child_iter_to_iter(GTK_TREE_MODEL_FILTER(lw->filter), &fit, cur)) {
					*out = fit;
					g_free(cur);
					while (!g_queue_is_empty(&q))
						g_free(g_queue_pop_head(&q));
					return TRUE;
				}
			}
			g_free(f);
			g_free(p);
			if (gtk_tree_model_iter_children(m, &child, cur)) {
				GtkTreeIter *c = g_new(GtkTreeIter, 1);
				*c = child;
				g_queue_push_tail(&q, c);
			}
		} while (gtk_tree_model_iter_next(m, cur));
		g_free(cur);
	}
	return FALSE;
}

static void lw_select(const gchar *file, const gchar *folder)
{
	GtkTreeIter it;
	GtkTreePath *path;
	if (!lw_filter_iter_for(file, folder, &it))
		return;
	path = gtk_tree_model_get_path(lw->filter, &it);
	gtk_tree_view_expand_to_path(lw->tree, path);
	gtk_tree_view_set_cursor(lw->tree, path, NULL, FALSE);
	gtk_tree_view_scroll_to_cell(lw->tree, path, NULL, FALSE, 0, 0);
	gtk_tree_path_free(path);
}

static void lw_update_status(void)
{
	gchar *s;
	s = g_strdup_printf(_("%d connections in %d folders"), lw->n_connections, lw->n_folders);
	gtk_label_set_text(lw->status, s);
	g_free(s);
}

static void lw_reload(void)
{
	GPtrArray *expanded, *saved;
	gchar *keep_file, *keep_folder;
	guint i;
	gboolean first = (gtk_tree_model_iter_n_children(GTK_TREE_MODEL(lw->store), NULL) == 0);

	expanded = g_ptr_array_new_with_free_func(g_free);
	if (first) {
		gchar *v = remmina_pref_get_value("lremote_expanded");
		if (v && *v) {
			gchar **parts = g_strsplit(v, ";", -1);
			for (i = 0; parts[i]; i++)
				g_ptr_array_add(expanded, g_strdup(parts[i]));
			g_strfreev(parts);
		}
		g_free(v);
	} else {
		gtk_tree_view_map_expanded_rows(lw->tree, lw_collect_expanded, expanded);
	}
	keep_file = g_strdup(lw->selected_file);
	keep_folder = g_strdup(lw->selected_folder);

	lw->loading = TRUE;
	gtk_tree_store_clear(lw->store);
	g_hash_table_remove_all(lw->folder_iters);
	lw->n_connections = 0;
	lw->n_folders = 0;

	saved = lw_saved_folders();
	for (i = 0; i < saved->len; i++)
		lw_ensure_folder(g_ptr_array_index(saved, i));
	g_ptr_array_free(saved, TRUE);
	remmina_file_manager_iterate(lw_add_profile, NULL);

	gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(lw->filter));
	for (i = 0; i < expanded->len; i++) {
		GtkTreeIter it;
		if (lw_filter_iter_for(NULL, g_ptr_array_index(expanded, i), &it)) {
			GtkTreePath *p = gtk_tree_model_get_path(lw->filter, &it);
			gtk_tree_view_expand_to_path(lw->tree, p);
			gtk_tree_path_free(p);
		}
	}
	g_ptr_array_free(expanded, TRUE);
	lw->loading = FALSE;

	if (keep_file || keep_folder)
		lw_select(keep_file, keep_file ? NULL : keep_folder);
	g_free(keep_file);
	g_free(keep_folder);
	lw_update_status();
	if (!lw->selected_file)
		lw_update_start_page(NULL);
}

void lremote_window_refresh(void)
{
	if (lw)
		lw_reload();
}

/* ------------------------------------------------------------------ */
/* Config panel                                                        */

static gboolean lw_add_protocol(gchar *name, RemminaPlugin *plugin, gpointer data)
{
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(data), plugin->name, plugin->name);
	return FALSE;
}

static void lw_fill_folder_combo(GtkComboBoxText *combo, const gchar *active)
{
	GList *keys, *l;
	gtk_combo_box_text_remove_all(combo);
	gtk_combo_box_text_append(combo, "", _("(top level)"));
	keys = g_list_sort(g_hash_table_get_keys(lw->folder_iters), (GCompareFunc)g_utf8_collate);
	for (l = keys; l; l = l->next)
		gtk_combo_box_text_append(combo, l->data, l->data);
	g_list_free(keys);
	if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), active ? active : ""))
		gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
}

static void lw_props_set_sensitive(gboolean on)
{
	gtk_widget_set_sensitive(lw->props_grid, on);
	gtk_widget_set_sensitive(lw->p_buttons, on);
}

static void lw_props_clear(void)
{
	GtkTextBuffer *b = gtk_text_view_get_buffer(lw->p_notes);
	gtk_entry_set_text(lw->p_name, "");
	gtk_entry_set_text(lw->p_server, "");
	gtk_entry_set_text(lw->p_username, "");
	gtk_entry_set_text(lw->p_password, "");
	gtk_entry_set_text(lw->p_domain, "");
	gtk_entry_set_text(lw->p_res_custom, "");
	gtk_text_buffer_set_text(b, "", -1);
}

static void lw_update_start_page(RemminaFile *f)
{
	gchar *s;
	if (f) {
		const gchar *proto = remmina_file_get_string(f, "protocol");
		const gchar *server = remmina_file_get_string(f, "server");
		const gchar *user = remmina_file_get_string(f, "username");
		gtk_label_set_text(lw->start_title, remmina_file_get_string(f, "name"));
		s = g_strdup_printf("%s  ·  %s%s%s", proto ? proto : "?", (user && *user) ? user : "",
				    (user && *user) ? "@" : "", (server && *server) ? server : _("no server"));
		gtk_label_set_text(lw->start_detail, s);
		g_free(s);
		gtk_widget_show(lw->start_connect);
	} else {
		gtk_label_set_text(lw->start_title, "lRemote");
		s = g_strdup_printf(_("%d connections in %d folders. Double-click a connection to open it in a tab."),
				    lw->n_connections, lw->n_folders);
		gtk_label_set_text(lw->start_detail, s);
		g_free(s);
		gtk_widget_hide(lw->start_connect);
	}
}

static void lw_props_load(const gchar *file)
{
	RemminaFile *f;
	gint res_mode, depth, i;
	const gchar *notes;
	gchar *group;

	lw_props_clear();
	if (!file) {
		lw_props_set_sensitive(FALSE);
		gtk_label_set_text(lw->props_title, lw->selected_folder ? _("Folder") : _("Config"));
		lw_update_start_page(NULL);
		return;
	}
	f = remmina_file_load(file);
	if (!f) {
		lw_props_set_sensitive(FALSE);
		return;
	}
	lw_props_set_sensitive(TRUE);
	gtk_label_set_text(lw->props_title, _("Config"));
	gtk_entry_set_text(lw->p_name, remmina_file_get_string(f, "name") ? remmina_file_get_string(f, "name") : "");
	group = lw_clean_folder(remmina_file_get_string(f, "group"));
	lw_fill_folder_combo(lw->p_folder, group);
	g_free(group);
	if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(lw->p_protocol), remmina_file_get_string(f, "protocol")))
		gtk_combo_box_set_active(GTK_COMBO_BOX(lw->p_protocol), -1);
	gtk_entry_set_text(lw->p_server, remmina_file_get_string(f, "server") ? remmina_file_get_string(f, "server") : "");
	gtk_entry_set_text(lw->p_username, remmina_file_get_string(f, "username") ? remmina_file_get_string(f, "username") : "");
	gtk_entry_set_text(lw->p_domain, remmina_file_get_string(f, "domain") ? remmina_file_get_string(f, "domain") : "");
	gtk_entry_set_placeholder_text(lw->p_password,
				       (remmina_file_get_string(f, "password") && *remmina_file_get_string(f, "password"))
				       ? _("Saved (type to change)") : _("Not saved"));

	res_mode = remmina_file_get_int(f, "resolution_mode", RES_USE_INITIAL_WINDOW_SIZE);
	if (res_mode == RES_USE_CUSTOM) {
		gchar *wh = g_strdup_printf("%dx%d", remmina_file_get_int(f, "resolution_width", 1024),
					    remmina_file_get_int(f, "resolution_height", 768));
		gtk_combo_box_set_active(GTK_COMBO_BOX(lw->p_resolution), RES_ROW_CUSTOM);
		gtk_entry_set_text(lw->p_res_custom, wh);
		g_free(wh);
	} else if (res_mode == RES_USE_CLIENT) {
		gtk_combo_box_set_active(GTK_COMBO_BOX(lw->p_resolution), RES_ROW_CLIENT);
	} else {
		gtk_combo_box_set_active(GTK_COMBO_BOX(lw->p_resolution), RES_ROW_WINDOW);
	}
	depth = remmina_file_get_int(f, "colordepth", 0);
	gtk_combo_box_set_active(GTK_COMBO_BOX(lw->p_colordepth), 0);
	for (i = 0; i < (gint)G_N_ELEMENTS(colordepths); i++)
		if (colordepths[i] == depth)
			gtk_combo_box_set_active(GTK_COMBO_BOX(lw->p_colordepth), i);

	notes = remmina_file_get_string(f, "notes_text");
	gtk_text_buffer_set_text(gtk_text_view_get_buffer(lw->p_notes), notes ? notes : "", -1);

	lw_update_start_page(f);
	remmina_file_free(f);
}

static void lw_on_res_changed(GtkComboBox *c, gpointer data)
{
	gtk_widget_set_sensitive(GTK_WIDGET(lw->p_res_custom), gtk_combo_box_get_active(c) == RES_ROW_CUSTOM);
}

static void lw_props_save(GtkWidget *w, gpointer data)
{
	RemminaFile *f;
	const gchar *pw, *id;
	GtkTextIter s, e;
	gchar *notes, *folder;
	gint row, depth_row;

	if (!lw->selected_file)
		return;
	f = remmina_file_load(lw->selected_file);
	if (!f)
		return;
	remmina_file_set_string(f, "name", gtk_entry_get_text(lw->p_name));
	id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(lw->p_folder));
	folder = lw_clean_folder(id);
	remmina_file_set_string(f, "group", *folder ? folder : NULL);
	g_free(folder);
	id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(lw->p_protocol));
	if (id)
		remmina_file_set_string(f, "protocol", id);
	remmina_file_set_string(f, "server", gtk_entry_get_text(lw->p_server));
	remmina_file_set_string(f, "username", gtk_entry_get_text(lw->p_username));
	remmina_file_set_string(f, "domain", gtk_entry_get_text(lw->p_domain));
	pw = gtk_entry_get_text(lw->p_password);
	if (pw && *pw)
		remmina_file_set_string(f, "password", pw);

	row = gtk_combo_box_get_active(GTK_COMBO_BOX(lw->p_resolution));
	if (row == RES_ROW_CUSTOM) {
		gint wd = 0, ht = 0;
		if (sscanf(gtk_entry_get_text(lw->p_res_custom), "%dx%d", &wd, &ht) == 2 && wd > 0 && ht > 0) {
			remmina_file_set_int(f, "resolution_mode", RES_USE_CUSTOM);
			remmina_file_set_int(f, "resolution_width", wd);
			remmina_file_set_int(f, "resolution_height", ht);
		}
	} else if (row == RES_ROW_CLIENT) {
		remmina_file_set_int(f, "resolution_mode", RES_USE_CLIENT);
	} else {
		remmina_file_set_int(f, "resolution_mode", RES_USE_INITIAL_WINDOW_SIZE);
	}
	depth_row = gtk_combo_box_get_active(GTK_COMBO_BOX(lw->p_colordepth));
	if (depth_row > 0)
		remmina_file_set_int(f, "colordepth", colordepths[depth_row]);

	gtk_text_buffer_get_bounds(gtk_text_view_get_buffer(lw->p_notes), &s, &e);
	notes = gtk_text_buffer_get_text(gtk_text_view_get_buffer(lw->p_notes), &s, &e, FALSE);
	remmina_file_set_string(f, "notes_text", notes);
	g_free(notes);

	remmina_file_save(f);
	remmina_file_free(f);
	gtk_entry_set_text(lw->p_password, "");
	lw_reload();
}

/* ------------------------------------------------------------------ */
/* actions                                                             */

static void lw_connect_file(const gchar *file)
{
	if (file)
		rcw_open_from_filename(file);
}

static void lw_connect_selected(GtkWidget *w, gpointer data)
{
	lw_connect_file(lw->selected_file);
}

static gchar *lw_target_folder(void)
{
	GtkTreeIter it;
	gchar *p = NULL;
	if (lw->selected_folder)
		return g_strdup(lw->selected_folder);
	if (lw->selected_file && lw_filter_iter_for(lw->selected_file, NULL, &it)) {
		gtk_tree_model_get(lw->filter, &it, COL_PATH, &p, -1);
		return p;
	}
	return g_strdup("");
}

static void lw_new_connection(GtkWidget *w, gpointer data)
{
	RemminaFile *f = remmina_file_new();
	gchar *folder = lw_target_folder();
	gchar *file;

	remmina_file_set_string(f, "name", _("New connection"));
	remmina_file_set_string(f, "protocol", "RDP");
	if (*folder)
		remmina_file_set_string(f, "group", folder);
	remmina_file_generate_filename(f);
	if (!remmina_file_get_filename(f)) {
		lw_message(GTK_MESSAGE_ERROR, _("Could not create the connection."), NULL);
		remmina_file_free(f);
		g_free(folder);
		return;
	}
	remmina_file_set_statefile(f);
	remmina_file_save(f);
	file = g_strdup(remmina_file_get_filename(f));
	remmina_file_free(f);
	g_free(folder);
	g_free(lw->selected_file);
	lw->selected_file = file;
	g_free(lw->selected_folder);
	lw->selected_folder = NULL;
	lw_reload();
	gtk_widget_grab_focus(GTK_WIDGET(lw->p_name));
	gtk_editable_select_region(GTK_EDITABLE(lw->p_name), 0, -1);
}

static void lw_new_folder(GtkWidget *w, gpointer data)
{
	gchar *parent = lw_target_folder();
	gchar *name = lw_ask_text(_("New folder"), _("Folder name"), NULL, FALSE);
	gchar *clean, *path;

	if (name) {
		clean = lw_clean_folder(name);
		if (*clean) {
			path = *parent ? g_strdup_printf("%s/%s", parent, clean) : g_strdup(clean);
			lw_folder_remember(path);
			g_free(lw->selected_file);
			lw->selected_file = NULL;
			g_free(lw->selected_folder);
			lw->selected_folder = path;
			lw_reload();
		}
		g_free(clean);
		g_free(name);
	}
	g_free(parent);
}

static void lw_edit_advanced(GtkWidget *w, gpointer data)
{
	GtkWidget *editor;
	if (!lw->selected_file)
		return;
	editor = remmina_file_editor_new_from_filename(lw->selected_file);
	if (editor) {
		gtk_window_set_transient_for(GTK_WINDOW(editor), lw->window);
		gtk_widget_show(editor);
	}
}

static void lw_duplicate(GtkWidget *w, gpointer data)
{
	RemminaFile *f;
	gchar *name;
	if (!lw->selected_file)
		return;
	f = remmina_file_copy(lw->selected_file);
	if (!f)
		return;
	/* TRANSLATORS: name given to a duplicated connection, %s is the original name */
	name = g_strdup_printf(_("%s (copy)"), remmina_file_get_string(f, "name"));
	remmina_file_set_string(f, "name", name);
	g_free(name);
	remmina_file_generate_filename(f);
	remmina_file_set_statefile(f);
	remmina_file_save(f);
	g_free(lw->selected_file);
	lw->selected_file = g_strdup(remmina_file_get_filename(f));
	remmina_file_free(f);
	lw_reload();
}

static void lw_delete_selected(GtkWidget *w, gpointer data)
{
	gchar *msg;
	GtkTreeIter it;
	gchar *name = NULL;

	if (lw->selected_file) {
		if (lw_filter_iter_for(lw->selected_file, NULL, &it))
			gtk_tree_model_get(lw->filter, &it, COL_NAME, &name, -1);
		msg = g_strdup_printf(_("Delete the connection “%s”?"), name ? name : "");
		if (lw_confirm(msg, NULL, _("_Delete"))) {
			remmina_file_delete(lw->selected_file);
			g_free(lw->selected_file);
			lw->selected_file = NULL;
			lw_reload();
			lw_props_load(NULL);
		}
		g_free(msg);
		g_free(name);
	} else if (lw->selected_folder) {
		/* Delete the folder; its connections move to the parent folder */
		gchar *folder = g_strdup(lw->selected_folder);
		gchar *slash = strrchr(folder, '/');
		gchar *parent = slash ? g_strndup(folder, slash - folder) : g_strdup("");
		msg = g_strdup_printf(_("Delete the folder “%s”?"), folder);
		if (lw_confirm(msg, _("Connections inside it are kept and moved to the parent folder."), _("_Delete"))) {
			GDir *dir;
			gchar *datadir = remmina_file_get_datadir();
			const gchar *fn;
			gsize fl = strlen(folder);
			dir = g_dir_open(datadir, 0, NULL);
			while (dir && (fn = g_dir_read_name(dir))) {
				gchar *path;
				RemminaFile *f;
				if (!g_str_has_suffix(fn, ".remmina"))
					continue;
				path = g_build_filename(datadir, fn, NULL);
				f = remmina_file_load(path);
				if (f) {
					gchar *g = lw_clean_folder(remmina_file_get_string(f, "group"));
					if (g_strcmp0(g, folder) == 0 || (g_str_has_prefix(g, folder) && g[fl] == '/')) {
						gchar *ng = *parent ? g_strconcat(parent, g + fl, NULL)
							    : g_strdup(g[fl] == '/' ? g + fl + 1 : "");
						remmina_file_set_string(f, "group", *ng ? ng : NULL);
						remmina_file_save(f);
						g_free(ng);
					}
					g_free(g);
					remmina_file_free(f);
				}
				g_free(path);
			}
			if (dir)
				g_dir_close(dir);
			lw_folder_rewrite_saved(folder, *parent ? parent : NULL);
			if (*parent)
				lw_folder_remember(parent);
			g_free(lw->selected_folder);
			lw->selected_folder = NULL;
			lw_reload();
			lw_props_load(NULL);
		}
		g_free(msg);
		g_free(parent);
		g_free(folder);
	}
}

/* Renames or moves every profile below old_path */
static void lw_rewrite_group(const gchar *old_path, const gchar *new_path)
{
	GDir *dir;
	gchar *datadir = remmina_file_get_datadir();
	const gchar *fn;
	gsize ol = strlen(old_path);

	dir = g_dir_open(datadir, 0, NULL);
	while (dir && (fn = g_dir_read_name(dir))) {
		gchar *path;
		RemminaFile *f;
		if (!g_str_has_suffix(fn, ".remmina"))
			continue;
		path = g_build_filename(datadir, fn, NULL);
		f = remmina_file_load(path);
		if (f) {
			gchar *g = lw_clean_folder(remmina_file_get_string(f, "group"));
			if (g_strcmp0(g, old_path) == 0 || (g_str_has_prefix(g, old_path) && g[ol] == '/')) {
				gchar *ng = g_strconcat(new_path, g + ol, NULL);
				remmina_file_set_string(f, "group", ng);
				remmina_file_save(f);
				g_free(ng);
			}
			g_free(g);
			remmina_file_free(f);
		}
		g_free(path);
	}
	if (dir)
		g_dir_close(dir);
	lw_folder_rewrite_saved(old_path, new_path);
	lw_folder_remember(new_path);
}

static void lw_rename_folder(GtkWidget *w, gpointer data)
{
	gchar *folder, *slash, *base, *name, *clean, *np;
	if (!lw->selected_folder)
		return;
	folder = g_strdup(lw->selected_folder);
	slash = strrchr(folder, '/');
	base = slash ? slash + 1 : folder;
	name = lw_ask_text(_("Rename folder"), _("New name"), base, FALSE);
	if (name) {
		clean = lw_clean_folder(name);
		if (*clean && !strchr(clean, '/') && g_strcmp0(clean, base) != 0) {
			np = slash ? g_strdup_printf("%.*s/%s", (int)(slash - folder), folder, clean) : g_strdup(clean);
			lw_rewrite_group(folder, np);
			g_free(lw->selected_folder);
			lw->selected_folder = np;
			lw_reload();
		}
		g_free(clean);
		g_free(name);
	}
	g_free(folder);
}

static void lw_move_to_folder(GtkWidget *w, gpointer data)
{
	GtkWidget *d, *box, *combo, *lbl;
	RemminaFile *f;
	gchar *cur;

	if (!lw->selected_file)
		return;
	f = remmina_file_load(lw->selected_file);
	if (!f)
		return;
	d = gtk_dialog_new_with_buttons(_("Move to folder"), lw->window, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
					_("_Cancel"), GTK_RESPONSE_CANCEL, _("_Move"), GTK_RESPONSE_OK, NULL);
	box = gtk_dialog_get_content_area(GTK_DIALOG(d));
	gtk_container_set_border_width(GTK_CONTAINER(box), 12);
	gtk_box_set_spacing(GTK_BOX(box), 8);
	lbl = gtk_label_new(_("Destination folder"));
	gtk_label_set_xalign(GTK_LABEL(lbl), 0);
	combo = gtk_combo_box_text_new();
	cur = lw_clean_folder(remmina_file_get_string(f, "group"));
	lw_fill_folder_combo(GTK_COMBO_BOX_TEXT(combo), cur);
	g_free(cur);
	gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), combo, FALSE, FALSE, 0);
	gtk_widget_show_all(d);
	if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK) {
		const gchar *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo));
		remmina_file_set_string(f, "group", (id && *id) ? id : NULL);
		remmina_file_save(f);
		lw_reload();
	}
	gtk_widget_destroy(d);
	remmina_file_free(f);
}

static void lw_connect_folder_rec(GtkTreeModel *m, GtkTreeIter *parent)
{
	GtkTreeIter child;
	if (!gtk_tree_model_iter_children(m, &child, parent))
		return;
	do {
		gchar *file = NULL;
		gboolean is_folder;
		gtk_tree_model_get(m, &child, COL_FILE, &file, COL_IS_FOLDER, &is_folder, -1);
		if (is_folder)
			lw_connect_folder_rec(m, &child);
		else
			lw_connect_file(file);
		g_free(file);
	} while (gtk_tree_model_iter_next(m, &child));
}

static void lw_connect_folder(GtkWidget *w, gpointer data)
{
	GtkTreeIter *it;
	if (!lw->selected_folder)
		return;
	it = g_hash_table_lookup(lw->folder_iters, lw->selected_folder);
	if (it)
		lw_connect_folder_rec(GTK_TREE_MODEL(lw->store), it);
}

static void lw_expand_all(GtkWidget *w, gpointer data)
{
	gtk_tree_view_expand_all(lw->tree);
}

static void lw_collapse_all(GtkWidget *w, gpointer data)
{
	gtk_tree_view_collapse_all(lw->tree);
}

/* ---- import / export ---- */

static void lw_import_mremoteng(GtkWidget *w, gpointer data)
{
	GtkFileChooserNative *ch;
	GtkFileFilter *flt;
	gchar *path, *password = NULL;
	gint attempt;

	ch = gtk_file_chooser_native_new(_("Import from mRemoteNG"), lw->window, GTK_FILE_CHOOSER_ACTION_OPEN,
					 _("_Open"), _("_Cancel"));
	flt = gtk_file_filter_new();
	gtk_file_filter_set_name(flt, _("mRemoteNG connections (confCons.xml)"));
	gtk_file_filter_add_pattern(flt, "*.xml");
	gtk_file_filter_add_pattern(flt, "*.XML");
	gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(ch), flt);
	flt = gtk_file_filter_new();
	gtk_file_filter_set_name(flt, _("All files"));
	gtk_file_filter_add_pattern(flt, "*");
	gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(ch), flt);
	if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(ch)) != GTK_RESPONSE_ACCEPT) {
		g_object_unref(ch);
		return;
	}
	path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(ch));
	g_object_unref(ch);

	for (attempt = 0; attempt < 4; attempt++) {
		LremoteMrngStats st = { 0 };
		GError *err = NULL;
		if (lremote_mremoteng_import(path, password, &st, &err)) {
			gchar *msg = g_strdup_printf(_("%d connections and %d folders were imported."), st.imported, st.folders);
			gchar *extra = NULL;
			if (st.unsupported > 0)
				extra = g_strdup_printf(_("%d connections use protocols lRemote does not support yet and were skipped: %s"),
							st.unsupported, st.unsupported_names ? st.unsupported_names->str : "");
			lw_reload();
			lw_message(GTK_MESSAGE_INFO, msg, extra);
			g_free(msg);
			g_free(extra);
			lremote_mremoteng_stats_clear(&st);
			break;
		}
		lremote_mremoteng_stats_clear(&st);
		if (err && err->code == 3) {	/* wrong master password */
			g_free(password);
			password = lw_ask_text(_("Master password"),
					       attempt == 0
					       ? _("This file is protected with a master password. Type the password set in mRemoteNG.")
					       : _("Wrong password. Try again."),
					       NULL, TRUE);
			g_clear_error(&err);
			if (!password)
				break;
			continue;
		}
		lw_message(GTK_MESSAGE_ERROR, _("Could not import the mRemoteNG file."), err ? err->message : NULL);
		g_clear_error(&err);
		break;
	}
	if (password) {
		memset(password, 0, strlen(password));
		g_free(password);
	}
	g_free(path);
}

/* Copies Remmina profiles, keeping their passwords readable */
static void lw_import_remmina(GtkWidget *w, gpointer data)
{
	gchar *srcdirs[3], *secret = NULL, *saved_secret, *datadir, *prefpath;
	GKeyFile *kf;
	gint i, copied = 0, skipped = 0;

	srcdirs[0] = g_build_filename(g_get_user_data_dir(), "remmina", NULL);
	srcdirs[1] = g_build_filename(g_get_home_dir(), ".remmina", NULL);
	srcdirs[2] = NULL;

	/* Remmina encrypts saved passwords with the key in its own preferences */
	prefpath = g_build_filename(g_get_user_config_dir(), "remmina", "remmina.pref", NULL);
	kf = g_key_file_new();
	if (g_key_file_load_from_file(kf, prefpath, G_KEY_FILE_NONE, NULL))
		secret = g_key_file_get_string(kf, "remmina_pref", "secret", NULL);
	g_key_file_free(kf);
	g_free(prefpath);

	datadir = remmina_file_get_datadir();
	g_mkdir_with_parents(datadir, 0700);
	for (i = 0; srcdirs[i]; i++) {
		GDir *dir = g_dir_open(srcdirs[i], 0, NULL);
		const gchar *fn;
		while (dir && (fn = g_dir_read_name(dir))) {
			gchar *src, *dst;
			RemminaFile *f;
			if (!g_str_has_suffix(fn, ".remmina"))
				continue;
			src = g_build_filename(srcdirs[i], fn, NULL);
			dst = g_build_filename(datadir, fn, NULL);
			if (g_file_test(dst, G_FILE_TEST_EXISTS)) {
				skipped++;
			} else {
				saved_secret = remmina_pref.secret;
				if (secret)
					remmina_pref.secret = secret;
				f = remmina_file_load(src);
				remmina_pref.secret = saved_secret;
				if (f) {
					remmina_file_set_filename(f, dst);
					remmina_file_set_statefile(f);
					remmina_file_save(f);
					remmina_file_free(f);
					copied++;
				}
			}
			g_free(src);
			g_free(dst);
		}
		if (dir)
			g_dir_close(dir);
	}
	g_free(srcdirs[0]);
	g_free(srcdirs[1]);
	g_free(secret);

	lw_reload();
	{
		gchar *msg = g_strdup_printf(_("%d connections were copied from Remmina."), copied);
		gchar *extra = skipped ? g_strdup_printf(_("%d already existed in lRemote and were kept."), skipped) : NULL;
		lw_message(GTK_MESSAGE_INFO, copied || skipped ? msg : _("No Remmina connections were found."), extra);
		g_free(msg);
		g_free(extra);
	}
}

static void lw_backup_filters(GtkFileChooser *ch)
{
	GtkFileFilter *flt = gtk_file_filter_new();
	gtk_file_filter_set_name(flt, _("lRemote settings backup"));
	gtk_file_filter_add_pattern(flt, "*" REMMINA_BACKUP_EXT);
	gtk_file_chooser_add_filter(ch, flt);
	flt = gtk_file_filter_new();
	gtk_file_filter_set_name(flt, _("All files"));
	gtk_file_filter_add_pattern(flt, "*");
	gtk_file_chooser_add_filter(ch, flt);
}

static void lw_export_settings(GtkWidget *w, gpointer data)
{
	GtkFileChooserNative *ch;
	GDateTime *now = g_date_time_new_now_local();
	gchar *stamp = g_date_time_format(now, "%Y-%m-%d"), *name, *path;
	RemminaBackupStats st;
	GError *err = NULL;

	ch = gtk_file_chooser_native_new(_("Export settings"), lw->window, GTK_FILE_CHOOSER_ACTION_SAVE, _("_Save"), _("_Cancel"));
	gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(ch), TRUE);
	lw_backup_filters(GTK_FILE_CHOOSER(ch));
	name = g_strdup_printf("lremote-%s%s", stamp, REMMINA_BACKUP_EXT);
	gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(ch), name);
	g_free(name);
	g_free(stamp);
	g_date_time_unref(now);
	if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(ch)) == GTK_RESPONSE_ACCEPT) {
		path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(ch));
		if (remmina_backup_export(path, &st, &err)) {
			gchar *msg = g_strdup_printf(ngettext("%d connection profile and the preferences were exported, without passwords.",
							      "%d connection profiles and the preferences were exported, without passwords.",
							      st.connections), st.connections);
			lw_message(GTK_MESSAGE_INFO, msg, NULL);
			g_free(msg);
		} else {
			lw_message(GTK_MESSAGE_ERROR, _("Export settings"), err ? err->message : NULL);
			g_clear_error(&err);
		}
		g_free(path);
	}
	g_object_unref(ch);
}

static void lw_import_settings(GtkWidget *w, gpointer data)
{
	GtkFileChooserNative *ch;
	gchar *path;
	RemminaBackupStats st;
	GError *err = NULL;
	GtkWidget *ask;
	gint choice;

	ch = gtk_file_chooser_native_new(_("Import settings"), lw->window, GTK_FILE_CHOOSER_ACTION_OPEN, _("_Open"), _("_Cancel"));
	lw_backup_filters(GTK_FILE_CHOOSER(ch));
	if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(ch)) != GTK_RESPONSE_ACCEPT) {
		g_object_unref(ch);
		return;
	}
	path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(ch));
	g_object_unref(ch);

	ask = gtk_message_dialog_new(lw->window, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, _("Import settings"));
	gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(ask),
		_("Profiles that already exist can be kept or replaced. Passwords are never part of a backup, so replaced profiles keep the passwords saved on this computer."));
	gtk_dialog_add_buttons(GTK_DIALOG(ask), _("_Cancel"), GTK_RESPONSE_CANCEL, _("_Keep existing"), 1, _("_Replace"), 2, NULL);
	gtk_dialog_set_default_response(GTK_DIALOG(ask), 1);
	choice = gtk_dialog_run(GTK_DIALOG(ask));
	gtk_widget_destroy(ask);
	if (choice == 1 || choice == 2) {
		if (remmina_backup_import(path, choice == 2 ? REMMINA_BACKUP_REPLACE : REMMINA_BACKUP_MERGE, &st, &err)) {
			gchar *msg;
			if (st.preferences) {
				remmina_pref_init();
				remmina_theme_apply();
			}
			lw_reload();
			msg = g_strdup_printf(ngettext("%d connection profile was imported.", "%d connection profiles were imported.",
						       st.connections), st.connections);
			lw_message(GTK_MESSAGE_INFO, msg, NULL);
			g_free(msg);
		} else {
			lw_message(GTK_MESSAGE_ERROR, _("Import settings"), err ? err->message : NULL);
			g_clear_error(&err);
		}
	}
	g_free(path);
}

/* ---- misc ---- */

static void lw_preferences(GtkWidget *w, gpointer data)
{
	GtkWidget *d = remmina_pref_dialog_new(0, lw->window);
	gtk_widget_show(d);
}

static void lw_plugins(GtkWidget *w, gpointer data)
{
	remmina_plugin_manager_show(lw->window);
}

static void lw_about(GtkWidget *w, gpointer data)
{
	const gchar *authors[] = { "mpjsilva", NULL };
	gtk_show_about_dialog(lw->window,
			      "program-name", "lRemote",
			      "version", LREMOTE_VERSION,
			      "logo-icon-name", REMMINA_APP_ID,
			      "comments", _("Remote connections manager for Linux, inspired by mRemoteNG and built on the Remmina engine."),
			      "website", "https://github.com/mpjsilva/lRemote",
			      "license-type", GTK_LICENSE_GPL_2_0,
			      "authors", authors,
			      "copyright", _("Based on Remmina © Antenore Gatta, Giovanni Panozzo and contributors"),
			      NULL);
}

static void lw_quit(GtkWidget *w, gpointer data)
{
	remmina_exec_command(REMMINA_COMMAND_EXIT, NULL);
}

static void lw_theme_toggled(GtkCheckMenuItem *item, gpointer data)
{
	if (!gtk_check_menu_item_get_active(item))
		return;
	remmina_pref.color_scheme = GPOINTER_TO_INT(data);
	remmina_theme_apply();
	remmina_pref_save();
}

static void lw_quick_connect(GtkWidget *w, gpointer data)
{
	const gchar *server = gtk_entry_get_text(lw->quick_server);
	const gchar *proto = gtk_combo_box_get_active_id(GTK_COMBO_BOX(lw->quick_proto));
	RemminaFile *f;
	gchar *s, *at;

	if (!server || !*server || !proto)
		return;
	f = remmina_file_new();
	s = g_strstrip(g_strdup(server));
	at = strrchr(s, '@');
	if (at) {	/* user@host */
		*at = '\0';
		remmina_file_set_string(f, "username", s);
		remmina_file_set_string(f, "server", at + 1);
		remmina_file_set_string(f, "name", at + 1);
	} else {
		remmina_file_set_string(f, "server", s);
		remmina_file_set_string(f, "name", s);
	}
	remmina_file_set_string(f, "protocol", proto);
	g_free(s);
	rcw_open_from_file(f);
}

/* ------------------------------------------------------------------ */
/* tree events                                                         */

static void lw_on_selection(GtkTreeSelection *sel, gpointer data)
{
	GtkTreeModel *m;
	GtkTreeIter it;
	gchar *file = NULL, *path = NULL;
	gboolean is_folder = FALSE;

	if (lw->loading)
		return;
	g_free(lw->selected_file);
	g_free(lw->selected_folder);
	lw->selected_file = NULL;
	lw->selected_folder = NULL;
	if (gtk_tree_selection_get_selected(sel, &m, &it)) {
		gtk_tree_model_get(m, &it, COL_FILE, &file, COL_PATH, &path, COL_IS_FOLDER, &is_folder, -1);
		if (is_folder) {
			lw->selected_folder = path;
			path = NULL;
		} else {
			lw->selected_file = file;
			file = NULL;
		}
	}
	g_free(file);
	g_free(path);
	lw_props_load(lw->selected_file);
}

static void lw_on_row_activated(GtkTreeView *tv, GtkTreePath *path, GtkTreeViewColumn *col, gpointer data)
{
	if (lw->selected_file) {
		lw_connect_file(lw->selected_file);
	} else if (gtk_tree_view_row_expanded(tv, path)) {
		gtk_tree_view_collapse_row(tv, path);
	} else {
		gtk_tree_view_expand_row(tv, path, FALSE);
	}
}

static GtkWidget *lw_menu_item(GtkWidget *menu, const gchar *label, GCallback cb)
{
	GtkWidget *item = gtk_menu_item_new_with_mnemonic(label);
	g_signal_connect(item, "activate", cb, NULL);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
	return item;
}

static void lw_menu_sep(GtkWidget *menu)
{
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
}

static gboolean lw_on_tree_button(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
	GtkTreePath *path = NULL;
	GtkWidget *menu;

	if (ev->type != GDK_BUTTON_PRESS || ev->button != 3)
		return FALSE;
	if (gtk_tree_view_get_path_at_pos(lw->tree, (gint)ev->x, (gint)ev->y, &path, NULL, NULL, NULL)) {
		gtk_tree_view_set_cursor(lw->tree, path, NULL, FALSE);
		gtk_tree_path_free(path);
	} else {
		gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(lw->tree));
	}

	menu = gtk_menu_new();
	if (lw->selected_file) {
		lw_menu_item(menu, _("_Connect"), G_CALLBACK(lw_connect_selected));
		lw_menu_item(menu, _("Connect in a new _window"), G_CALLBACK(lw_connect_selected_window));
		lw_menu_sep(menu);
		lw_menu_item(menu, _("_Edit (all settings)…"), G_CALLBACK(lw_edit_advanced));
		lw_menu_item(menu, _("D_uplicate"), G_CALLBACK(lw_duplicate));
		lw_menu_item(menu, _("_Move to folder…"), G_CALLBACK(lw_move_to_folder));
		lw_menu_sep(menu);
		lw_menu_item(menu, _("_Delete"), G_CALLBACK(lw_delete_selected));
	} else if (lw->selected_folder) {
		lw_menu_item(menu, _("Connect _all"), G_CALLBACK(lw_connect_folder));
		lw_menu_sep(menu);
		lw_menu_item(menu, _("New _connection"), G_CALLBACK(lw_new_connection));
		lw_menu_item(menu, _("New _folder"), G_CALLBACK(lw_new_folder));
		lw_menu_item(menu, _("_Rename…"), G_CALLBACK(lw_rename_folder));
		lw_menu_sep(menu);
		lw_menu_item(menu, _("_Delete"), G_CALLBACK(lw_delete_selected));
	} else {
		lw_menu_item(menu, _("New _connection"), G_CALLBACK(lw_new_connection));
		lw_menu_item(menu, _("New _folder"), G_CALLBACK(lw_new_folder));
	}
	gtk_widget_show_all(menu);
	gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
	return TRUE;
}

static gboolean lw_on_tree_key(GtkWidget *w, GdkEventKey *ev, gpointer data)
{
	if (ev->keyval == GDK_KEY_Delete) {
		lw_delete_selected(NULL, NULL);
		return TRUE;
	}
	if (ev->keyval == GDK_KEY_F2 && lw->selected_folder) {
		lw_rename_folder(NULL, NULL);
		return TRUE;
	}
	if (ev->keyval == GDK_KEY_F2 && lw->selected_file) {
		gtk_widget_grab_focus(GTK_WIDGET(lw->p_name));
		return TRUE;
	}
	return FALSE;
}

static void lw_on_search(GtkSearchEntry *e, gpointer data)
{
	gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(lw->filter));
	if (*gtk_entry_get_text(GTK_ENTRY(e)))
		gtk_tree_view_expand_all(lw->tree);
}

/* ------------------------------------------------------------------ */
/* window                                                              */

static gint lw_pref_int(const gchar *key, gint def)
{
	gchar *v = remmina_pref_get_value(key);
	gint r = (v && *v) ? atoi(v) : def;
	g_free(v);
	return r > 0 ? r : def;
}

static void lw_pref_set_int(const gchar *key, gint val)
{
	gchar *v = g_strdup_printf("%d", val);
	remmina_pref_set_value(key, v);
	g_free(v);
}

void lremote_window_save_state(void)
{
	gint wd, ht;
	GPtrArray *exp;
	gchar *v;

	if (!lw || !lw->window)
		return;
	gtk_window_get_size(lw->window, &wd, &ht);
	lw_pref_set_int("lremote_width", wd);
	lw_pref_set_int("lremote_height", ht);
	lw_pref_set_int("lremote_hpaned", gtk_paned_get_position(lw->hpaned));
	exp = g_ptr_array_new_with_free_func(g_free);
	gtk_tree_view_map_expanded_rows(lw->tree, lw_collect_expanded, exp);
	g_ptr_array_add(exp, NULL);
	v = g_strjoinv(";", (gchar **)exp->pdata);
	remmina_pref_set_value("lremote_expanded", v);
	g_free(v);
	g_ptr_array_free(exp, TRUE);
}

static gboolean lw_pref_bool(const gchar *key, gboolean def)
{
	gchar *v = remmina_pref_get_value(key);
	gboolean on = (v && *v) ? g_strcmp0(v, "1") == 0 : def;
	g_free(v);
	return on;
}

/* Sessions open as tabs inside the main window (mRemoteNG style) */
gboolean lremote_window_embed_wanted(void)
{
	return lw != NULL && lw_pref_bool("lremote_session_tabs", TRUE);
}

void lremote_window_embed_attach(GtkWidget *grid)
{
	if (!lw)
		return;
	gtk_box_pack_start(GTK_BOX(lw->sessions), grid, TRUE, TRUE, 0);
	gtk_widget_show(grid);
	gtk_stack_set_visible_child_name(lw->right, "sessions");
}

void lremote_window_embed_detach(GtkWidget *grid)
{
	if (!lw || !grid)
		return;
	if (gtk_widget_get_parent(grid) == lw->sessions)
		gtk_widget_destroy(grid);
	gtk_stack_set_visible_child_name(lw->right, "start");
}

void lremote_window_embed_present(GtkWidget *grid)
{
	if (!lw)
		return;
	gtk_stack_set_visible_child_name(lw->right, "sessions");
	gtk_widget_show(GTK_WIDGET(lw->window));
	gtk_window_present_with_time(lw->window, (guint32)(g_get_monotonic_time() / 1000));
}

static gboolean lw_has_sessions(void)
{
	GList *l = gtk_container_get_children(GTK_CONTAINER(lw->sessions));
	gboolean any = l != NULL;
	g_list_free(l);
	return any;
}

/* While a session has focus, keys go to it first: menu accelerators and
 * mnemonics of the main window must not steal Ctrl+N, Alt+F and so on */
static gboolean lw_on_key(GtkWidget *w, GdkEventKey *ev, gpointer data)
{
	GtkWidget *focus = gtk_window_get_focus(lw->window);

	if (focus && gtk_widget_is_ancestor(focus, lw->sessions)) {
		gtk_window_propagate_key_event(lw->window, ev);
		return TRUE;
	}
	return FALSE;
}

static void lw_toggle_session_tabs(GtkCheckMenuItem *item, gpointer data)
{
	remmina_pref_set_value("lremote_session_tabs", gtk_check_menu_item_get_active(item) ? "1" : "0");
}

static void lw_connect_selected_window(GtkWidget *w, gpointer data)
{
	if (lw->selected_file)
		rcw_open_from_filename_in_window(lw->selected_file);
}

static gboolean lw_close_to_tray(void)
{
	gchar *v = remmina_pref_get_value("lremote_close_to_tray");
	gboolean on = v && g_strcmp0(v, "1") == 0;
	g_free(v);
	return on;
}

static gboolean lw_on_delete(GtkWidget *w, GdkEvent *ev, gpointer data)
{
	lremote_window_save_state();
	/* Keep running in the tray; "Restore" in the tray menu brings it back */
	if (lw_close_to_tray() && remmina_icon_is_available()) {
		gtk_widget_hide(w);
		return TRUE;
	}
	if (lw_has_sessions() && !lw_confirm(_("Close lRemote?"), _("Open connections will be closed."), _("_Close")))
		return TRUE;
	return FALSE;
}

static void lw_toggle_close_to_tray(GtkCheckMenuItem *item, gpointer data)
{
	remmina_pref_set_value("lremote_close_to_tray", gtk_check_menu_item_get_active(item) ? "1" : "0");
}

static void lw_check_updates(GtkWidget *w, gpointer data)
{
	lremote_update_check(lw->window, FALSE);
}

static gboolean lw_condexit(gpointer data)
{
	remmina_application_condexit(REMMINA_CONDEXIT_ONMAINWINDELETE);
	return G_SOURCE_REMOVE;
}

static void lw_on_destroy(GtkWidget *w, gpointer data)
{
	if (!lw)
		return;
	g_free(lw->selected_file);
	g_free(lw->selected_folder);
	g_hash_table_destroy(lw->folder_iters);
	g_object_unref(lw->filter);
	g_object_unref(lw->store);
	g_free(lw);
	lw = NULL;
	g_idle_add(lw_condexit, NULL);
}

static GtkWidget *lw_label(const gchar *text)
{
	GtkWidget *l = gtk_label_new(text);
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_style_context_add_class(gtk_widget_get_style_context(l), "dim-label");
	return l;
}

static void lw_grid_row(GtkGrid *g, gint row, const gchar *label, GtkWidget *w)
{
	gtk_grid_attach(g, lw_label(label), 0, row, 1, 1);
	gtk_widget_set_hexpand(w, TRUE);
	gtk_grid_attach(g, w, 1, row, 1, 1);
}

static GtkWidget *lw_build_menubar(GtkAccelGroup *accel)
{
	GtkWidget *bar = gtk_menu_bar_new(), *top, *menu, *item, *sub, *submenu;
	GSList *group = NULL;
	const gchar *labels[] = { N_("Follow system"), N_("Light"), N_("Dark") };
	gint i;

	/* File */
	top = gtk_menu_item_new_with_mnemonic(_("_File"));
	menu = gtk_menu_new();
	gtk_menu_item_set_submenu(GTK_MENU_ITEM(top), menu);
	item = lw_menu_item(menu, _("New _connection"), G_CALLBACK(lw_new_connection));
	gtk_widget_add_accelerator(item, "activate", accel, GDK_KEY_n, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE);
	item = lw_menu_item(menu, _("New _folder"), G_CALLBACK(lw_new_folder));
	gtk_widget_add_accelerator(item, "activate", accel, GDK_KEY_n, GDK_CONTROL_MASK | GDK_SHIFT_MASK, GTK_ACCEL_VISIBLE);
	lw_menu_sep(menu);
	lw_menu_item(menu, _("Import from _mRemoteNG…"), G_CALLBACK(lw_import_mremoteng));
	lw_menu_item(menu, _("Import from _Remmina"), G_CALLBACK(lw_import_remmina));
	lw_menu_sep(menu);
	lw_menu_item(menu, _("_Export settings…"), G_CALLBACK(lw_export_settings));
	lw_menu_item(menu, _("_Import settings…"), G_CALLBACK(lw_import_settings));
	lw_menu_sep(menu);
	item = lw_menu_item(menu, _("_Quit"), G_CALLBACK(lw_quit));
	gtk_widget_add_accelerator(item, "activate", accel, GDK_KEY_q, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE);
	gtk_menu_shell_append(GTK_MENU_SHELL(bar), top);

	/* View */
	top = gtk_menu_item_new_with_mnemonic(_("_View"));
	menu = gtk_menu_new();
	gtk_menu_item_set_submenu(GTK_MENU_ITEM(top), menu);
	sub = gtk_menu_item_new_with_mnemonic(_("_Theme"));
	submenu = gtk_menu_new();
	gtk_menu_item_set_submenu(GTK_MENU_ITEM(sub), submenu);
	for (i = 0; i < 3; i++) {
		item = gtk_radio_menu_item_new_with_label(group, _(labels[i]));
		group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(item));
		gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), remmina_pref.color_scheme == i);
		g_signal_connect(item, "toggled", G_CALLBACK(lw_theme_toggled), GINT_TO_POINTER(i));
		gtk_menu_shell_append(GTK_MENU_SHELL(submenu), item);
	}
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), sub);
	item = gtk_check_menu_item_new_with_mnemonic(_("_Minimize to tray on close"));
	gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), lw_close_to_tray());
	g_signal_connect(item, "toggled", G_CALLBACK(lw_toggle_close_to_tray), NULL);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
	item = gtk_check_menu_item_new_with_mnemonic(_("Open connections in _tabs"));
	gtk_widget_set_tooltip_text(item, _("On: inside this window, like mRemoteNG. Off: each in its own window, like Remmina."));
	gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), lw_pref_bool("lremote_session_tabs", TRUE));
	g_signal_connect(item, "toggled", G_CALLBACK(lw_toggle_session_tabs), NULL);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
	lw_menu_sep(menu);
	lw_menu_item(menu, _("_Expand all"), G_CALLBACK(lw_expand_all));
	lw_menu_item(menu, _("C_ollapse all"), G_CALLBACK(lw_collapse_all));
	gtk_menu_shell_append(GTK_MENU_SHELL(bar), top);

	/* Tools */
	top = gtk_menu_item_new_with_mnemonic(_("_Tools"));
	menu = gtk_menu_new();
	gtk_menu_item_set_submenu(GTK_MENU_ITEM(top), menu);
	item = lw_menu_item(menu, _("_Preferences"), G_CALLBACK(lw_preferences));
	gtk_widget_add_accelerator(item, "activate", accel, GDK_KEY_comma, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE);
	lw_menu_item(menu, _("P_lugins"), G_CALLBACK(lw_plugins));
	gtk_menu_shell_append(GTK_MENU_SHELL(bar), top);

	/* Help */
	top = gtk_menu_item_new_with_mnemonic(_("_Help"));
	menu = gtk_menu_new();
	gtk_menu_item_set_submenu(GTK_MENU_ITEM(top), menu);
	lw_menu_item(menu, _("Check for _updates"), G_CALLBACK(lw_check_updates));
	lw_menu_sep(menu);
	lw_menu_item(menu, _("_About lRemote"), G_CALLBACK(lw_about));
	gtk_menu_shell_append(GTK_MENU_SHELL(bar), top);
	return bar;
}

static GtkWidget *lw_build_quickbar(void)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6), *btn, *newbtn;

	gtk_container_set_border_width(GTK_CONTAINER(box), 6);
	newbtn = gtk_button_new_from_icon_name("list-add-symbolic", GTK_ICON_SIZE_BUTTON);
	gtk_widget_set_tooltip_text(newbtn, _("New connection"));
	g_signal_connect(newbtn, "clicked", G_CALLBACK(lw_new_connection), NULL);
	btn = gtk_button_new_from_icon_name("folder-new-symbolic", GTK_ICON_SIZE_BUTTON);
	gtk_widget_set_tooltip_text(btn, _("New folder"));
	g_signal_connect(btn, "clicked", G_CALLBACK(lw_new_folder), NULL);
	gtk_box_pack_start(GTK_BOX(box), newbtn, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), btn, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 4);

	gtk_box_pack_start(GTK_BOX(box), lw_label(_("Quick connect")), FALSE, FALSE, 0);
	lw->quick_proto = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
	remmina_plugin_manager_for_each_plugin(REMMINA_PLUGIN_TYPE_PROTOCOL, lw_add_protocol, lw->quick_proto);
	if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(lw->quick_proto), "RDP"))
		gtk_combo_box_set_active(GTK_COMBO_BOX(lw->quick_proto), 0);
	gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(lw->quick_proto), FALSE, FALSE, 0);
	lw->quick_server = GTK_ENTRY(gtk_entry_new());
	gtk_entry_set_placeholder_text(lw->quick_server, _("server or user@server"));
	gtk_entry_set_width_chars(lw->quick_server, 28);
	g_signal_connect(lw->quick_server, "activate", G_CALLBACK(lw_quick_connect), NULL);
	gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(lw->quick_server), FALSE, FALSE, 0);
	btn = gtk_button_new_with_mnemonic(_("_Connect"));
	g_signal_connect(btn, "clicked", G_CALLBACK(lw_quick_connect), NULL);
	gtk_box_pack_start(GTK_BOX(box), btn, FALSE, FALSE, 0);
	return box;
}

static GtkWidget *lw_build_tree(void)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4), *sw;
	GtkCellRenderer *r;
	GtkTreeViewColumn *col;

	lw->search = GTK_SEARCH_ENTRY(gtk_search_entry_new());
	gtk_entry_set_placeholder_text(GTK_ENTRY(lw->search), _("Search connections"));
	g_signal_connect(lw->search, "search-changed", G_CALLBACK(lw_on_search), NULL);
	gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(lw->search), FALSE, FALSE, 0);

	lw->store = gtk_tree_store_new(N_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
				       G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT);
	gtk_tree_sortable_set_default_sort_func(GTK_TREE_SORTABLE(lw->store), lw_sort, NULL, NULL);
	gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(lw->store), GTK_TREE_SORTABLE_DEFAULT_SORT_COLUMN_ID,
					     GTK_SORT_ASCENDING);
	lw->filter = gtk_tree_model_filter_new(GTK_TREE_MODEL(lw->store), NULL);
	gtk_tree_model_filter_set_visible_func(GTK_TREE_MODEL_FILTER(lw->filter), lw_visible, NULL, NULL);

	lw->tree = GTK_TREE_VIEW(gtk_tree_view_new_with_model(lw->filter));
	gtk_tree_view_set_headers_visible(lw->tree, FALSE);
	gtk_tree_view_set_enable_search(lw->tree, FALSE);
	gtk_tree_view_set_tooltip_column(lw->tree, COL_SERVER);
	col = gtk_tree_view_column_new();
	r = gtk_cell_renderer_pixbuf_new();
	gtk_tree_view_column_pack_start(col, r, FALSE);
	gtk_tree_view_column_add_attribute(col, r, "icon-name", COL_ICON);
	r = gtk_cell_renderer_text_new();
	gtk_tree_view_column_pack_start(col, r, TRUE);
	gtk_tree_view_column_add_attribute(col, r, "text", COL_NAME);
	gtk_tree_view_column_add_attribute(col, r, "weight", COL_WEIGHT);
	gtk_tree_view_append_column(lw->tree, col);
	g_signal_connect(gtk_tree_view_get_selection(lw->tree), "changed", G_CALLBACK(lw_on_selection), NULL);
	g_signal_connect(lw->tree, "row-activated", G_CALLBACK(lw_on_row_activated), NULL);
	g_signal_connect(lw->tree, "button-press-event", G_CALLBACK(lw_on_tree_button), NULL);
	g_signal_connect(lw->tree, "key-press-event", G_CALLBACK(lw_on_tree_key), NULL);

	sw = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
	gtk_container_add(GTK_CONTAINER(sw), GTK_WIDGET(lw->tree));
	gtk_box_pack_start(GTK_BOX(box), sw, TRUE, TRUE, 0);
	return box;
}

static GtkWidget *lw_build_props(void)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6), *sw, *grid, *btn, *notes_sw;
	gint row = 0;

	lw->props_title = GTK_LABEL(gtk_label_new(_("Config")));
	gtk_label_set_xalign(lw->props_title, 0);
	gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(lw->props_title)), "heading");
	gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(lw->props_title), FALSE, FALSE, 0);

	grid = gtk_grid_new();
	lw->props_grid = grid;
	gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 8);

	lw->p_name = GTK_ENTRY(gtk_entry_new());
	lw_grid_row(GTK_GRID(grid), row++, _("Name"), GTK_WIDGET(lw->p_name));
	lw->p_folder = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
	lw_grid_row(GTK_GRID(grid), row++, _("Folder"), GTK_WIDGET(lw->p_folder));
	lw->p_protocol = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
	remmina_plugin_manager_for_each_plugin(REMMINA_PLUGIN_TYPE_PROTOCOL, lw_add_protocol, lw->p_protocol);
	lw_grid_row(GTK_GRID(grid), row++, _("Protocol"), GTK_WIDGET(lw->p_protocol));
	lw->p_server = GTK_ENTRY(gtk_entry_new());
	gtk_entry_set_placeholder_text(lw->p_server, _("host or host:port"));
	lw_grid_row(GTK_GRID(grid), row++, _("Server"), GTK_WIDGET(lw->p_server));
	lw->p_username = GTK_ENTRY(gtk_entry_new());
	lw_grid_row(GTK_GRID(grid), row++, _("Username"), GTK_WIDGET(lw->p_username));
	lw->p_password = GTK_ENTRY(gtk_entry_new());
	gtk_entry_set_visibility(lw->p_password, FALSE);
	gtk_entry_set_input_purpose(lw->p_password, GTK_INPUT_PURPOSE_PASSWORD);
	lw_grid_row(GTK_GRID(grid), row++, _("Password"), GTK_WIDGET(lw->p_password));
	lw->p_domain = GTK_ENTRY(gtk_entry_new());
	lw_grid_row(GTK_GRID(grid), row++, _("Domain"), GTK_WIDGET(lw->p_domain));
	lw->p_resolution = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
	gtk_combo_box_text_append_text(lw->p_resolution, _("Fit to window"));
	gtk_combo_box_text_append_text(lw->p_resolution, _("Use client resolution"));
	gtk_combo_box_text_append_text(lw->p_resolution, _("Custom"));
	g_signal_connect(lw->p_resolution, "changed", G_CALLBACK(lw_on_res_changed), NULL);
	lw_grid_row(GTK_GRID(grid), row++, _("Resolution"), GTK_WIDGET(lw->p_resolution));
	lw->p_res_custom = GTK_ENTRY(gtk_entry_new());
	gtk_entry_set_placeholder_text(lw->p_res_custom, "1920x1080");
	lw_grid_row(GTK_GRID(grid), row++, "", GTK_WIDGET(lw->p_res_custom));
	lw->p_colordepth = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
	gtk_combo_box_text_append_text(lw->p_colordepth, _("Automatic"));
	gtk_combo_box_text_append_text(lw->p_colordepth, _("256 colours (8 bit)"));
	gtk_combo_box_text_append_text(lw->p_colordepth, _("High colour (15 bit)"));
	gtk_combo_box_text_append_text(lw->p_colordepth, _("High colour (16 bit)"));
	gtk_combo_box_text_append_text(lw->p_colordepth, _("True colour (24 bit)"));
	gtk_combo_box_text_append_text(lw->p_colordepth, _("True colour (32 bit)"));
	lw_grid_row(GTK_GRID(grid), row++, _("Colours"), GTK_WIDGET(lw->p_colordepth));
	lw->p_notes = GTK_TEXT_VIEW(gtk_text_view_new());
	gtk_text_view_set_wrap_mode(lw->p_notes, GTK_WRAP_WORD_CHAR);
	notes_sw = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(notes_sw), GTK_SHADOW_IN);
	gtk_widget_set_size_request(notes_sw, -1, 60);
	gtk_container_add(GTK_CONTAINER(notes_sw), GTK_WIDGET(lw->p_notes));
	lw_grid_row(GTK_GRID(grid), row++, _("Notes"), notes_sw);

	sw = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(sw), TRUE);
	gtk_container_add(GTK_CONTAINER(sw), grid);
	gtk_box_pack_start(GTK_BOX(box), sw, TRUE, TRUE, 0);

	lw->p_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	btn = gtk_button_new_with_mnemonic(_("_Save"));
	gtk_style_context_add_class(gtk_widget_get_style_context(btn), "suggested-action");
	g_signal_connect(btn, "clicked", G_CALLBACK(lw_props_save), NULL);
	gtk_box_pack_end(GTK_BOX(lw->p_buttons), btn, FALSE, FALSE, 0);
	btn = gtk_button_new_with_mnemonic(_("_All settings…"));
	gtk_widget_set_tooltip_text(btn, _("Open every setting of this protocol"));
	g_signal_connect(btn, "clicked", G_CALLBACK(lw_edit_advanced), NULL);
	gtk_box_pack_end(GTK_BOX(lw->p_buttons), btn, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), lw->p_buttons, FALSE, FALSE, 0);
	return box;
}

static GtkWidget *lw_build_start_page(void)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12), *img, *btns, *b;
	PangoAttrList *attrs;

	gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
	gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
	img = gtk_image_new_from_icon_name(REMMINA_APP_ID, GTK_ICON_SIZE_DIALOG);
	gtk_image_set_pixel_size(GTK_IMAGE(img), 96);
	gtk_box_pack_start(GTK_BOX(box), img, FALSE, FALSE, 0);

	lw->start_title = GTK_LABEL(gtk_label_new("lRemote"));
	attrs = pango_attr_list_new();
	pango_attr_list_insert(attrs, pango_attr_scale_new(1.8));
	pango_attr_list_insert(attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
	gtk_label_set_attributes(lw->start_title, attrs);
	pango_attr_list_unref(attrs);
	gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(lw->start_title), FALSE, FALSE, 0);

	lw->start_detail = GTK_LABEL(gtk_label_new(""));
	gtk_label_set_line_wrap(lw->start_detail, TRUE);
	gtk_label_set_justify(lw->start_detail, GTK_JUSTIFY_CENTER);
	gtk_label_set_max_width_chars(lw->start_detail, 60);
	gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(lw->start_detail)), "dim-label");
	gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(lw->start_detail), FALSE, FALSE, 0);

	lw->start_connect = gtk_button_new_with_mnemonic(_("_Connect"));
	gtk_style_context_add_class(gtk_widget_get_style_context(lw->start_connect), "suggested-action");
	gtk_widget_set_halign(lw->start_connect, GTK_ALIGN_CENTER);
	g_signal_connect(lw->start_connect, "clicked", G_CALLBACK(lw_connect_selected), NULL);
	gtk_box_pack_start(GTK_BOX(box), lw->start_connect, FALSE, FALSE, 0);

	btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_set_halign(btns, GTK_ALIGN_CENTER);
	b = gtk_button_new_with_mnemonic(_("New _connection"));
	g_signal_connect(b, "clicked", G_CALLBACK(lw_new_connection), NULL);
	gtk_box_pack_start(GTK_BOX(btns), b, FALSE, FALSE, 0);
	b = gtk_button_new_with_mnemonic(_("Import from _mRemoteNG…"));
	g_signal_connect(b, "clicked", G_CALLBACK(lw_import_mremoteng), NULL);
	gtk_box_pack_start(GTK_BOX(btns), b, FALSE, FALSE, 0);
	b = gtk_button_new_with_mnemonic(_("Import from _Remmina"));
	g_signal_connect(b, "clicked", G_CALLBACK(lw_import_remmina), NULL);
	gtk_box_pack_start(GTK_BOX(btns), b, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), btns, FALSE, FALSE, 12);
	return box;
}

/* Give the config panel the height of its fields and the tree the rest,
 * so neither scrolls while there is room (once, at the first layout) */
static void lw_fit_props(GtkWidget *paned, GdkRectangle *a, gpointer frame)
{
	gint min, nat, pos;

	g_signal_handlers_disconnect_by_func(paned, lw_fit_props, frame);
	gtk_widget_get_preferred_height(GTK_WIDGET(frame), &min, &nat);
	pos = a->height - nat - 6;
	gtk_paned_set_position(GTK_PANED(paned), MAX(pos, a->height / 2));
}

GtkWindow *lremote_window_get(void)
{
	return lw ? lw->window : NULL;
}

GtkWidget *lremote_window_present(void)
{
	GtkWidget *win, *vbox, *left, *frame;
	GtkAccelGroup *accel;

	if (lw) {
		gtk_window_present(lw->window);
		return GTK_WIDGET(lw->window);
	}
	lw = g_new0(LremoteWindow, 1);
	lw->folder_iters = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

	win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	lw->window = GTK_WINDOW(win);
	gtk_window_set_title(lw->window, "lRemote");
	gtk_window_set_icon_name(lw->window, REMMINA_APP_ID);
	gtk_window_set_default_size(lw->window, lw_pref_int("lremote_width", 1100), lw_pref_int("lremote_height", 720));
	accel = gtk_accel_group_new();
	gtk_window_add_accel_group(lw->window, accel);
	g_signal_connect(win, "delete-event", G_CALLBACK(lw_on_delete), NULL);
	g_signal_connect(win, "key-press-event", G_CALLBACK(lw_on_key), NULL);
	g_signal_connect(win, "destroy", G_CALLBACK(lw_on_destroy), NULL);

	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_container_add(GTK_CONTAINER(win), vbox);
	gtk_box_pack_start(GTK_BOX(vbox), lw_build_menubar(accel), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(vbox), lw_build_quickbar(), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);

	lw->hpaned = GTK_PANED(gtk_paned_new(GTK_ORIENTATION_HORIZONTAL));
	gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(lw->hpaned), TRUE, TRUE, 0);

	lw->vpaned = GTK_PANED(gtk_paned_new(GTK_ORIENTATION_VERTICAL));
	left = lw_build_tree();
	gtk_container_set_border_width(GTK_CONTAINER(left), 6);
	gtk_paned_pack1(lw->vpaned, left, TRUE, FALSE);
	frame = lw_build_props();
	gtk_container_set_border_width(GTK_CONTAINER(frame), 6);
	gtk_paned_pack2(lw->vpaned, frame, FALSE, FALSE);
	gtk_paned_pack1(lw->hpaned, GTK_WIDGET(lw->vpaned), FALSE, FALSE);
	lw->right = GTK_STACK(gtk_stack_new());
	gtk_stack_add_named(lw->right, lw_build_start_page(), "start");
	lw->sessions = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_stack_add_named(lw->right, lw->sessions, "sessions");
	gtk_paned_pack2(lw->hpaned, GTK_WIDGET(lw->right), TRUE, FALSE);
	gtk_paned_set_position(lw->hpaned, lw_pref_int("lremote_hpaned", 340));
	g_signal_connect(lw->vpaned, "size-allocate", G_CALLBACK(lw_fit_props), frame);

	lw->status = GTK_LABEL(gtk_label_new(""));
	gtk_label_set_xalign(lw->status, 0);
	gtk_widget_set_margin_start(GTK_WIDGET(lw->status), 8);
	gtk_widget_set_margin_top(GTK_WIDGET(lw->status), 3);
	gtk_widget_set_margin_bottom(GTK_WIDGET(lw->status), 3);
	gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(lw->status)), "dim-label");
	gtk_box_pack_start(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(lw->status), FALSE, FALSE, 0);

	if (g_application_get_default() && GTK_IS_APPLICATION(g_application_get_default()))
		gtk_window_set_application(lw->window, GTK_APPLICATION(g_application_get_default()));
	remmina_widget_pool_register(win);

	gtk_widget_show_all(win);
	lw_reload();
	lw_props_load(NULL);
	lw_on_res_changed(GTK_COMBO_BOX(lw->p_resolution), NULL);
	return win;
}
