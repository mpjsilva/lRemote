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

/* Colour scheme preference (stored in remmina_pref.color_scheme) */
typedef enum {
	REMMINA_COLOR_SCHEME_SYSTEM = 0,	/* follow the desktop setting */
	REMMINA_COLOR_SCHEME_LIGHT  = 1,
	REMMINA_COLOR_SCHEME_DARK   = 2,
} RemminaColorScheme;

/* Starts watching the desktop colour scheme and applies the current preference */
void remmina_theme_init(void);
/* Re-applies the colour scheme from remmina_pref.color_scheme */
void remmina_theme_apply(void);
/* TRUE when the desktop currently prefers a dark appearance */
gboolean remmina_theme_system_prefers_dark(void);
/* TRUE when Remmina is currently showing its dark variant */
gboolean remmina_theme_is_dark(void);

G_END_DECLS
