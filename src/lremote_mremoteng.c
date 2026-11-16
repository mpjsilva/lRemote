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
 * Importer for mRemoteNG connection files (confCons.xml, ConfVersion 1.x to 2.9).
 *
 * The file is a tree of <Node> elements. Containers become Remmina groups
 * ("Folder/Subfolder"), connections become .remmina profiles. Every
 * attribute can be marked Inherit<Name>="True", in which case the value of
 * the closest ancestor is used, exactly as mRemoteNG does.
 *
 * Passwords are stored encrypted with the master password ("mR3m" unless
 * the user changed it). ConfVersion >= 2.6 uses AES-GCM, older files use
 * AES-CBC with an MD5 key. Files saved with FullFileEncryption carry one
 * encrypted blob holding the whole node tree.
 */

#include "config.h"
#include <string.h>
#include <stdlib.h>
#include <glib.h>
#include <glib/gi18n.h>

#include "lremote_crypto.h"
#include "lremote_mremoteng.h"
#include "remmina_file.h"
#include "remmina_file_manager.h"
#include "remmina_log.h"
#include "remmina_plugin_manager.h"
#include "remmina/remmina_trace_calls.h"

#define MRNG_DEFAULT_PASSWORD "mR3m"

typedef struct {
	GHashTable *attrs;	/* attribute name -> value, owned */
	gchar *path;		/* group path of this container ("" for root) */
	gboolean container;
} MrngNode;

typedef struct {
	GQueue *stack;		/* MrngNode*, top = current parent */
	const gchar *password;
	gboolean legacy;
	gint iterations;
	gboolean full_file;
	GString *full_file_text;
	LremoteMrngStats *stats;
	GError *error;
} MrngParser;

static GQuark lremote_mrng_quark(void)
{
	return g_quark_from_static_string("lremote-mremoteng");
}

void lremote_mremoteng_stats_clear(LremoteMrngStats *stats)
{
	if (stats->unsupported_names)
		g_string_free(stats->unsupported_names, TRUE);
	memset(stats, 0, sizeof(*stats));
}

static MrngNode *mrng_node_new(const gchar **names, const gchar **values)
{
	MrngNode *n = g_new0(MrngNode, 1);
	gint i;

	n->attrs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	for (i = 0; names && names[i]; i++)
		g_hash_table_insert(n->attrs, g_strdup(names[i]), g_strdup(values[i]));
	return n;
}

static void mrng_node_free(MrngNode *n)
{
	if (!n)
		return;
	g_hash_table_destroy(n->attrs);
	g_free(n->path);
	g_free(n);
}

static const gchar *mrng_raw(MrngNode *n, const gchar *name)
{
	return n ? g_hash_table_lookup(n->attrs, name) : NULL;
}

static gboolean mrng_true(const gchar *v)
{
	return v && g_ascii_strcasecmp(v, "True") == 0;
}

/* Effective value honouring the inheritance flag up the ancestor chain.
 * inherit_name is the property name used in the Inherit<...> attribute,
 * which differs from the value attribute for a few settings. */
static const gchar *mrng_get_full(MrngParser *p, MrngNode *node, const gchar *name, const gchar *inherit_name)
{
	GList *l;
	gchar *inherit_key;
	const gchar *v;
	MrngNode *cur = node;

	inherit_key = g_strconcat("Inherit", inherit_name, NULL);
	l = p->stack->head;	/* head = innermost parent */
	while (cur) {
		if (!mrng_true(mrng_raw(cur, inherit_key)) || !l) {
			v = mrng_raw(cur, name);
			g_free(inherit_key);
			return v;
		}
		cur = l->data;
		l = l->next;
	}
	g_free(inherit_key);
	return NULL;
}

static const gchar *mrng_get(MrngParser *p, MrngNode *node, const gchar *name)
{
	return mrng_get_full(p, node, name, name);
}

static gchar *mrng_password(MrngParser *p, const gchar *cipher)
{
	gchar *plain;

	if (!cipher || !*cipher)
		return NULL;
	if (p->legacy)
		plain = lremote_mrng_decrypt_legacy(cipher, p->password, NULL);
	else
		plain = lremote_mrng_decrypt_gcm(cipher, p->password, p->iterations, NULL);
	if (plain && *plain) {
		p->stats->passwords = TRUE;
		return plain;
	}
	g_free(plain);
	return NULL;
}

static void mrng_set(RemminaFile *f, const gchar *key, const gchar *value)
{
	if (value && *value)
		remmina_file_set_string(f, key, value);
}

static void mrng_import_connection(MrngParser *p, MrngNode *node, MrngNode *parent)
{
	TRACE_CALL(__func__);
	const gchar *proto, *host, *port, *name, *v;
	gchar *server, *plain;
	const gchar *protocol = NULL;
	RemminaFile *f;
	gint iport = 0;

	name = mrng_raw(node, "Name");
	proto = mrng_get(p, node, "Protocol");
	host = mrng_get(p, node, "Hostname");
	port = mrng_get(p, node, "Port");
	if (port)
		iport = atoi(port);
	if (!proto)
		proto = "RDP";

	if (g_ascii_strcasecmp(proto, "RDP") == 0)
		protocol = "RDP";
	else if (g_ascii_strcasecmp(proto, "VNC") == 0)
		protocol = "VNC";
	else if (g_ascii_strcasecmp(proto, "SSH2") == 0 || g_ascii_strcasecmp(proto, "SSH1") == 0)
		protocol = "SSH";
	else if (g_ascii_strcasecmp(proto, "HTTP") == 0 || g_ascii_strcasecmp(proto, "HTTPS") == 0)
		protocol = "WWW";
	else if (g_ascii_strcasecmp(proto, "ARD") == 0)
		protocol = "VNC";

	if (!protocol || !remmina_plugin_manager_get_plugin(REMMINA_PLUGIN_TYPE_PROTOCOL, protocol)) {
		p->stats->unsupported++;
		if (!p->stats->unsupported_names)
			p->stats->unsupported_names = g_string_new("");
		g_string_append_printf(p->stats->unsupported_names, "%s%s (%s)",
				       p->stats->unsupported_names->len ? ", " : "", name ? name : "?", proto);
		return;
	}

	f = remmina_file_new();
	mrng_set(f, "name", name);
	remmina_file_set_string(f, "protocol", protocol);
	if (parent && parent->path && *parent->path)
		remmina_file_set_string(f, "group", parent->path);
	mrng_set(f, "notes_text", mrng_get(p, node, "Descr"));

	if (g_strcmp0(protocol, "WWW") == 0) {
		server = g_strdup_printf("%s://%s%s%s", g_ascii_strcasecmp(proto, "HTTPS") == 0 ? "https" : "http",
					 host ? host : "", (iport && iport != 80 && iport != 443) ? ":" : "",
					 (iport && iport != 80 && iport != 443) ? port : "");
	} else if (g_strcmp0(protocol, "RDP") == 0) {
		server = (iport && iport != 3389) ? g_strdup_printf("%s:%d", host ? host : "", iport) : g_strdup(host ? host : "");
	} else if (g_strcmp0(protocol, "VNC") == 0) {
		server = (iport && iport != 5900) ? g_strdup_printf("%s:%d", host ? host : "", iport) : g_strdup(host ? host : "");
	} else {	/* SSH */
		server = (iport && iport != 22) ? g_strdup_printf("%s:%d", host ? host : "", iport) : g_strdup(host ? host : "");
	}
	remmina_file_set_string(f, "server", server);
	g_free(server);

	mrng_set(f, "username", mrng_get(p, node, "Username"));
	plain = mrng_password(p, mrng_get(p, node, "Password"));
	mrng_set(f, "password", plain);
	g_free(plain);

	if (g_strcmp0(protocol, "RDP") == 0) {
		mrng_set(f, "domain", mrng_get(p, node, "Domain"));

		v = mrng_get(p, node, "Resolution");
		if (v && g_str_has_prefix(v, "Res")) {	/* e.g. Res1024x768 */
			gint w = 0, h = 0;
			if (sscanf(v, "Res%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
				remmina_file_set_int(f, "resolution_mode", RES_USE_CUSTOM);
				remmina_file_set_int(f, "resolution_width", w);
				remmina_file_set_int(f, "resolution_height", h);
			}
		} else if (v && g_ascii_strcasecmp(v, "Fullscreen") == 0) {
			remmina_file_set_int(f, "resolution_mode", RES_USE_CLIENT);
			remmina_file_set_int(f, "viewmode", 4);	/* fullscreen */
		} else {	/* SmartSize / FitToWindow */
			remmina_file_set_int(f, "resolution_mode", RES_USE_INITIAL_WINDOW_SIZE);
			remmina_file_set_int(f, "dynamic_resolution", 1);
		}

		v = mrng_get(p, node, "Colors");
		if (v && g_str_has_prefix(v, "Colors")) {
			gint depth = atoi(v + 6);
			if (depth == 8 || depth == 15 || depth == 16 || depth == 24 || depth == 32)
				remmina_file_set_int(f, "colordepth", depth);
		}

		v = mrng_get(p, node, "RedirectSound");
		if (v && g_ascii_strcasecmp(v, "BringToThisComputer") == 0)
			remmina_file_set_string(f, "sound", "local");
		else if (v && g_ascii_strcasecmp(v, "LeaveAtRemoteComputer") == 0)
			remmina_file_set_string(f, "sound", "remote");
		else
			remmina_file_set_string(f, "sound", "off");

		if (mrng_true(mrng_get_full(p, node, "ConnectToConsole", "UseConsoleSession")))
			remmina_file_set_int(f, "console", 1);
		if (mrng_true(mrng_get(p, node, "RedirectPrinters")))
			remmina_file_set_int(f, "shareprinter", 1);
		if (mrng_true(mrng_get(p, node, "RedirectSmartCards")))
			remmina_file_set_int(f, "sharesmartcard", 1);
		v = mrng_get(p, node, "RedirectDiskDrives");
		if (mrng_true(v) || (v && g_ascii_strcasecmp(v, "Local") == 0))
			remmina_file_set_string(f, "sharefolder", g_get_home_dir());
		v = mrng_get(p, node, "RDPAuthenticationLevel");
		if (v && g_ascii_strcasecmp(v, "NoAuth") == 0)
			remmina_file_set_int(f, "cert_ignore", 1);

		v = mrng_get(p, node, "RDGatewayUsageMethod");
		if (v && g_ascii_strcasecmp(v, "Never") != 0) {
			const gchar *gw = mrng_get(p, node, "RDGatewayHostname");
			if (gw && *gw) {
				remmina_file_set_string(f, "gateway_server", gw);
				if (g_ascii_strcasecmp(v, "Detect") == 0)
					remmina_file_set_int(f, "gateway_usage", 1);
				v = mrng_get(p, node, "RDGatewayUseConnectionCredentials");
				if (v && g_ascii_strcasecmp(v, "Yes") == 0) {
					mrng_set(f, "gateway_username", mrng_get(p, node, "Username"));
					mrng_set(f, "gateway_domain", mrng_get(p, node, "Domain"));
					mrng_set(f, "gateway_password", remmina_file_get_string(f, "password"));
				} else {
					mrng_set(f, "gateway_username", mrng_get(p, node, "RDGatewayUsername"));
					mrng_set(f, "gateway_domain", mrng_get(p, node, "RDGatewayDomain"));
					plain = mrng_password(p, mrng_get(p, node, "RDGatewayPassword"));
					mrng_set(f, "gateway_password", plain);
					g_free(plain);
				}
			}
		}
	} else if (g_strcmp0(protocol, "VNC") == 0) {
		if (mrng_true(mrng_get(p, node, "VNCViewOnly")))
			remmina_file_set_int(f, "viewonly", 1);
		v = mrng_get(p, node, "VNCColors");
		if (v && g_ascii_strcasecmp(v, "Col8Bit") == 0)
			remmina_file_set_int(f, "colordepth", 8);
	}

	remmina_file_generate_filename(f);
	/* save() requires the state file, which generate_filename() leaves unset */
	remmina_file_set_statefile(f);
	remmina_file_save(f);
	remmina_file_free(f);
	p->stats->imported++;
}

/* mRemoteNG 1.77+ writes the root as <mrng:Connections>; ignore any namespace prefix */
static const gchar *mrng_local(const gchar *element)
{
	const gchar *c = strchr(element, ':');
	return c ? c + 1 : element;
}

static gchar *mrng_find_root(const gchar *xml, gssize len)
{
	gchar *r = g_strstr_len(xml, len, "<Connections");
	return r ? r : g_strstr_len(xml, len, "<mrng:Connections");
}

static void mrng_start_element(GMarkupParseContext *ctx, const gchar *element, const gchar **names,
			       const gchar **values, gpointer user_data, GError **error)
{
	MrngParser *p = user_data;
	MrngNode *node, *parent;
	const gchar *type;

	if (g_strcmp0(mrng_local(element), "Connections") == 0) {
		gint i;
		node = mrng_node_new(names, values);
		node->container = TRUE;
		node->path = g_strdup("");
		for (i = 0; names[i]; i++) {
			if (g_strcmp0(names[i], "KdfIterations") == 0)
				p->iterations = atoi(values[i]);
			else if (g_strcmp0(names[i], "FullFileEncryption") == 0)
				p->full_file = mrng_true(values[i]);
			else if (g_strcmp0(names[i], "ConfVersion") == 0)
				p->legacy = g_ascii_strtod(values[i], NULL) < 2.6;
		}
		g_queue_push_head(p->stack, node);
		return;
	}
	if (g_strcmp0(mrng_local(element), "Node") != 0)
		return;

	parent = g_queue_peek_head(p->stack);
	node = mrng_node_new(names, values);
	type = mrng_raw(node, "Type");
	node->container = (type && g_ascii_strcasecmp(type, "Container") == 0);
	if (node->container) {
		const gchar *name = mrng_raw(node, "Name");
		node->path = (parent && parent->path && *parent->path)
			     ? g_strdup_printf("%s/%s", parent->path, name ? name : "")
			     : g_strdup(name ? name : "");
		p->stats->folders++;
	}
	if (!node->container && !p->error)
		mrng_import_connection(p, node, parent);
	g_queue_push_head(p->stack, node);
}

static void mrng_end_element(GMarkupParseContext *ctx, const gchar *element, gpointer user_data, GError **error)
{
	MrngParser *p = user_data;

	if (g_strcmp0(mrng_local(element), "Node") == 0 || g_strcmp0(mrng_local(element), "Connections") == 0)
		mrng_node_free(g_queue_pop_head(p->stack));
}

static void mrng_text(GMarkupParseContext *ctx, const gchar *text, gsize len, gpointer user_data, GError **error)
{
	MrngParser *p = user_data;

	/* With full file encryption the root element's text is the encrypted node tree */
	if (p->full_file && g_queue_get_length(p->stack) == 1)
		g_string_append_len(p->full_file_text, text, len);
}

static gboolean mrng_check_password(MrngParser *p, const gchar *protected_b64, GError **error)
{
	gchar *marker;

	if (!protected_b64 || !*protected_b64)
		return TRUE;	/* very old files have no marker */
	if (p->legacy)
		marker = lremote_mrng_decrypt_legacy(protected_b64, p->password, NULL);
	else
		marker = lremote_mrng_decrypt_gcm(protected_b64, p->password, p->iterations, NULL);
	if (!marker || (g_strcmp0(marker, "ThisIsProtected") != 0 && g_strcmp0(marker, "ThisIsNotProtected") != 0)) {
		g_free(marker);
		g_set_error_literal(error, lremote_mrng_quark(), 3, _("Wrong master password."));
		return FALSE;
	}
	p->stats->protected_file = g_strcmp0(marker, "ThisIsProtected") == 0;
	g_free(marker);
	return TRUE;
}

/* Reads only the root element attributes to decide the format and verify the password */
static gboolean mrng_probe_root(MrngParser *p, const gchar *xml, gsize len, GError **error)
{
	gchar *start, *end, *protected_b64 = NULL, *engine = NULL, *mode = NULL, *keyprot = NULL;
	gchar **parts;
	gint i;

	start = mrng_find_root(xml, len);
	if (!start) {
		g_set_error_literal(error, lremote_mrng_quark(), 4, _("This is not an mRemoteNG connections file."));
		return FALSE;
	}
	end = strchr(start, '>');
	if (!end)
		return FALSE;
	{
		gchar *head = g_strndup(start, end - start);
		parts = g_strsplit(head, "\"", -1);
		g_free(head);
	}
	/* parts alternate: ' attr=' , 'value', ' attr=', 'value' ... */
	for (i = 0; parts[i] && parts[i + 1]; i += 2) {
		gchar *key = g_strstrip(parts[i]);
		gsize klen = strlen(key);
		if (klen && key[klen - 1] == '=')
			key[klen - 1] = '\0';
		key = strrchr(key, ' ') ? strrchr(key, ' ') + 1 : key;
		if (g_strcmp0(key, "KdfIterations") == 0)
			p->iterations = atoi(parts[i + 1]);
		else if (g_strcmp0(key, "FullFileEncryption") == 0)
			p->full_file = mrng_true(parts[i + 1]);
		else if (g_strcmp0(key, "ConfVersion") == 0)
			p->legacy = g_ascii_strtod(parts[i + 1], NULL) < 2.6;
		else if (g_strcmp0(key, "Protected") == 0)
			protected_b64 = g_strdup(parts[i + 1]);
		else if (g_strcmp0(key, "EncryptionEngine") == 0)
			engine = g_strdup(parts[i + 1]);
		else if (g_strcmp0(key, "BlockCipherMode") == 0)
			mode = g_strdup(parts[i + 1]);
		else if (g_strcmp0(key, "KeyProtection") == 0)
			keyprot = g_strdup(parts[i + 1]);
	}
	g_strfreev(parts);
	if (keyprot && g_ascii_strcasecmp(keyprot, "Dpapi") == 0) {
		g_set_error_literal(error, lremote_mrng_quark(), 5,
				    _("This file is protected with Windows DPAPI and can only be opened on the Windows account that saved it. In mRemoteNG, set a master password and save the file again."));
		g_free(protected_b64); g_free(engine); g_free(mode); g_free(keyprot);
		return FALSE;
	}
	if (!p->legacy && ((engine && g_ascii_strcasecmp(engine, "AES") != 0) || (mode && g_ascii_strcasecmp(mode, "GCM") != 0))) {
		g_set_error(error, lremote_mrng_quark(), 2,
			    _("Unsupported encryption %s/%s. In mRemoteNG, choose AES with GCM and save the file again."),
			    engine ? engine : "?", mode ? mode : "?");
		g_free(protected_b64); g_free(engine); g_free(mode); g_free(keyprot);
		return FALSE;
	}
	g_free(engine); g_free(mode); g_free(keyprot);
	{
		gboolean ok = mrng_check_password(p, protected_b64, error);
		g_free(protected_b64);
		return ok;
	}
}

gboolean lremote_mremoteng_import(const gchar *path, const gchar *password, LremoteMrngStats *stats, GError **error)
{
	TRACE_CALL(__func__);
	GMarkupParser parser = { mrng_start_element, mrng_end_element, mrng_text, NULL, NULL };
	GMarkupParseContext *ctx;
	MrngParser p;
	gchar *xml = NULL;
	gsize len = 0;
	gboolean ok;

	lremote_mremoteng_stats_clear(stats);
	{
		/* Make sure profiles land in the user's data dir, not a system one */
		gchar *userdir = g_build_filename(g_get_user_data_dir(), LREMOTE_DIRNAME, NULL);
		g_mkdir_with_parents(userdir, 0700);
		g_free(userdir);
	}
	if (!g_file_get_contents(path, &xml, &len, error))
		return FALSE;
	/* .NET writes a UTF-8 BOM that GMarkup rejects */
	if (len >= 3 && (guchar)xml[0] == 0xEF && (guchar)xml[1] == 0xBB && (guchar)xml[2] == 0xBF) {
		memmove(xml, xml + 3, len - 3);
		len -= 3;
		xml[len] = '\0';
	}

	memset(&p, 0, sizeof(p));
	p.stack = g_queue_new();
	p.password = (password && *password) ? password : MRNG_DEFAULT_PASSWORD;
	p.iterations = 1000;
	p.stats = stats;
	p.full_file_text = g_string_new("");

	/* Before 2.6, full file encryption wrote the whole document as one base64 blob */
	if (!mrng_find_root(xml, len)) {
		gchar *plain = lremote_mrng_decrypt_legacy(g_strstrip(xml), p.password, NULL);
		if (plain && mrng_find_root(plain, -1)) {
			g_free(xml);
			xml = plain;
			len = strlen(xml);
		} else {
			g_free(plain);
			g_free(xml);
			g_queue_free(p.stack);
			g_string_free(p.full_file_text, TRUE);
			g_set_error_literal(error, lremote_mrng_quark(), 3,
					    _("Wrong master password, or this is not an mRemoteNG connections file."));
			return FALSE;
		}
	}

	if (!mrng_probe_root(&p, xml, len, error)) {
		g_free(xml);
		g_queue_free(p.stack);
		g_string_free(p.full_file_text, TRUE);
		return FALSE;
	}

	ctx = g_markup_parse_context_new(&parser, 0, &p, NULL);
	ok = g_markup_parse_context_parse(ctx, xml, len, error) && g_markup_parse_context_end_parse(ctx, error);
	g_markup_parse_context_free(ctx);
	g_free(xml);

	if (ok && p.full_file && p.full_file_text->len > 0) {
		/* Decrypt the inner tree and parse it inside a synthetic root */
		gchar *inner, *wrapped;
		g_strstrip(p.full_file_text->str);
		if (p.legacy)
			inner = lremote_mrng_decrypt_legacy(p.full_file_text->str, p.password, error);
		else
			inner = lremote_mrng_decrypt_gcm(p.full_file_text->str, p.password, p.iterations, error);
		if (inner) {
			wrapped = g_strconcat("<Connections>", inner, "</Connections>", NULL);
			p.full_file = FALSE;
			ctx = g_markup_parse_context_new(&parser, 0, &p, NULL);
			ok = g_markup_parse_context_parse(ctx, wrapped, strlen(wrapped), error)
			     && g_markup_parse_context_end_parse(ctx, error);
			g_markup_parse_context_free(ctx);
			g_free(wrapped);
			g_free(inner);
		} else {
			ok = FALSE;
		}
	}
	if (ok && p.error) {
		g_propagate_error(error, p.error);
		p.error = NULL;
		ok = FALSE;
	}

	g_queue_free_full(p.stack, (GDestroyNotify)mrng_node_free);
	g_string_free(p.full_file_text, TRUE);
	REMMINA_DEBUG("mRemoteNG import: %d profiles, %d folders, %d unsupported", stats->imported, stats->folders, stats->unsupported);
	return ok;
}
