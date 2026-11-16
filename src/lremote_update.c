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
 * Update check against the GitHub releases of lRemote.
 *
 * The latest release is read from the GitHub API. Its x86_64 RPM asset
 * name (lremote-VERSION-RELEASE.fcNN.x86_64.rpm) is compared with the
 * running build. A newer package is downloaded to the Downloads folder
 * and installed with "pkexec dnf install", then lRemote restarts.
 *
 * The repository may be private: the token from the GitHub CLI
 * ("gh auth token") is used when available.
 */

#define _GNU_SOURCE
#include "config.h"
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <glib/gstdio.h>
#include <curl/curl.h>
#include <json-glib/json-glib.h>
#include <glib/gi18n.h>

#include "lremote_update.h"
#include "remmina_exec.h"

#define LREMOTE_REPO "mpjsilva/lRemote"
#define LREMOTE_API_LATEST "https://api.github.com/repos/" LREMOTE_REPO "/releases/latest"

typedef struct {
	GtkWindow *	parent;
	gboolean	quiet;
	/* results */
	glong		http_code;
	gchar *		error;
	gchar *		tag;
	gchar *		asset_name;
	gchar *		asset_url;	/* API url, needs Accept: octet-stream */
	gchar *		notes;
	gchar *		token;
	gchar *		download_path;
} UpdateJob;

static gboolean busy = FALSE;

static void job_free(UpdateJob *j)
{
	if (j->parent)
		g_object_unref(j->parent);
	g_free(j->error);
	g_free(j->tag);
	g_free(j->asset_name);
	g_free(j->asset_url);
	g_free(j->notes);
	if (j->token) {
		memset(j->token, 0, strlen(j->token));
		g_free(j->token);
	}
	g_free(j->download_path);
	g_free(j);
	busy = FALSE;
}

static void message(UpdateJob *j, GtkMessageType type, const gchar *primary, const gchar *secondary)
{
	GtkWidget *d = gtk_message_dialog_new(j->parent, GTK_DIALOG_MODAL, type, GTK_BUTTONS_OK, "%s", primary);
	if (secondary)
		gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", secondary);
	gtk_window_set_title(GTK_WINDOW(d), _("Updates"));
	gtk_dialog_run(GTK_DIALOG(d));
	gtk_widget_destroy(d);
}

/* "0.1.0-6.fc44" -> "0.1.0-6" */
static gchar *version_of_running(void)
{
	gchar *rel = g_strdup(LREMOTE_RELEASE);
	gchar *dot = strchr(rel, '.');
	gchar *r;
	if (dot)
		*dot = '\0';
	r = g_strdup_printf("%s-%s", LREMOTE_VERSION, rel);
	g_free(rel);
	return r;
}

/* "lremote-0.1.0-6.fc44.x86_64.rpm" -> "0.1.0-6" */
static gchar *version_of_asset(const gchar *name)
{
	const gchar *start;
	gchar *v, *dot;
	if (!g_str_has_prefix(name, "lremote-"))
		return NULL;
	start = name + strlen("lremote-");
	v = g_strdup(start);
	/* cut at the first ".fc" or "." after the release number */
	dot = strstr(v, ".fc");
	if (!dot)
		dot = strstr(v, ".x86_64");
	if (dot)
		*dot = '\0';
	return v;
}

static size_t to_gstring(void *ptr, size_t size, size_t n, void *data)
{
	g_string_append_len((GString *)data, ptr, size * n);
	return size * n;
}

static struct curl_slist *headers_for(UpdateJob *j, const gchar *accept)
{
	struct curl_slist *h = NULL;
	gchar *a = g_strdup_printf("Accept: %s", accept);
	h = curl_slist_append(h, a);
	g_free(a);
	h = curl_slist_append(h, "X-GitHub-Api-Version: 2022-11-28");
	if (j->token) {
		gchar *auth = g_strdup_printf("Authorization: Bearer %s", j->token);
		h = curl_slist_append(h, auth);
		memset(auth, 0, strlen(auth));
		g_free(auth);
	}
	return h;
}

static gchar *gh_token(void)
{
	gchar *out = NULL;
	gint status = 1;
	gchar *gh = g_find_program_in_path("gh");
	if (!gh)
		return NULL;
	g_free(gh);
	if (g_spawn_command_line_sync("gh auth token", &out, NULL, &status, NULL) && status == 0 && out) {
		g_strstrip(out);
		if (*out)
			return out;
	}
	g_free(out);
	return NULL;
}

static void check_thread(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
	UpdateJob *j = data;
	CURL *curl;
	GString *body = g_string_new(NULL);
	struct curl_slist *h;
	CURLcode rc;
	gint attempt;

	for (attempt = 0; attempt < 2; attempt++) {
		/* First try without credentials; a private repo answers 404 */
		if (attempt == 1) {
			if (j->http_code != 404 && j->http_code != 401)
				break;
			j->token = gh_token();
			if (!j->token)
				break;
		}
		g_string_truncate(body, 0);
		curl = curl_easy_init();
		h = headers_for(j, "application/vnd.github+json");
		curl_easy_setopt(curl, CURLOPT_URL, LREMOTE_API_LATEST);
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h);
		curl_easy_setopt(curl, CURLOPT_USERAGENT, "lRemote/" LREMOTE_VERSION);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, to_gstring);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
		rc = curl_easy_perform(curl);
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &j->http_code);
		curl_slist_free_all(h);
		curl_easy_cleanup(curl);
		if (rc != CURLE_OK) {
			j->error = g_strdup(curl_easy_strerror(rc));
			break;
		}
	}

	if (!j->error && j->http_code == 200) {
		JsonParser *p = json_parser_new();
		if (json_parser_load_from_data(p, body->str, body->len, NULL)) {
			JsonObject *o = json_node_get_object(json_parser_get_root(p));
			JsonArray *assets = json_object_has_member(o, "assets") ? json_object_get_array_member(o, "assets") : NULL;
			guint i;
			if (json_object_has_member(o, "tag_name"))
				j->tag = g_strdup(json_object_get_string_member(o, "tag_name"));
			if (json_object_has_member(o, "body") && !json_object_get_null_member(o, "body"))
				j->notes = g_strdup(json_object_get_string_member(o, "body"));
			for (i = 0; assets && i < json_array_get_length(assets); i++) {
				JsonObject *a = json_array_get_object_element(assets, i);
				const gchar *name = json_object_get_string_member(a, "name");
				if (name && g_str_has_suffix(name, ".x86_64.rpm")) {
					j->asset_name = g_strdup(name);
					j->asset_url = g_strdup(json_object_get_string_member(a, "url"));
					break;
				}
			}
		}
		g_object_unref(p);
	}
	g_string_free(body, TRUE);
	g_task_return_boolean(task, TRUE);
}

static void download_thread(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
	UpdateJob *j = data;
	CURL *curl = curl_easy_init();
	struct curl_slist *h = headers_for(j, "application/octet-stream");
	gchar *tmp = g_strconcat(j->download_path, ".part", NULL);
	FILE *f = fopen(tmp, "wb");
	CURLcode rc;
	glong code = 0;

	if (!f) {
		j->error = g_strdup(g_strerror(errno));
	} else {
		curl_easy_setopt(curl, CURLOPT_URL, j->asset_url);
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h);
		curl_easy_setopt(curl, CURLOPT_USERAGENT, "lRemote/" LREMOTE_VERSION);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
		/* GitHub redirects assets to storage that must not receive our token */
		curl_easy_setopt(curl, CURLOPT_UNRESTRICTED_AUTH, 0L);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);
		rc = curl_easy_perform(curl);
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
		fclose(f);
		if (rc != CURLE_OK)
			j->error = g_strdup(curl_easy_strerror(rc));
		else if (code != 200)
			j->error = g_strdup_printf(_("The server answered with code %ld."), code);
		else if (g_rename(tmp, j->download_path) != 0)
			j->error = g_strdup(g_strerror(errno));
	}
	if (j->error)
		g_unlink(tmp);
	g_free(tmp);
	curl_slist_free_all(h);
	curl_easy_cleanup(curl);
	g_task_return_boolean(task, TRUE);
}

static void restart(void)
{
	/* Wait for this process to end, then start the new version */
	gchar *cmd = g_strdup_printf("sh -c 'while kill -0 %d 2>/dev/null; do sleep 0.3; done; exec lremote'", (gint)getpid());
	g_spawn_command_line_async(cmd, NULL);
	g_free(cmd);
	remmina_exec_command(REMMINA_COMMAND_EXIT, NULL);
}

static void install_done(GObject *proc, GAsyncResult *res, gpointer data)
{
	UpdateJob *j = data;
	GError *err = NULL;
	GtkWidget *d;

	if (!g_subprocess_wait_check_finish(G_SUBPROCESS(proc), res, &err)) {
		gchar *msg = g_strdup_printf(_("The package was saved to %s. You can install it with:\nsudo dnf install %s"),
					     j->download_path, j->download_path);
		message(j, GTK_MESSAGE_ERROR, _("The update was not installed."), msg);
		g_free(msg);
		g_clear_error(&err);
		g_object_unref(proc);
		job_free(j);
		return;
	}
	g_object_unref(proc);
	d = gtk_message_dialog_new(j->parent, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_NONE,
				   _("lRemote %s was installed."), j->tag ? j->tag : "");
	gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d),
		_("Restart now to use the new version. Open connections will be closed."));
	gtk_dialog_add_buttons(GTK_DIALOG(d), _("_Later"), GTK_RESPONSE_CANCEL, _("_Restart now"), GTK_RESPONSE_OK, NULL);
	gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
	if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK) {
		gtk_widget_destroy(d);
		job_free(j);
		restart();
		return;
	}
	gtk_widget_destroy(d);
	job_free(j);
}

static void download_done(GObject *src, GAsyncResult *res, gpointer data)
{
	UpdateJob *j = data;
	GSubprocess *proc;
	GError *err = NULL;
	gchar *pkexec = g_find_program_in_path("pkexec");
	gchar *dnf = g_find_program_in_path("dnf");

	if (j->error) {
		message(j, GTK_MESSAGE_ERROR, _("Could not download the update."), j->error);
		job_free(j);
		goto out;
	}
	if (!pkexec || !dnf) {
		gchar *msg = g_strdup_printf(_("The package was saved to %s."), j->download_path);
		message(j, GTK_MESSAGE_INFO, _("Install the downloaded package with your package manager."), msg);
		g_free(msg);
		job_free(j);
		goto out;
	}
	/* pkexec asks for the administrator password in a desktop dialog */
	proc = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, &err, pkexec, dnf, "install", "-y", j->download_path, NULL);
	if (!proc) {
		message(j, GTK_MESSAGE_ERROR, _("The update was not installed."), err->message);
		g_clear_error(&err);
		job_free(j);
		goto out;
	}
	g_subprocess_wait_check_async(proc, NULL, install_done, j);
out:
	g_free(pkexec);
	g_free(dnf);
}

static void check_done(GObject *src, GAsyncResult *res, gpointer data)
{
	UpdateJob *j = data;
	gchar *mine, *theirs;
	GtkWidget *d;
	gint cmp;

	if (j->error || j->http_code != 200) {
		if (!j->quiet) {
			if (j->http_code == 404 || j->http_code == 401)
				message(j, GTK_MESSAGE_WARNING, _("Could not check for updates."),
					_("The lRemote repository is private. Sign in with the GitHub CLI (gh auth login) on this computer, or make the repository public."));
			else
				message(j, GTK_MESSAGE_WARNING, _("Could not check for updates."),
					j->error ? j->error : _("GitHub did not answer as expected."));
		}
		job_free(j);
		return;
	}
	mine = version_of_running();
	theirs = j->asset_name ? version_of_asset(j->asset_name) : NULL;
	cmp = theirs ? strverscmp(theirs, mine) : 0;
	if (cmp <= 0) {
		if (!j->quiet) {
			gchar *msg = g_strdup_printf(_("You have the latest version (%s)."), mine);
			message(j, GTK_MESSAGE_INFO, msg, NULL);
			g_free(msg);
		}
		g_free(mine);
		g_free(theirs);
		job_free(j);
		return;
	}

	d = gtk_message_dialog_new(j->parent, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
				   _("lRemote %s is available."), theirs);
	{
		gchar *sec = g_strdup_printf(_("Installed version: %s.\nDownload and install it now? Your administrator password will be asked."), mine);
		gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", sec);
		g_free(sec);
	}
	gtk_window_set_title(GTK_WINDOW(d), _("Updates"));
	gtk_dialog_add_buttons(GTK_DIALOG(d), _("_Not now"), GTK_RESPONSE_CANCEL, _("_Update"), GTK_RESPONSE_OK, NULL);
	gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
	g_free(mine);
	g_free(theirs);
	if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK) {
		const gchar *dl = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
		GTask *t;
		gtk_widget_destroy(d);
		if (!dl)
			dl = g_get_home_dir();
		g_mkdir_with_parents(dl, 0755);
		j->download_path = g_build_filename(dl, j->asset_name, NULL);
		t = g_task_new(NULL, NULL, download_done, j);
		g_task_set_task_data(t, j, NULL);
		g_task_run_in_thread(t, download_thread);
		g_object_unref(t);
		return;
	}
	gtk_widget_destroy(d);
	job_free(j);
}

void lremote_update_check(GtkWindow *parent, gboolean quiet)
{
	UpdateJob *j;
	GTask *t;

	if (busy)
		return;
	busy = TRUE;
	j = g_new0(UpdateJob, 1);
	j->parent = parent ? g_object_ref(parent) : NULL;
	j->quiet = quiet;
	t = g_task_new(NULL, NULL, check_done, j);
	g_task_set_task_data(t, j, NULL);
	g_task_run_in_thread(t, check_thread);
	g_object_unref(t);
}
