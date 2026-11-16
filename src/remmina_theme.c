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
 * Light / dark / system colour scheme support.
 *
 * "System" is resolved through the XDG desktop portal
 * (org.freedesktop.portal.Settings, namespace org.freedesktop.appearance,
 * key color-scheme), which works on GNOME, KDE, sway and inside Flatpak
 * and Snap. When the portal is not available we fall back to the GNOME
 * GSettings key org.gnome.desktop.interface color-scheme, and finally to
 * the name of the active GTK theme.
 */

#include "config.h"
#include <string.h>
#include <gio/gio.h>
#include <gtk/gtk.h>

#include "remmina_log.h"
#include "remmina_pref.h"
#include "remmina_theme.h"
#include "remmina/remmina_trace_calls.h"

#define PORTAL_BUS_NAME   "org.freedesktop.portal.Desktop"
#define PORTAL_OBJ_PATH   "/org/freedesktop/portal/desktop"
#define PORTAL_IFACE      "org.freedesktop.portal.Settings"
#define APPEARANCE_NS     "org.freedesktop.appearance"
#define COLOR_SCHEME_KEY  "color-scheme"

static GDBusProxy *portal_proxy = NULL;
static GSettings *gnome_settings = NULL;
static gboolean initialized = FALSE;
static gboolean portal_available = FALSE;
/* 0 = no preference, 1 = prefer dark, 2 = prefer light (portal values) */
static guint32 portal_scheme = 0;
static gboolean current_dark = FALSE;

static gboolean remmina_theme_gtk_theme_is_dark(void)
{
	TRACE_CALL(__func__);
	gchar *name = NULL;
	gboolean dark = FALSE;

	g_object_get(gtk_settings_get_default(), "gtk-theme-name", &name, NULL);
	if (name) {
		gchar *lower = g_ascii_strdown(name, -1);
		dark = (strstr(lower, "dark") != NULL);
		g_free(lower);
		g_free(name);
	}
	return dark;
}

static gboolean remmina_theme_gnome_prefers_dark(void)
{
	TRACE_CALL(__func__);
	gchar *value;
	gboolean dark;

	if (!gnome_settings)
		return FALSE;
	value = g_settings_get_string(gnome_settings, "color-scheme");
	dark = (g_strcmp0(value, "prefer-dark") == 0);
	g_free(value);
	return dark;
}

gboolean remmina_theme_system_prefers_dark(void)
{
	TRACE_CALL(__func__);
	if (portal_available)
		return portal_scheme == 1;
	if (gnome_settings)
		return remmina_theme_gnome_prefers_dark();
	return remmina_theme_gtk_theme_is_dark();
}

gboolean remmina_theme_is_dark(void)
{
	return current_dark;
}

void remmina_theme_apply(void)
{
	TRACE_CALL(__func__);
	gboolean dark;

	switch (remmina_pref.color_scheme) {
	case REMMINA_COLOR_SCHEME_LIGHT:
		dark = FALSE;
		break;
	case REMMINA_COLOR_SCHEME_DARK:
		dark = TRUE;
		break;
	case REMMINA_COLOR_SCHEME_SYSTEM:
	default:
		dark = remmina_theme_system_prefers_dark();
		break;
	}

	current_dark = dark;
	/* Keep the legacy boolean in sync for code and plugins still reading it */
	remmina_pref.dark_theme = dark;
	g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", dark, NULL);
	REMMINA_DEBUG("Colour scheme %d applied, dark=%d", remmina_pref.color_scheme, dark);
}

static void remmina_theme_on_portal_setting_changed(GDBusProxy *proxy, const gchar *sender_name,
						    const gchar *signal_name, GVariant *parameters, gpointer user_data)
{
	TRACE_CALL(__func__);
	const gchar *ns, *key;
	GVariant *value;

	if (g_strcmp0(signal_name, "SettingChanged") != 0)
		return;
	g_variant_get(parameters, "(&s&sv)", &ns, &key, &value);
	if (g_strcmp0(ns, APPEARANCE_NS) == 0 && g_strcmp0(key, COLOR_SCHEME_KEY) == 0
	    && g_variant_is_of_type(value, G_VARIANT_TYPE_UINT32)) {
		portal_scheme = g_variant_get_uint32(value);
		if (remmina_pref.color_scheme == REMMINA_COLOR_SCHEME_SYSTEM)
			remmina_theme_apply();
	}
	g_variant_unref(value);
}

static void remmina_theme_on_gnome_setting_changed(GSettings *settings, const gchar *key, gpointer user_data)
{
	TRACE_CALL(__func__);
	if (remmina_pref.color_scheme == REMMINA_COLOR_SCHEME_SYSTEM)
		remmina_theme_apply();
}

static void remmina_theme_on_gtk_theme_changed(GObject *settings, GParamSpec *pspec, gpointer user_data)
{
	TRACE_CALL(__func__);
	if (remmina_pref.color_scheme == REMMINA_COLOR_SCHEME_SYSTEM && !portal_available && !gnome_settings)
		remmina_theme_apply();
}

static gboolean remmina_theme_portal_read(void)
{
	TRACE_CALL(__func__);
	GError *error = NULL;
	GVariant *ret, *inner, *value;

	ret = g_dbus_proxy_call_sync(portal_proxy, "Read",
				     g_variant_new("(ss)", APPEARANCE_NS, COLOR_SCHEME_KEY),
				     G_DBUS_CALL_FLAGS_NONE, 1000, NULL, &error);
	if (!ret) {
		REMMINA_DEBUG("Portal colour scheme unavailable: %s", error ? error->message : "unknown");
		g_clear_error(&error);
		return FALSE;
	}
	/* Read returns (v) where v wraps another variant holding the uint32 */
	g_variant_get(ret, "(v)", &inner);
	if (g_variant_is_of_type(inner, G_VARIANT_TYPE_VARIANT)) {
		value = g_variant_get_variant(inner);
	} else {
		value = g_variant_ref(inner);
	}
	if (g_variant_is_of_type(value, G_VARIANT_TYPE_UINT32))
		portal_scheme = g_variant_get_uint32(value);
	g_variant_unref(value);
	g_variant_unref(inner);
	g_variant_unref(ret);
	return TRUE;
}

static void remmina_theme_init_gnome_fallback(void)
{
	TRACE_CALL(__func__);
	GSettingsSchemaSource *src = g_settings_schema_source_get_default();
	GSettingsSchema *schema;

	if (!src)
		return;
	schema = g_settings_schema_source_lookup(src, "org.gnome.desktop.interface", TRUE);
	if (!schema)
		return;
	if (g_settings_schema_has_key(schema, "color-scheme")) {
		gnome_settings = g_settings_new("org.gnome.desktop.interface");
		g_signal_connect(gnome_settings, "changed::color-scheme",
				 G_CALLBACK(remmina_theme_on_gnome_setting_changed), NULL);
	}
	g_settings_schema_unref(schema);
}

void remmina_theme_init(void)
{
	TRACE_CALL(__func__);
	GError *error = NULL;

	if (initialized) {
		remmina_theme_apply();
		return;
	}
	initialized = TRUE;

	portal_proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SESSION,
						     G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
						     NULL, PORTAL_BUS_NAME, PORTAL_OBJ_PATH, PORTAL_IFACE,
						     NULL, &error);
	if (portal_proxy) {
		portal_available = remmina_theme_portal_read();
		if (portal_available) {
			g_signal_connect(portal_proxy, "g-signal",
					 G_CALLBACK(remmina_theme_on_portal_setting_changed), NULL);
		} else {
			g_clear_object(&portal_proxy);
		}
	} else {
		REMMINA_DEBUG("Settings portal not available: %s", error ? error->message : "unknown");
		g_clear_error(&error);
	}

	if (!portal_available)
		remmina_theme_init_gnome_fallback();

	g_signal_connect(gtk_settings_get_default(), "notify::gtk-theme-name",
			 G_CALLBACK(remmina_theme_on_gtk_theme_changed), NULL);

	remmina_theme_apply();
}
