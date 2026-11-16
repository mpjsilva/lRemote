/*
 * Remmina - The GTK+ Remote Desktop Client
 * Copyright (C) 2025 Remmina contributors
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
 * Export / import of the whole configuration (preferences plus every
 * connection profile) as one text bundle in GKeyFile format.
 *
 * Secrets are never written: passwords, passphrases, the preference
 * encryption key and the unlock password are stripped on export and
 * ignored on import, so the bundle can be copied to another machine or
 * kept in plain backups without exposing credentials.
 */

#include "config.h"
#include <string.h>
#include <glib.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>

#include "remmina_backup.h"
#include "remmina_file_manager.h"
#include "remmina_log.h"
#include "remmina_pref.h"
#include "remmina/remmina_trace_calls.h"

#define BUNDLE_GROUP      "remmina_backup"
#define PREF_GROUP        "remmina_pref"
#define CONN_PREFIX       "connection:"
#define BUNDLE_VERSION    1

static const gchar *sensitive_fragments[] = {
	"password", "passphrase", "secret", "pin", "token", NULL
};

gboolean remmina_backup_is_sensitive_key(const gchar *key)
{
	gchar *lower;
	gint i;
	gboolean sensitive = FALSE;

	if (!key)
		return FALSE;
	lower = g_ascii_strdown(key, -1);
	for (i = 0; sensitive_fragments[i]; i++) {
		if (strstr(lower, sensitive_fragments[i])) {
			sensitive = TRUE;
			break;
		}
	}
	g_free(lower);
	return sensitive;
}

/* Copies every non-sensitive key of src_group into dst_group */
static gint remmina_backup_copy_group(GKeyFile *src, const gchar *src_group, GKeyFile *dst, const gchar *dst_group)
{
	gchar **keys;
	gsize n, i;
	gint copied = 0;

	keys = g_key_file_get_keys(src, src_group, &n, NULL);
	if (!keys)
		return 0;
	for (i = 0; i < n; i++) {
		gchar *value;
		if (remmina_backup_is_sensitive_key(keys[i]))
			continue;
		value = g_key_file_get_value(src, src_group, keys[i], NULL);
		if (value) {
			g_key_file_set_value(dst, dst_group, keys[i], value);
			g_free(value);
			copied++;
		}
	}
	g_strfreev(keys);
	return copied;
}

gboolean remmina_backup_export(const gchar *path, RemminaBackupStats *stats, GError **error)
{
	TRACE_CALL(__func__);
	GKeyFile *bundle, *src;
	GDir *dir;
	const gchar *name;
	gchar *datadir, *filename, *content, *created;
	GDateTime *now;
	gsize length;
	gboolean ok;
	RemminaBackupStats local = { 0 };

	if (!stats)
		stats = &local;
	memset(stats, 0, sizeof(*stats));

	bundle = g_key_file_new();
	now = g_date_time_new_now_local();
	created = g_date_time_format_iso8601(now);
	g_date_time_unref(now);
	g_key_file_set_integer(bundle, BUNDLE_GROUP, "version", BUNDLE_VERSION);
	g_key_file_set_string(bundle, BUNDLE_GROUP, "application", "Remmina " VERSION);
	g_key_file_set_string(bundle, BUNDLE_GROUP, "created", created);
	g_key_file_set_boolean(bundle, BUNDLE_GROUP, "passwords_included", FALSE);
	g_free(created);

	/* Preferences */
	src = g_key_file_new();
	if (g_key_file_load_from_file(src, remmina_pref_file, G_KEY_FILE_NONE, NULL)) {
		if (remmina_backup_copy_group(src, PREF_GROUP, bundle, PREF_GROUP) > 0)
			stats->preferences = TRUE;
	}
	g_key_file_free(src);

	/* Connection profiles */
	datadir = remmina_file_get_datadir();
	dir = g_dir_open(datadir, 0, NULL);
	if (dir) {
		while ((name = g_dir_read_name(dir)) != NULL) {
			gchar *group;
			if (!g_str_has_suffix(name, ".remmina"))
				continue;
			filename = g_build_filename(datadir, name, NULL);
			src = g_key_file_new();
			if (g_key_file_load_from_file(src, filename, G_KEY_FILE_NONE, NULL)
			    && g_key_file_has_group(src, "remmina")) {
				group = g_strconcat(CONN_PREFIX, name, NULL);
				remmina_backup_copy_group(src, "remmina", bundle, group);
				g_free(group);
				stats->connections++;
			}
			g_key_file_free(src);
			g_free(filename);
		}
		g_dir_close(dir);
	}
	g_free(datadir);

	content = g_key_file_to_data(bundle, &length, error);
	ok = content && g_file_set_contents(path, content, length, error);
	g_free(content);
	g_key_file_free(bundle);
	REMMINA_DEBUG("Exported %d profiles to %s", stats->connections, path);
	return ok;
}

static gboolean remmina_backup_valid_basename(const gchar *name)
{
	/* The group name is the profile file name: refuse anything that could escape the data dir */
	return name && *name && !strchr(name, '/') && !strchr(name, '\\') && !g_str_has_prefix(name, ".")
	       && g_str_has_suffix(name, ".remmina");
}

gboolean remmina_backup_import(const gchar *path, RemminaBackupMode mode, RemminaBackupStats *stats, GError **error)
{
	TRACE_CALL(__func__);
	GKeyFile *bundle, *dst;
	gchar **groups;
	gsize n, i;
	gchar *datadir, *content;
	gsize length;
	gint version;
	RemminaBackupStats local = { 0 };

	if (!stats)
		stats = &local;
	memset(stats, 0, sizeof(*stats));

	bundle = g_key_file_new();
	if (!g_key_file_load_from_file(bundle, path, G_KEY_FILE_NONE, error)) {
		g_key_file_free(bundle);
		return FALSE;
	}
	if (!g_key_file_has_group(bundle, BUNDLE_GROUP)) {
		g_set_error(error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_GROUP_NOT_FOUND,
			    _("This is not a Remmina settings backup."));
		g_key_file_free(bundle);
		return FALSE;
	}
	version = g_key_file_get_integer(bundle, BUNDLE_GROUP, "version", NULL);
	if (version > BUNDLE_VERSION) {
		g_set_error(error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_PARSE,
			    _("The backup was made by a newer Remmina and cannot be read."));
		g_key_file_free(bundle);
		return FALSE;
	}

	datadir = remmina_file_get_datadir();
	g_mkdir_with_parents(datadir, 0700);

	groups = g_key_file_get_groups(bundle, &n);
	for (i = 0; i < n; i++) {
		const gchar *name;
		gchar *filename;

		if (!g_str_has_prefix(groups[i], CONN_PREFIX))
			continue;
		name = groups[i] + strlen(CONN_PREFIX);
		if (!remmina_backup_valid_basename(name)) {
			REMMINA_DEBUG("Skipping profile with unsafe name %s", name);
			continue;
		}
		filename = g_build_filename(datadir, name, NULL);
		if (mode == REMMINA_BACKUP_MERGE && g_file_test(filename, G_FILE_TEST_EXISTS)) {
			stats->skipped++;
			g_free(filename);
			continue;
		}
		dst = g_key_file_new();
		/* Keep the existing file's secrets when replacing a profile on the same machine */
		g_key_file_load_from_file(dst, filename, G_KEY_FILE_NONE, NULL);
		remmina_backup_copy_group(bundle, groups[i], dst, "remmina");
		content = g_key_file_to_data(dst, &length, NULL);
		if (content && g_file_set_contents(filename, content, length, NULL)) {
			g_chmod(filename, 0600);
			stats->connections++;
		}
		g_free(content);
		g_key_file_free(dst);
		g_free(filename);
	}
	g_strfreev(groups);
	g_free(datadir);

	/* Preferences: merge into the current file so local secrets survive */
	if (g_key_file_has_group(bundle, PREF_GROUP)) {
		dst = g_key_file_new();
		g_key_file_load_from_file(dst, remmina_pref_file, G_KEY_FILE_NONE, NULL);
		if (remmina_backup_copy_group(bundle, PREF_GROUP, dst, PREF_GROUP) > 0) {
			content = g_key_file_to_data(dst, &length, NULL);
			if (content && g_file_set_contents(remmina_pref_file, content, length, NULL))
				stats->preferences = TRUE;
			g_free(content);
		}
		g_key_file_free(dst);
	}

	g_key_file_free(bundle);
	REMMINA_DEBUG("Imported %d profiles (%d skipped) from %s", stats->connections, stats->skipped, path);
	return TRUE;
}
