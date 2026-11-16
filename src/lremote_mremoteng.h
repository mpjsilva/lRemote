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

#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
	gint	 imported;	/* profiles written */
	gint	 folders;	/* containers seen (become groups) */
	gint	 unsupported;	/* connections whose protocol has no plugin here */
	GString *unsupported_names;
	gboolean protected_file;	/* file used a custom master password */
	gboolean passwords;	/* passwords were decrypted and stored */
} LremoteMrngStats;

/* Imports an mRemoteNG confCons.xml. password may be NULL to use the
 * mRemoteNG default. Returns FALSE and sets error on a wrong password or
 * unreadable file; stats is always filled. */
gboolean lremote_mremoteng_import(const gchar *path, const gchar *password, LremoteMrngStats *stats, GError **error);

void lremote_mremoteng_stats_clear(LremoteMrngStats *stats);

G_END_DECLS
