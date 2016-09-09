/*
 *
 *  Wireless daemon for Linux
 *
 *  Copyright (C) 2016  Intel Corporation. All rights reserved.
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

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <ell/ell.h>

#include "crypto.h"
#include "eap.h"
#include "wscutil.h"
#include "util.h"

#define EAP_WSC_OFFSET 12

/* WSC v2.0.5, Section 7.7.1 */
enum wsc_op {
	WSC_OP_START	= 0x01,
	WSC_OP_ACK	= 0x02,
	WSC_OP_NACK	= 0x03,
	WSC_OP_MSG	= 0x04,
	WSC_OP_DONE	= 0x05,
	WSC_OP_FRAG_ACK = 0x06,
};

/* WSC v2.0.5, Section 7.7.1 */
enum wsc_flag {
	WSC_FLAG_MF	= 0x01,
	WSC_FLAG_LF	= 0x02,
};

enum state {
	STATE_EXPECT_START = 0,
	STATE_EXPECT_M2,
	STATE_EXPECT_M4,
	STATE_EXPECT_M6,
	STATE_EXPECT_M8,
	STATE_FINISHED,
};

static struct l_key *dh5_generator;
static struct l_key *dh5_prime;

struct eap_wsc_state {
	struct wsc_m1 *m1;
	struct wsc_m2 *m2;
	uint8_t *sent_pdu;
	size_t sent_len;
	struct l_key *private;
	char *device_password;
	uint8_t e_snonce1[16];
	uint8_t e_snonce2[16];
	uint8_t iv1[16];
	uint8_t iv2[16];
	uint8_t psk1[16];
	uint8_t psk2[16];
	uint8_t r_hash2[32];
	enum state state;
	struct l_checksum *hmac_auth_key;
	struct l_cipher *aes_cbc_128;
};

static inline void eap_wsc_state_set_sent_pdu(struct eap_wsc_state *wsc,
						uint8_t *pdu, size_t len)
{
	l_free(wsc->sent_pdu);
	wsc->sent_pdu = pdu;
	wsc->sent_len = len;
}

static inline bool authenticator_check(struct eap_wsc_state *wsc,
					const uint8_t *pdu, size_t len)
{
	uint8_t authenticator[8];
	struct iovec iov[2];

	iov[0].iov_base = wsc->sent_pdu;
	iov[0].iov_len = wsc->sent_len;
	iov[1].iov_base = (void *) pdu;
	iov[1].iov_len = len - 12;
	l_checksum_updatev(wsc->hmac_auth_key, iov, 2);
	l_checksum_get_digest(wsc->hmac_auth_key, authenticator, 8);

	/* Authenticator is the last 8 bytes of the message */
	if (memcmp(authenticator, pdu + len - 8, 8))
		return false;

	return true;
}

static inline void authenticator_put(struct eap_wsc_state *wsc,
					const uint8_t *prev_msg,
					size_t prev_msg_len,
					uint8_t *cur_msg, size_t cur_msg_len)
{
	struct iovec iov[2];

	iov[0].iov_base = (void *) prev_msg;
	iov[0].iov_len = prev_msg_len;
	iov[1].iov_base = cur_msg;
	iov[1].iov_len = cur_msg_len - 12;

	l_checksum_updatev(wsc->hmac_auth_key, iov, 2);
	l_checksum_get_digest(wsc->hmac_auth_key, cur_msg + cur_msg_len - 8, 8);
}

static inline bool keywrap_authenticator_check(struct eap_wsc_state *wsc,
						const uint8_t *pdu, size_t len)
{
	uint8_t authenticator[8];

	/* We omit the included KeyWrapAuthenticator element from the hash */
	l_checksum_update(wsc->hmac_auth_key, pdu, len - 12);
	l_checksum_get_digest(wsc->hmac_auth_key, authenticator, 8);

	/* KeyWrapAuthenticator is the last 8 bytes of the message */
	if (memcmp(authenticator, pdu + len - 8, 8))
		return false;

	return true;
}

static inline void keywrap_authenticator_put(struct eap_wsc_state *wsc,
						uint8_t *pdu, size_t len)
{
	l_checksum_update(wsc->hmac_auth_key, pdu, len - 12);
	l_checksum_get_digest(wsc->hmac_auth_key, pdu + len - 8, 8);
}

static inline bool r_hash_check(struct eap_wsc_state *wsc,
				uint8_t *r_snonce,
				uint8_t *psk,
				uint8_t *r_hash_expected)
{
	struct iovec iov[4];
	uint8_t r_hash[32];

	/*
	 * WSC 2.0.5, Section 7.4:
	 * The Registrar creates two 128-bit secret nonces, R-S1, R-S2 and
	 * then computes
	 * R-Hash1 = HMACAuthKey(R-S1 || PSK1 || PKE || PKR)
	 * R-Hash2 = HMACAuthKey(R-S2 || PSK2 || PKE || PKR)
	 */
	iov[0].iov_base = r_snonce;
	iov[0].iov_len = 16;
	iov[1].iov_base = psk;
	iov[1].iov_len = 16;
	iov[2].iov_base = wsc->m1->public_key;
	iov[2].iov_len = sizeof(wsc->m1->public_key);
	iov[3].iov_base = wsc->m2->public_key;
	iov[3].iov_len = sizeof(wsc->m2->public_key);
	l_checksum_updatev(wsc->hmac_auth_key, iov, 4);
	l_checksum_get_digest(wsc->hmac_auth_key, r_hash, sizeof(r_hash));

	return !memcmp(r_hash, r_hash_expected, sizeof(r_hash));
}

static uint8_t *encrypted_settings_decrypt(struct eap_wsc_state *wsc,
						const uint8_t *pdu,
						size_t len,
						size_t *out_len)
{
	size_t encrypted_len;
	uint8_t *decrypted;
	unsigned int i;
	uint8_t pad;

	/* WSC 2.0.5, Section 12, Encrypted Settings:
	 * "The Data field of the Encrypted Settings attribute includes an
	 * initialization vector (IV) followed by a set of encrypted Wi-Fi
	 * Simple Configuration TLV attributes."
	 *
	 * Account for the IV being in the beginning 16 bytes
	 */
	if (len < 16 )
		return NULL;

	encrypted_len = len - 16;
	if (encrypted_len < 16 || encrypted_len % 16)
		return NULL;

	decrypted = l_malloc(encrypted_len);

	l_cipher_set_iv(wsc->aes_cbc_128, pdu, 16);

	if (!l_cipher_decrypt(wsc->aes_cbc_128, pdu + 16,
						decrypted, encrypted_len))
		goto fail;

	/* Check that the pad value is sane */
	pad = decrypted[encrypted_len - 1];
	if (pad > encrypted_len)
		goto fail;

	for (i = 0; i < pad; i++) {
		if (decrypted[encrypted_len - pad + i] == pad)
			continue;

		goto fail;
	}

	*out_len = encrypted_len - pad;

	return decrypted;

fail:
	l_free(decrypted);
	return NULL;
}

static bool encrypted_settings_encrypt(struct eap_wsc_state *wsc,
						const uint8_t *iv,
						const uint8_t *in,
						size_t in_len,
						uint8_t *out,
						size_t *out_len)
{
	size_t len = 0;
	unsigned int i;
	uint8_t pad;

	l_cipher_set_iv(wsc->aes_cbc_128, iv, 16);
	memcpy(out, iv, 16);
	len += 16;

	memcpy(out + len, in, in_len);
	len += in_len;

	pad = 16 - in_len % 16;

	for (i = 0; i < pad; i++)
		out[len++] = pad;

	if (!l_cipher_encrypt(wsc->aes_cbc_128, out + 16, out + 16, len - 16))
		return false;

	*out_len = len;
	return true;
}

static int eap_wsc_probe(struct eap_state *eap, const char *name)
{
	struct eap_wsc_state *wsc;

	if (strcasecmp(name, "WSC"))
		return -ENOTSUP;

	wsc = l_new(struct eap_wsc_state, 1);

	eap_set_data(eap, wsc);

	return 0;
}

static void eap_wsc_remove(struct eap_state *eap)
{
	struct eap_wsc_state *wsc = eap_get_data(eap);

	eap_set_data(eap, NULL);

	l_free(wsc->device_password);
	l_key_free(wsc->private);

	l_free(wsc->sent_pdu);
	wsc->sent_pdu = 0;
	wsc->sent_len = 0;

	l_checksum_free(wsc->hmac_auth_key);
	l_cipher_free(wsc->aes_cbc_128);

	l_free(wsc->m1);
	l_free(wsc->m2);

	l_free(wsc);
}

static void eap_wsc_send_response(struct eap_state *eap,
						uint8_t *pdu, size_t len)
{
	struct eap_wsc_state *wsc = eap_get_data(eap);
	uint8_t buf[len + 14];

	buf[12] = WSC_OP_MSG;
	buf[13] = 0;
	memcpy(buf + 14, pdu, len);

	eap_send_response(eap, EAP_TYPE_EXPANDED, buf, len + 14);

	eap_wsc_state_set_sent_pdu(wsc, pdu, len);
}

static void eap_wsc_send_nack(struct eap_state *eap,
					enum wsc_configuration_error error)
{
	struct eap_wsc_state *wsc = eap_get_data(eap);
	struct wsc_nack nack;
	uint8_t *pdu;
	size_t pdu_len;
	uint8_t buf[256];

	/*
	 * WSC 2.0.5, Table 34, Configuration Error 0 states:
	 * "- not valid for WSC_NACK except when a station acts as an External
	 * Registrar (to learn the current AP settings after M7 with
	 * configuration error = 0)"
	 *
	 * However, section 7.7.3 states:
	 * "Once M5 is sent, for example, if anything but M6 is received,
	 * the Enrollee will respond with a NACK message."
	 *
	 * Section 7.1 states:
	 * "If a message is received with either an invalid nonce or an invalid
	 * Authenticator attribute, the recipient shall silently ignore this
	 * message."
	 *
	 * So it is entirely unclear what to do in the situation of an
	 * out-of-order message being sent.  To centralize decision making,
	 * callers will call this function with error 0.
	 */
	if (error == WSC_CONFIGURATION_ERROR_NO_ERROR)
		return;

	nack.version2 = true;
	memcpy(nack.enrollee_nonce, wsc->m1->enrollee_nonce,
						sizeof(nack.enrollee_nonce));

	if (wsc->m2)
		memcpy(nack.registrar_nonce, wsc->m2->registrar_nonce,
						sizeof(nack.registrar_nonce));
	else
		memset(nack.registrar_nonce, 0, sizeof(nack.registrar_nonce));

	nack.configuration_error = error;

	pdu = wsc_build_wsc_nack(&nack, &pdu_len);
	if (!pdu)
		return;

	buf[12] = WSC_OP_NACK;
	buf[13] = 0;
	memcpy(buf + 14, pdu, pdu_len);

	eap_send_response(eap, EAP_TYPE_EXPANDED, buf, pdu_len + 14);
	l_free(pdu);
}

static void eap_wsc_send_done(struct eap_state *eap)
{
	struct eap_wsc_state *wsc = eap_get_data(eap);
	struct wsc_done done;
	uint8_t *pdu;
	size_t pdu_len;
	uint8_t buf[256];

	done.version2 = true;
	memcpy(done.enrollee_nonce, wsc->m1->enrollee_nonce,
						sizeof(done.enrollee_nonce));
	memcpy(done.registrar_nonce, wsc->m2->registrar_nonce,
						sizeof(done.registrar_nonce));

	pdu = wsc_build_wsc_done(&done, &pdu_len);
	if (!pdu)
		return;

	buf[12] = WSC_OP_DONE;
	buf[13] = 0;
	memcpy(buf + 14, pdu, pdu_len);

	eap_send_response(eap, EAP_TYPE_EXPANDED, buf, pdu_len + 14);
	l_free(pdu);
}

static void eap_wsc_handle_m8(struct eap_state *eap,
					const uint8_t *pdu, size_t len)
{
	struct eap_wsc_state *wsc = eap_get_data(eap);
	struct wsc_m8 m8;
	struct iovec encrypted;
	uint8_t *decrypted;
	size_t decrypted_len;
	struct wsc_m8_encrypted_settings m8es;
	struct iovec creds[3];
	size_t n_creds;

	/* Spec unclear what to do here, see comments in eap_wsc_send_nack */
	if (wsc_parse_m8(pdu, len, &m8, &encrypted) != 0) {
		eap_wsc_send_nack(eap, WSC_CONFIGURATION_ERROR_NO_ERROR);
		return;
	}

	if (!authenticator_check(wsc, pdu, len))
		return;

	decrypted = encrypted_settings_decrypt(wsc, encrypted.iov_base,
							encrypted.iov_len,
							&decrypted_len);
	if (!decrypted)
		goto send_nack;

	n_creds = L_ARRAY_SIZE(creds);

	if (wsc_parse_m8_encrypted_settings(decrypted, decrypted_len,
						&m8es, creds, &n_creds))
		goto invalid_settings;

	if (!keywrap_authenticator_check(wsc, decrypted, decrypted_len))
		goto invalid_settings;

	l_free(decrypted);

	eap_wsc_send_done(eap);
	wsc->state = STATE_FINISHED;
	return;

invalid_settings:
	l_free(decrypted);
send_nack:
	eap_wsc_send_nack(eap, WSC_CONFIGURATION_ERROR_DECRYPTION_CRC_FAILURE);
}

static void eap_wsc_send_m7(struct eap_state *eap,
				const uint8_t *m6_pdu, size_t m6_len)
{
	struct eap_wsc_state *wsc = eap_get_data(eap);
	struct wsc_m7_encrypted_settings m7es;
	struct wsc_m7 m7;
	uint8_t *pdu;
	size_t pdu_len;
	/* 20 for SNonce, 12 for Authenticator, 16 for IV + up to 16 pad */
	uint8_t encrypted[64];
	size_t encrypted_len;
	bool r;

	memcpy(m7es.e_snonce2, wsc->e_snonce2, sizeof(wsc->e_snonce2));
	pdu = wsc_build_m7_encrypted_settings(&m7es, &pdu_len);
	if (!pdu)
		return;

	keywrap_authenticator_put(wsc, pdu, pdu_len);
	r = encrypted_settings_encrypt(wsc, wsc->iv2, pdu, pdu_len,
						encrypted, &encrypted_len);
	l_free(pdu);

	if (!r)
		return;

	m7.version2 = true;
	memcpy(m7.registrar_nonce, wsc->m2->registrar_nonce,
						sizeof(m7.registrar_nonce));

	pdu = wsc_build_m7(&m7, encrypted, encrypted_len, &pdu_len);
	if (!pdu)
		return;

	authenticator_put(wsc, m6_pdu, m6_len, pdu, pdu_len);
	eap_wsc_send_response(eap, pdu, pdu_len);
	wsc->state = STATE_EXPECT_M8;
}

static void eap_wsc_handle_m6(struct eap_state *eap,
					const uint8_t *pdu, size_t len)
{
	struct eap_wsc_state *wsc = eap_get_data(eap);
	struct wsc_m6 m6;
	struct iovec encrypted;
	uint8_t *decrypted;
	size_t decrypted_len;
	struct wsc_m6_encrypted_settings m6es;

	/* Spec unclear what to do here, see comments in eap_wsc_send_nack */
	if (wsc_parse_m6(pdu, len, &m6, &encrypted) != 0) {
		eap_wsc_send_nack(eap, WSC_CONFIGURATION_ERROR_NO_ERROR);
		return;
	}

	if (!authenticator_check(wsc, pdu, len))
		return;

	decrypted = encrypted_settings_decrypt(wsc, encrypted.iov_base,
							encrypted.iov_len,
							&decrypted_len);
	if (!decrypted)
		goto send_nack;

	if (wsc_parse_m6_encrypted_settings(decrypted, decrypted_len, &m6es))
		goto invalid_settings;

	if (!keywrap_authenticator_check(wsc, decrypted, decrypted_len))
		goto invalid_settings;

	l_free(decrypted);

	/* We now have R-SNonce2, verify R-Hash2 stored in eap_wsc_handle_m4 */
	if (!r_hash_check(wsc, m6es.r_snonce2, wsc->psk2, wsc->r_hash2)) {
		eap_wsc_send_nack(eap,
			WSC_CONFIGURATION_ERROR_DEVICE_PASSWORD_AUTH_FAILURE);
		return;
	}

	eap_wsc_send_m7(eap, pdu, len);
	return;

invalid_settings:
	l_free(decrypted);
send_nack:
	eap_wsc_send_nack(eap, WSC_CONFIGURATION_ERROR_DECRYPTION_CRC_FAILURE);
}

static void eap_wsc_send_m5(struct eap_state *eap,
				const uint8_t *m4_pdu, size_t m4_len)
{
	struct eap_wsc_state *wsc = eap_get_data(eap);
	struct wsc_m5_encrypted_settings m5es;
	struct wsc_m5 m5;
	uint8_t *pdu;
	size_t pdu_len;
	/* 20 for SNonce, 12 for Authenticator, 16 for IV + up to 16 pad */
	uint8_t encrypted[64];
	size_t encrypted_len;
	bool r;

	memcpy(m5es.e_snonce1, wsc->e_snonce1, sizeof(wsc->e_snonce1));
	pdu = wsc_build_m5_encrypted_settings(&m5es, &pdu_len);
	if (!pdu)
		return;

	keywrap_authenticator_put(wsc, pdu, pdu_len);
	r = encrypted_settings_encrypt(wsc, wsc->iv1, pdu, pdu_len,
						encrypted, &encrypted_len);
	l_free(pdu);

	if (!r)
		return;

	m5.version2 = true;
	memcpy(m5.registrar_nonce, wsc->m2->registrar_nonce,
						sizeof(m5.registrar_nonce));

	pdu = wsc_build_m5(&m5, encrypted, encrypted_len, &pdu_len);
	if (!pdu)
		return;

	authenticator_put(wsc, m4_pdu, m4_len, pdu, pdu_len);
	eap_wsc_send_response(eap, pdu, pdu_len);
	wsc->state = STATE_EXPECT_M6;
}

static void eap_wsc_handle_m4(struct eap_state *eap,
					const uint8_t *pdu, size_t len)
{
	struct eap_wsc_state *wsc = eap_get_data(eap);
	struct wsc_m4 m4;
	struct iovec encrypted;
	uint8_t *decrypted;
	size_t decrypted_len;
	struct wsc_m4_encrypted_settings m4es;

	/* Spec unclear what to do here, see comments in eap_wsc_send_nack */
	if (wsc_parse_m4(pdu, len, &m4, &encrypted) != 0) {
		eap_wsc_send_nack(eap, WSC_CONFIGURATION_ERROR_NO_ERROR);
		return;
	}

	if (!authenticator_check(wsc, pdu, len))
		return;

	decrypted = encrypted_settings_decrypt(wsc, encrypted.iov_base,
							encrypted.iov_len,
							&decrypted_len);
	if (!decrypted)
		goto send_nack;

	if (wsc_parse_m4_encrypted_settings(decrypted, decrypted_len, &m4es))
		goto invalid_settings;

	if (!keywrap_authenticator_check(wsc, decrypted, decrypted_len))
		goto invalid_settings;

	l_free(decrypted);

	/* Since we have obtained R-SNonce1, we can now verify R-Hash1. */
	if (!r_hash_check(wsc, m4es.r_snonce1, wsc->psk1, m4.r_hash1)) {
		eap_wsc_send_nack(eap,
			WSC_CONFIGURATION_ERROR_DEVICE_PASSWORD_AUTH_FAILURE);
		return;
	}

	/* Now store R_Hash2 so we can verify it when we receive M6 */
	memcpy(wsc->r_hash2, m4.r_hash2, sizeof(m4.r_hash2));
	eap_wsc_send_m5(eap, pdu, len);
	return;

invalid_settings:
	l_free(decrypted);
send_nack:
	eap_wsc_send_nack(eap, WSC_CONFIGURATION_ERROR_DECRYPTION_CRC_FAILURE);
}

static void eap_wsc_send_m3(struct eap_state *eap,
				const uint8_t *m2_pdu, size_t m2_len)
{
	struct eap_wsc_state *wsc = eap_get_data(eap);
	struct wsc_m2 *m2 = wsc->m2;
	size_t len;
	size_t len_half1;
	struct wsc_m3 m3;
	struct iovec iov[4];
	uint8_t *pdu;
	size_t pdu_len;

	len = strlen(wsc->device_password);

	/* WSC 2.0.5, Section 7.4:
	 * In case the UTF8 representation of the DevicePassword length is an
	 * odd number (N), the first half of DevicePassword will have length
	 * of N/2+1 and the second half of the DevicePassword will have length
	 * of N/2.
	 */
	len_half1 = len / 2;
	if ((len % 2) == 1)
		len_half1 += 1;

	l_checksum_update(wsc->hmac_auth_key, wsc->device_password, len_half1);
	l_checksum_get_digest(wsc->hmac_auth_key, wsc->psk1, sizeof(wsc->psk1));

	l_checksum_update(wsc->hmac_auth_key, wsc->device_password + len_half1,
					len / 2);
	l_checksum_get_digest(wsc->hmac_auth_key, wsc->psk2, sizeof(wsc->psk2));

	m3.version2 = true;
	memcpy(m3.registrar_nonce, m2->registrar_nonce,
						sizeof(m3.registrar_nonce));

	/* WSC 2.0.5, Section 7.4:
	 * The Enrollee creates two 128-bit secret nonces, E-S1, E-S2 and then
	 * computes:
	 * E-Hash1 = HMACAuthKey(E-S1 || PSK1 || PKE || PKR)
	 * E-Hash2 = HMACAuthKey(E-S2 || PSK2 || PKE || PKR)
	 */
	iov[0].iov_base = wsc->e_snonce1;
	iov[0].iov_len = sizeof(wsc->e_snonce1);
	iov[1].iov_base = wsc->psk1;
	iov[1].iov_len = sizeof(wsc->psk1);
	iov[2].iov_base = wsc->m1->public_key;
	iov[2].iov_len = sizeof(wsc->m1->public_key);
	iov[3].iov_base = m2->public_key;
	iov[3].iov_len = sizeof(m2->public_key);
	l_checksum_updatev(wsc->hmac_auth_key, iov, 4);
	l_checksum_get_digest(wsc->hmac_auth_key,
					m3.e_hash1, sizeof(m3.e_hash1));

	iov[0].iov_base = wsc->e_snonce2;
	iov[0].iov_len = sizeof(wsc->e_snonce2);
	iov[1].iov_base = wsc->psk2;
	iov[1].iov_len = sizeof(wsc->psk2);
	l_checksum_updatev(wsc->hmac_auth_key, iov, 4);
	l_checksum_get_digest(wsc->hmac_auth_key,
					m3.e_hash2, sizeof(m3.e_hash2));

	pdu = wsc_build_m3(&m3, &pdu_len);
	if (!pdu)
		return;

	authenticator_put(wsc, m2_pdu, m2_len, pdu, pdu_len);
	eap_wsc_send_response(eap, pdu, pdu_len);
	wsc->state = STATE_EXPECT_M4;
}

static void eap_wsc_handle_m2(struct eap_state *eap,
					const uint8_t *pdu, size_t len)
{
	struct eap_wsc_state *wsc = eap_get_data(eap);
	struct l_key *remote_public;
	uint8_t shared_secret[192];
	size_t shared_secret_len = sizeof(shared_secret);
	struct l_checksum *sha256;
	uint8_t dhkey[32];
	struct l_checksum *hmac_sha256;
	struct iovec iov[3];
	uint8_t kdk[32];
	struct wsc_session_key keys;
	bool r;

	/* TODO: Check to see if message is M2D first */
	if (!wsc->m2)
		wsc->m2 = l_new(struct wsc_m2, 1);

	/* Spec unclear what to do here, see comments in eap_wsc_send_nack */
	if (wsc_parse_m2(pdu, len, wsc->m2) != 0) {
		eap_wsc_send_nack(eap, WSC_CONFIGURATION_ERROR_NO_ERROR);
		return;
	}

	remote_public = l_key_new(L_KEY_RAW, wsc->m2->public_key,
						sizeof(wsc->m2->public_key));
	if (!remote_public)
		return;

	r = l_key_compute_dh_secret(remote_public, wsc->private, dh5_prime,
					shared_secret, &shared_secret_len);
	l_key_free(remote_public);

	if (!r)
		return;

	sha256 = l_checksum_new(L_CHECKSUM_SHA256);
	if (!sha256)
		return;

	l_checksum_update(sha256, shared_secret, shared_secret_len);
	l_checksum_get_digest(sha256, dhkey, sizeof(dhkey));
	l_checksum_free(sha256);

	memset(shared_secret, 0, shared_secret_len);

	hmac_sha256 = l_checksum_new_hmac(L_CHECKSUM_SHA256,
							dhkey, sizeof(dhkey));
	memset(dhkey, 0, sizeof(dhkey));

	if (!hmac_sha256)
		return;

	iov[0].iov_base = wsc->m1->enrollee_nonce;
	iov[0].iov_len = 16;
	iov[1].iov_base = wsc->m1->addr;
	iov[1].iov_len = 6;
	iov[2].iov_base = wsc->m2->registrar_nonce;
	iov[2].iov_len = 16;

	l_checksum_updatev(hmac_sha256, iov, 3);
	l_checksum_get_digest(hmac_sha256, kdk, sizeof(kdk));
	l_checksum_free(hmac_sha256);

	r = wsc_kdf(kdk, &keys, sizeof(keys));
	memset(kdk, 0, sizeof(kdk));
	if (!r)
		return;

	wsc->hmac_auth_key = l_checksum_new_hmac(L_CHECKSUM_SHA256,
						keys.auth_key,
						sizeof(keys.auth_key));
	if (!authenticator_check(wsc, pdu, len)) {
		l_checksum_free(wsc->hmac_auth_key);
		wsc->hmac_auth_key = NULL;
		goto clear_keys;
	}

	/* Everything checks out, lets build M3 */
	eap_wsc_send_m3(eap, pdu, len);

	/*
	 * AuthKey is uploaded into the kernel, once we upload KeyWrapKey,
	 * the keys variable is no longer useful.  Make sure to wipe it
	 */
	wsc->aes_cbc_128 = l_cipher_new(L_CIPHER_AES_CBC, keys.keywrap_key,
						sizeof(keys.keywrap_key));

clear_keys:
	memset(&keys, 0, sizeof(keys));
}

static void eap_wsc_handle_request(struct eap_state *eap,
					const uint8_t *pkt, size_t len)
{
	struct eap_wsc_state *wsc = eap_get_data(eap);
	uint8_t op;
	uint8_t flags;
	uint8_t *pdu;
	size_t pdu_len;

	if (len < 2)
		return;

	op = pkt[0];
	flags = pkt[1];

	/* TODO: Handle fragmentation */
	if (flags != 0)
		return;

	switch (op) {
	case WSC_OP_START:
		if (len != 2)
			return;

		if (wsc->state != STATE_EXPECT_START)
			return;

		pdu = wsc_build_m1(wsc->m1, &pdu_len);
		if (!pdu)
			return;

		eap_wsc_send_response(eap, pdu, pdu_len);
		wsc->state = STATE_EXPECT_M2;
		return;
	case WSC_OP_NACK:
		/* TODO: Process NACK and respond with NACK */
		return;
	case WSC_OP_ACK:
	case WSC_OP_DONE:
		/* Should never receive these as Enrollee */
		return;
	case WSC_OP_FRAG_ACK:
		/* TODO: Handle fragmentation */
		return;
	case WSC_OP_MSG:
		break;
	}

	if (len <= 2)
		return;

	switch (wsc->state) {
	case STATE_EXPECT_START:
		return;
	case STATE_EXPECT_M2:
		eap_wsc_handle_m2(eap, pkt + 2, len - 2);
		break;
	case STATE_EXPECT_M4:
		eap_wsc_handle_m4(eap, pkt + 2, len - 2);
		break;
	case STATE_EXPECT_M6:
		eap_wsc_handle_m6(eap, pkt + 2, len - 2);
		break;
	case STATE_EXPECT_M8:
		eap_wsc_handle_m8(eap, pkt + 2, len - 2);
		break;
	case STATE_FINISHED:
		eap_wsc_send_nack(eap, WSC_CONFIGURATION_ERROR_NO_ERROR);
		return;
	}
}

static bool load_hexencoded(struct l_settings *settings, const char *key,
						uint8_t *to, size_t len)
{
	const char *v;
	size_t decoded_len;
	unsigned char *decoded;

	v = l_settings_get_value(settings, "WSC", key);
	if (!v)
		return false;

	decoded = l_util_from_hexstring(v, &decoded_len);
	if (!decoded)
		return false;

	if (decoded_len != len) {
		l_free(decoded);
		return false;
	}

	memcpy(to, decoded, len);
	l_free(decoded);

	return true;
}

static bool load_primary_device_type(struct l_settings *settings,
					struct wsc_primary_device_type *pdt)
{
	const char *v;
	int r;

	v = l_settings_get_value(settings, "WSC", "PrimaryDeviceType");
	if (!v)
		return false;

	r = sscanf(v, "%hx-%2hhx%2hhx%2hhx%2hhx-%2hx", &pdt->category,
			&pdt->oui[0], &pdt->oui[1], &pdt->oui[2],
			&pdt->oui_type, &pdt->subcategory);
	if (r != 6)
		return false;

	return true;
}

static bool load_constrained_string(struct l_settings *settings,
						const char *key,
						char *out, size_t max)
{
	char *v;
	size_t tocopy;

	v = l_settings_get_string(settings, "WSC", key);
	if (!v)
		return false;

	tocopy = strlen(v);
	if (tocopy >= max)
		tocopy = max - 1;

	memcpy(out, v, tocopy);
	out[max - 1] = '\0';

	l_free(v);

	return true;
}

static bool eap_wsc_load_settings(struct eap_state *eap,
					struct l_settings *settings,
					const char *prefix)
{
	struct eap_wsc_state *wsc = eap_get_data(eap);
	const char *v;
	uint8_t private_key[192];
	size_t len;
	unsigned int u32;
	const char *device_password;

	wsc->m1 = l_new(struct wsc_m1, 1);
	wsc->m1->version2 = true;

	v = l_settings_get_value(settings, "WSC", "EnrolleeMAC");
	if (!v)
		return false;

	if (!util_string_to_address(v, wsc->m1->addr))
		return false;

	if (!wsc_uuid_from_addr(wsc->m1->addr, wsc->m1->uuid_e))
		return false;

	if (!load_hexencoded(settings, "EnrolleeNonce",
						wsc->m1->enrollee_nonce, 16))
		l_getrandom(wsc->m1->enrollee_nonce, 16);

	if (!load_hexencoded(settings, "PrivateKey", private_key, 192))
		l_getrandom(private_key, 192);

	wsc->private = l_key_new(L_KEY_RAW, private_key, 192);
	memset(private_key, 0, 192);

	if (!wsc->private)
		return false;

	len = sizeof(wsc->m1->public_key);
	if (!l_key_compute_dh_public(dh5_generator, wsc->private, dh5_prime,
						wsc->m1->public_key, &len))
		return false;

	if (len != sizeof(wsc->m1->public_key))
		return false;

	wsc->m1->auth_type_flags = WSC_AUTHENTICATION_TYPE_WPA2_PERSONAL |
					WSC_AUTHENTICATION_TYPE_WPA_PERSONAL |
					WSC_AUTHENTICATION_TYPE_OPEN;
	wsc->m1->encryption_type_flags = WSC_ENCRYPTION_TYPE_NONE |
						WSC_ENCRYPTION_TYPE_AES_TKIP;
	wsc->m1->connection_type_flags = WSC_CONNECTION_TYPE_ESS;

	if (!l_settings_get_uint(settings, "WSC",
						"ConfigurationMethods", &u32))
		u32 = WSC_CONFIGURATION_METHOD_VIRTUAL_DISPLAY_PIN;

	wsc->m1->config_methods = u32;
	wsc->m1->state = WSC_STATE_NOT_CONFIGURED;

	if (!load_constrained_string(settings, "Manufacturer",
			wsc->m1->manufacturer, sizeof(wsc->m1->manufacturer)))
		strcpy(wsc->m1->manufacturer, " ");

	if (!load_constrained_string(settings, "ModelName",
			wsc->m1->model_name, sizeof(wsc->m1->model_name)))
		strcpy(wsc->m1->model_name, " ");

	if (!load_constrained_string(settings, "ModelNumber",
			wsc->m1->model_number, sizeof(wsc->m1->model_number)))
		strcpy(wsc->m1->model_number, " ");

	if (!load_constrained_string(settings, "SerialNumber",
			wsc->m1->serial_number, sizeof(wsc->m1->serial_number)))
		strcpy(wsc->m1->serial_number, " ");

	if (!load_primary_device_type(settings,
					&wsc->m1->primary_device_type)) {
		/* Make ourselves a WFA standard PC by default */
		wsc->m1->primary_device_type.category = 1;
		memcpy(wsc->m1->primary_device_type.oui, wsc_wfa_oui, 3);
		wsc->m1->primary_device_type.oui_type = 0x04;
		wsc->m1->primary_device_type.subcategory = 1;
	}

	if (!load_constrained_string(settings, "DeviceName",
			wsc->m1->device_name, sizeof(wsc->m1->device_name)))
		strcpy(wsc->m1->device_name, " ");

	if (!l_settings_get_uint(settings, "WSC", "RFBand", &u32))
		return false;

	switch (u32) {
	case WSC_RF_BAND_2_4_GHZ:
	case WSC_RF_BAND_5_0_GHZ:
	case WSC_RF_BAND_60_GHZ:
		wsc->m1->rf_bands = u32;
		break;
	default:
		return false;
	}

	wsc->m1->association_state = WSC_ASSOCIATION_STATE_NOT_ASSOCIATED;
	wsc->m1->device_password_id = WSC_DEVICE_PASSWORD_ID_PUSH_BUTTON;
	wsc->m1->configuration_error = WSC_CONFIGURATION_ERROR_NO_ERROR;

	if (!l_settings_get_uint(settings, "WSC",
						"OSVersion", &u32))
		u32 = 0;

	wsc->m1->os_version = u32 & 0x7fffffff;

	device_password = l_settings_get_string(settings, "WSC",
							"DevicePassword");
	if (device_password) {
		int i;

		for (i = 0; device_password[i]; i++) {
			if (!l_ascii_isxdigit(device_password[i]))
				return false;
		}

		if (i < 8)
			return false;

		wsc->device_password = strdup(device_password);
		/*
		 * WSC 2.0.5: Section 7.4:
		 * If an out-of-band mechanism is used as the configuration
		 * method, the device password is expressed in hexadecimal
		 * using ASCII character (two characters per octet, uppercase
		 * letters only).
		 */
		for (i = 0; wsc->device_password[i]; i++) {
			if (wsc->device_password[i] >= 'a' &&
					wsc->device_password[i] <= 'f')
				wsc->device_password[i] =
					'A' + wsc->device_password[i] - 'a';
		}
	} else
		wsc->device_password = strdup("00000000");

	if (!load_hexencoded(settings, "E-SNonce1", wsc->e_snonce1, 16))
		l_getrandom(wsc->e_snonce1, 16);

	if (!load_hexencoded(settings, "E-SNonce2", wsc->e_snonce2, 16))
		l_getrandom(wsc->e_snonce2, 16);

	if (!load_hexencoded(settings, "IV1", wsc->iv1, 16))
		l_getrandom(wsc->iv1, 16);

	if (!load_hexencoded(settings, "IV2", wsc->iv2, 16))
		l_getrandom(wsc->iv2, 16);

	return true;
}

static struct eap_method eap_wsc = {
	.vendor_id = { 0x00, 0x37, 0x2a },
	.vendor_type = 0x00000001,
	.request_type = EAP_TYPE_EXPANDED,
	.exports_msk = true,
	.name = "WSC",
	.probe = eap_wsc_probe,
	.remove = eap_wsc_remove,
	.handle_request = eap_wsc_handle_request,
	.load_settings = eap_wsc_load_settings,
};

static int eap_wsc_init(void)
{
	int r = -ENOTSUP;

	l_debug("");

	dh5_generator = l_key_new(L_KEY_RAW, crypto_dh5_generator,
						crypto_dh5_generator_size);
	if (!dh5_generator)
		goto fail_generator;

	dh5_prime = l_key_new(L_KEY_RAW, crypto_dh5_prime,
						crypto_dh5_prime_size);
	if (!dh5_prime)
		goto fail_prime;

	r = eap_register_method(&eap_wsc);
	if (!r)
		return 0;

	l_key_free(dh5_prime);
	dh5_prime = NULL;

fail_prime:
	l_key_free(dh5_generator);
	dh5_generator = NULL;
fail_generator:
	return r;
}

static void eap_wsc_exit(void)
{
	l_debug("");

	eap_unregister_method(&eap_wsc);

	l_key_free(dh5_prime);
	l_key_free(dh5_generator);
}

EAP_METHOD_BUILTIN(eap_wsc, eap_wsc_init, eap_wsc_exit)
