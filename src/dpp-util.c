/*
 *
 *  Wireless daemon for Linux
 *
 *  Copyright (C) 2021  Intel Corporation. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>

#include <errno.h>

#include <ell/ell.h>

#include "src/dpp-util.h"
#include "src/util.h"
#include "src/band.h"
#include "src/crypto.h"

static void append_freqs(struct l_string *uri,
					const uint32_t *freqs, size_t len)
{
	size_t i;
	enum band_freq band;

	l_string_append_printf(uri, "C:");

	for (i = 0; i < len; i++) {
		uint8_t oper_class;
		uint8_t channel = band_freq_to_channel(freqs[i], &band);

		/* For now use global operating classes */
		if (band == BAND_FREQ_2_4_GHZ)
			oper_class = 81;
		else
			oper_class = 115;

		l_string_append_printf(uri, "%u/%u", oper_class, channel);

		if (i != len - 1)
			l_string_append_c(uri, ',');
	}

	l_string_append_c(uri, ';');
}

char *dpp_generate_uri(const uint8_t *asn1, size_t asn1_len, uint8_t version,
			const uint8_t *mac, const uint32_t *freqs,
			size_t freqs_len, const char *info, const char *host)
{
	struct l_string *uri = l_string_new(256);
	char *base64;

	base64 = l_base64_encode(asn1, asn1_len, 0);

	l_string_append_printf(uri, "DPP:K:%s;", base64);
	l_free(base64);

	if (mac)
		l_string_append_printf(uri, "M:%02x%02x%02x%02x%02x%02x;",
					MAC_STR(mac));

	if (freqs)
		append_freqs(uri, freqs, freqs_len);

	if (info)
		l_string_append_printf(uri, "I:%s;", info);

	if (host)
		l_string_append_printf(uri, "H:%s;", host);

	if (version)
		l_string_append_printf(uri, "V:%u;", version);

	l_string_append_c(uri, ';');

	return l_string_unwrap(uri);
}

void dpp_attr_iter_init(struct dpp_attr_iter *iter, const uint8_t *pdu,
			size_t len)
{
	iter->pos = pdu;
	iter->end = pdu + len;
}

bool dpp_attr_iter_next(struct dpp_attr_iter *iter,
			enum dpp_attribute_type *type_out, size_t *len_out,
			const uint8_t **data_out)
{
	enum dpp_attribute_type type;
	size_t len;

	if (iter->pos + 4 > iter->end)
		return false;

	type = l_get_le16(iter->pos);
	len = l_get_le16(iter->pos + 2);

	iter->pos += 4;

	if (iter->pos + len > iter->end)
		return false;

	*type_out = type;
	*len_out = len;
	*data_out = iter->pos;

	iter->pos += len;

	return true;
}

/*
 * EasyConnect 2.0 Table 3. Key and Nonce Length Dependency on Prime Length
 */
static enum l_checksum_type dpp_sha_from_key_len(size_t len)
{
	if (len == 32)
		return L_CHECKSUM_SHA256;
	else if (len == 48)
		return L_CHECKSUM_SHA384;
	else if (len == 64)
		return L_CHECKSUM_SHA512;
	else
		return L_CHECKSUM_NONE;

}

size_t dpp_nonce_len_from_key_len(size_t len)
{
	if (len == 32)
		return 16;
	else if (len == 48)
		return 24;
	else if (len == 64)
		return 32;
	else
		return 0;
}

/*
 * 3.2.2
 * H()
 */
bool dpp_hash(enum l_checksum_type type, uint8_t *out, unsigned int num, ...)
{
	struct l_checksum *sha = l_checksum_new(type);
	size_t hsize = l_checksum_digest_length(type);
	unsigned int i;

	va_list va;

	va_start(va, num);

	for (i = 0; i < num; i++) {
		void *data = va_arg(va, void *);
		size_t len = va_arg(va, size_t);

		l_checksum_update(sha, data, len);
	}

	va_end(va);

	l_checksum_get_digest(sha, out, hsize);
	l_checksum_free(sha);

	return true;
}

/*
 * 3.2.2
 *
 * HKDF is defined as:
 *
 * key = HKDF(salt, info, ikm)
 *     = HKDF-Expand(HKDF-Extract(salt, ikm), info, len)
 *
 * Note: A NULL 'salt' means a zero'ed buffer and 'salt_len' should still be
 *       set for this zero'ed buffer length.
 */
static bool dpp_hkdf(enum l_checksum_type sha, const void *salt,
			size_t salt_len, const char *info, const void *ikm,
			size_t ikm_len, void *out, size_t out_len)
{
	uint8_t tmp[64];
	uint8_t zero_salt[64] = { 0 };
	size_t hash_len = l_checksum_digest_length(sha);

	if (!salt)
		salt = zero_salt;

	if (!hkdf_extract(sha, salt, salt_len, 1, tmp,
				ikm, ikm_len))
		return false;

	return hkdf_expand(sha, tmp, hash_len, info, out, out_len);
}

bool dpp_derive_r_auth(const void *i_nonce, const void *r_nonce,
				size_t nonce_len, struct l_ecc_point *i_proto,
				struct l_ecc_point *r_proto,
				struct l_ecc_point *r_boot,
				void *r_auth)
{
	uint64_t pix[L_ECC_MAX_DIGITS];
	uint64_t prx[L_ECC_MAX_DIGITS];
	uint64_t brx[L_ECC_MAX_DIGITS];
	size_t keys_len;
	uint8_t zero = 0;
	enum l_checksum_type type;

	keys_len = l_ecc_point_get_x(i_proto, pix, sizeof(pix));
	l_ecc_point_get_x(r_proto, prx, sizeof(prx));
	l_ecc_point_get_x(r_boot, brx, sizeof(brx));

	type = dpp_sha_from_key_len(keys_len);

	/*
	 * R-auth = H(I-nonce | R-nonce | PI.x | PR.x | [ BI.x | ] BR.x | 0)
	 */
	return dpp_hash(type, r_auth, 6, i_nonce, nonce_len, r_nonce, nonce_len,
			pix, keys_len, prx, keys_len, brx, keys_len, &zero, 1);
}

bool dpp_derive_i_auth(const void *r_nonce, const void *i_nonce,
				size_t nonce_len, struct l_ecc_point *r_proto,
				struct l_ecc_point *i_proto,
				struct l_ecc_point *r_boot, void *i_auth)
{
	uint64_t prx[L_ECC_MAX_DIGITS];
	uint64_t pix[L_ECC_MAX_DIGITS];
	uint64_t brx[L_ECC_MAX_DIGITS];
	size_t keys_len;
	uint8_t one = 1;
	enum l_checksum_type type;

	keys_len = l_ecc_point_get_x(r_proto, prx, sizeof(prx));
	l_ecc_point_get_x(i_proto, pix, sizeof(pix));
	l_ecc_point_get_x(r_boot, brx, sizeof(brx));

	type = dpp_sha_from_key_len(keys_len);

	/*
	 * I-auth = H(R-nonce | I-nonce | PR.x | PI.x | BR.x | [ BI.x | ] 1)
	 */
	return dpp_hash(type, i_auth, 6, r_nonce, nonce_len, i_nonce, nonce_len,
			prx, keys_len, pix, keys_len, brx, keys_len, &one, 1);
}

/*
 * Derives key k1. This returns the intermediate secret M.x used in deriving
 * key ke.
 */
struct l_ecc_scalar *dpp_derive_k1(const struct l_ecc_point *i_proto_public,
				const struct l_ecc_scalar *boot_private,
				void *k1)
{
	struct l_ecc_scalar *m;
	uint64_t mx_bytes[L_ECC_MAX_DIGITS];
	ssize_t key_len;
	enum l_checksum_type sha;

	if (!l_ecdh_generate_shared_secret(boot_private, i_proto_public, &m))
		return NULL;

	key_len = l_ecc_scalar_get_data(m, mx_bytes, sizeof(mx_bytes));

	sha = dpp_sha_from_key_len(key_len);

	if (!dpp_hkdf(sha, NULL, key_len, "first intermediate key", mx_bytes,
			key_len, k1, key_len)) {
		l_ecc_scalar_free(m);
		return NULL;
	}

	return m;
}

/*
 * Derives key k2. This returns the intermediate secret N.x used in deriving
 * key ke.
 */
struct l_ecc_scalar *dpp_derive_k2(const struct l_ecc_point *i_proto_public,
				const struct l_ecc_scalar *proto_private,
				void *k2)
{
	struct l_ecc_scalar *n;
	uint64_t nx_bytes[L_ECC_MAX_DIGITS];
	ssize_t key_len;
	enum l_checksum_type sha;

	if (!l_ecdh_generate_shared_secret(proto_private, i_proto_public, &n))
		return NULL;

	key_len = l_ecc_scalar_get_data(n, nx_bytes, sizeof(nx_bytes));

	sha = dpp_sha_from_key_len(key_len);

	if (!dpp_hkdf(sha, NULL, key_len, "second intermediate key", nx_bytes,
			key_len, k2, key_len)) {
		l_ecc_scalar_free(n);
		return NULL;
	}

	return n;
}

bool dpp_derive_ke(const uint8_t *i_nonce, const uint8_t *r_nonce,
				struct l_ecc_scalar *m, struct l_ecc_scalar *n,
				void *ke)
{
	uint8_t nonces[32 + 32];
	size_t nonce_len;
	uint64_t mx_bytes[L_ECC_MAX_DIGITS];
	uint64_t nx_bytes[L_ECC_MAX_DIGITS];
	uint64_t bk[L_ECC_MAX_DIGITS];
	ssize_t key_len;
	enum l_checksum_type sha;

	key_len = l_ecc_scalar_get_data(m, mx_bytes, sizeof(mx_bytes));
	l_ecc_scalar_get_data(n, nx_bytes, sizeof(nx_bytes));

	nonce_len = dpp_nonce_len_from_key_len(key_len);
	sha = dpp_sha_from_key_len(key_len);

	memcpy(nonces, i_nonce, nonce_len);
	memcpy(nonces + nonce_len, r_nonce, nonce_len);

	/* bk = HKDF-Extract(I-nonce | R-nonce, M.x | N.x [ | L.x]) */
	if (!hkdf_extract(sha, nonces, nonce_len * 2, 2, bk, mx_bytes,
			key_len, nx_bytes, key_len))
		return false;

	/* ke = HKDF-Expand(bk, "DPP Key", length) */
	return hkdf_expand(sha, bk, key_len, "DPP Key", ke, key_len);
}