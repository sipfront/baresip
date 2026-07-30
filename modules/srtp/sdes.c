/**
 * @file /srtp/sdes.c  SDP Security Descriptions for Media Streams (RFC 4568)
 *
 * Copyright (C) 2010 Alfred E. Heggestad
 */
#include <string.h>
#include <re.h>
#include <baresip.h>
#include "sdes.h"


const char sdp_attr_crypto[] = "crypto";


/**
 * Encodes raw SDES keying material using canonical padded Base64.
 *
 * @param encoded      Buffer that receives the Base64 key.
 * @param encoded_size Total size of the output buffer.
 * @param key          Raw master key and salt.
 * @param key_len      Length of the raw keying material.
 * @param encoded_len  Length of the Base64 key on return.
 *
 * @return 0 on success, otherwise an error code.
 */
int sdes_encode_key(char *encoded, size_t encoded_size,
		    const uint8_t *key, size_t key_len, size_t *encoded_len)
{
	const size_t padding_len = (3 - key_len % 3) % 3;
	const size_t canonical_len = 4 * ((key_len + 2) / 3);
	const size_t unpadded_len = canonical_len - padding_len;
	int err;

	if (!encoded || !key || !encoded_len)
		return EINVAL;

	if (canonical_len >= encoded_size)
		return EOVERFLOW;

	*encoded_len = encoded_size;
	err = base64_encode(key, key_len, encoded, encoded_len);
	if (err)
		return err;

	if (*encoded_len < unpadded_len || *encoded_len > canonical_len)
		return EBADMSG;

	memset(encoded + unpadded_len, '=', padding_len);
	encoded[canonical_len] = '\0';
	*encoded_len = canonical_len;

	return 0;
}


/**
 * Encodes an SDES crypto attribute with canonical Base64 key padding.
 *
 * @param m       SDP media object that receives the crypto attribute.
 * @param tag     Crypto attribute tag.
 * @param suite   SRTP crypto suite name.
 * @param key     Base64-encoded master key and salt.
 * @param key_len Length of the Base64-encoded key.
 *
 * @return 0 on success, otherwise an error code.
 */
int sdes_encode_crypto(struct sdp_media *m, uint32_t tag, const char *suite,
		       const char *key, size_t key_len)
{
	static const char padding[] = "==";
	const size_t remainder = key_len % 4;
	const size_t padding_len = remainder ? 4 - remainder : 0;

	if (remainder == 1)
		return EBADMSG;

	return sdp_media_set_lattr(m, true, sdp_attr_crypto,
				   "%u %s inline:%b%b", tag, suite,
				   key, key_len, padding, padding_len);
}


/* http://tools.ietf.org/html/rfc4568
 * a=crypto:<tag> <crypto-suite> <key-params> [<session-params>]
 */
int sdes_decode_crypto(struct crypto *c, const char *val)
{
	struct pl tag, key_prms;
	int err;

	err = re_regex(val, str_len(val), "[0-9]+ [^ ]+ [^ ]+[]*[^]*",
		       &tag, &c->suite, &key_prms, NULL, &c->sess_prms);
	if (err)
		return err;

	c->tag = pl_u32(&tag);

	c->lifetime = c->mki = pl_null;
	err = re_regex(key_prms.p, key_prms.l, "[^:]+:[^|]+[|]*[^|]*[|]*[^|]*",
		       &c->key_method, &c->key_info,
		       NULL, &c->lifetime, NULL, &c->mki);
	if (err)
		return err;

	return 0;
}
