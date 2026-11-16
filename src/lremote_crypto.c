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

#include <string.h>
#include <glib.h>
#include <glib/gi18n.h>
#include <openssl/evp.h>
#include <openssl/md5.h>

#include "lremote_crypto.h"

#define MRNG_SALT_LEN  16
#define MRNG_NONCE_LEN 16
#define MRNG_TAG_LEN   16
#define MRNG_KEY_LEN   32

static GQuark lremote_crypto_quark(void)
{
	return g_quark_from_static_string("lremote-crypto");
}

static gchar *fail(GError **error, const gchar *msg)
{
	g_set_error_literal(error, lremote_crypto_quark(), 1, msg);
	return NULL;
}

gchar *lremote_mrng_decrypt_gcm(const gchar *b64, const gchar *password, gint iterations, GError **error)
{
	guchar *data, key[MRNG_KEY_LEN];
	gsize len;
	gint ct_len, out_len = 0, final_len = 0, aad_len = 0;
	gchar *out = NULL;
	EVP_CIPHER_CTX *ctx;
	gboolean ok = FALSE;

	if (!b64 || !*b64)
		return g_strdup("");
	if (!password || !*password)
		return fail(error, _("A password is required."));
	if (iterations < 1)
		iterations = 1000;

	data = g_base64_decode(b64, &len);
	if (!data || len < MRNG_SALT_LEN + MRNG_NONCE_LEN + MRNG_TAG_LEN) {
		g_free(data);
		return fail(error, _("The encrypted value is too short."));
	}
	ct_len = (gint)len - MRNG_SALT_LEN - MRNG_NONCE_LEN - MRNG_TAG_LEN;

	if (!PKCS5_PBKDF2_HMAC_SHA1(password, (int)strlen(password), data, MRNG_SALT_LEN, iterations, MRNG_KEY_LEN, key)) {
		g_free(data);
		return fail(error, _("Key derivation failed."));
	}

	out = g_malloc0(ct_len + 1);
	ctx = EVP_CIPHER_CTX_new();
	if (ctx
	    && EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1
	    && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, MRNG_NONCE_LEN, NULL) == 1
	    && EVP_DecryptInit_ex(ctx, NULL, NULL, key, data + MRNG_SALT_LEN) == 1
	    /* mRemoteNG authenticates the salt as associated data */
	    && EVP_DecryptUpdate(ctx, NULL, &aad_len, data, MRNG_SALT_LEN) == 1
	    && EVP_DecryptUpdate(ctx, (guchar *)out, &out_len, data + MRNG_SALT_LEN + MRNG_NONCE_LEN, ct_len) == 1
	    && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, MRNG_TAG_LEN, data + MRNG_SALT_LEN + MRNG_NONCE_LEN + ct_len) == 1
	    && EVP_DecryptFinal_ex(ctx, (guchar *)out + out_len, &final_len) == 1) {
		out[out_len + final_len] = '\0';
		ok = TRUE;
	}
	if (ctx)
		EVP_CIPHER_CTX_free(ctx);
	memset(key, 0, sizeof(key));
	g_free(data);
	if (!ok) {
		g_free(out);
		return fail(error, _("Wrong password or corrupted data."));
	}
	return out;
}

gchar *lremote_mrng_decrypt_legacy(const gchar *b64, const gchar *password, GError **error)
{
	guchar *data, key[MD5_DIGEST_LENGTH];
	gsize len;
	gint ct_len, out_len = 0, final_len = 0;
	gchar *out;
	EVP_CIPHER_CTX *ctx;
	gboolean ok = FALSE;

	if (!b64 || !*b64)
		return g_strdup("");
	if (!password)
		password = "";
	data = g_base64_decode(b64, &len);
	if (!data || len < 32 || (len % 16) != 0) {
		g_free(data);
		return fail(error, _("The encrypted value is malformed."));
	}
	ct_len = (gint)len - 16;
	{
		EVP_MD_CTX *md = EVP_MD_CTX_new();
		unsigned int md_len = 0;
		EVP_DigestInit_ex(md, EVP_md5(), NULL);
		EVP_DigestUpdate(md, password, strlen(password));
		EVP_DigestFinal_ex(md, key, &md_len);
		EVP_MD_CTX_free(md);
	}
	out = g_malloc0(ct_len + 1);
	ctx = EVP_CIPHER_CTX_new();
	if (ctx
	    && EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), NULL, key, data) == 1
	    && EVP_DecryptUpdate(ctx, (guchar *)out, &out_len, data + 16, ct_len) == 1
	    && EVP_DecryptFinal_ex(ctx, (guchar *)out + out_len, &final_len) == 1) {
		out[out_len + final_len] = '\0';
		ok = TRUE;
	}
	if (ctx)
		EVP_CIPHER_CTX_free(ctx);
	g_free(data);
	if (!ok) {
		g_free(out);
		return fail(error, _("Wrong password or corrupted data."));
	}
	return out;
}
