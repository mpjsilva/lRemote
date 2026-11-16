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

#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define REMMINA_BACKUP_EXT ".remmina-backup"

typedef enum {
	REMMINA_BACKUP_MERGE   = 0,	/* add new profiles, keep existing ones untouched */
	REMMINA_BACKUP_REPLACE = 1,	/* overwrite profiles with the same file name */
} RemminaBackupMode;

typedef struct {
	gint	connections;	/* profiles written (import) or stored (export) */
	gint	skipped;	/* profiles kept because they already existed (merge) */
	gboolean preferences;	/* preferences were part of the bundle */
} RemminaBackupStats;

/* Writes preferences and every connection profile, without passwords, to path */
gboolean remmina_backup_export(const gchar *path, RemminaBackupStats *stats, GError **error);
/* Reads a bundle created by remmina_backup_export. Returns FALSE on error */
gboolean remmina_backup_import(const gchar *path, RemminaBackupMode mode, RemminaBackupStats *stats, GError **error);
/* TRUE when the setting must never leave the machine */
gboolean remmina_backup_is_sensitive_key(const gchar *key);

G_END_DECLS
