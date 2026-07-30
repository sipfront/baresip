/**
 * @file test/sdes.c Selftest for SDP Security Descriptions.
 *
 * Copyright (C) 2026 Sipfront
 */
#include <re.h>
#include <baresip.h>
#include "../modules/srtp/sdes.h"
#include "test.h"


/**
 * Verifies that SDES keys and crypto attributes use canonical Base64 padding.
 *
 * @return 0 on success, otherwise an error code.
 */
int test_sdes_crypto_padding(void)
{
	static const char suite[] = "AEAD_AES_256_GCM";
	static const char key[] =
		"SQPi+bjwvReOgM8RivFt92NcAPLSJ2DizcUgxJzwm0qe9Qv2cYpo5tPC488";
	static const char padded_key[] =
		"SQPi+bjwvReOgM8RivFt92NcAPLSJ2DizcUgxJzwm0qe9Qv2cYpo5tPC488=";
	static const char expected[] =
		"1 AEAD_AES_256_GCM inline:"
		"SQPi+bjwvReOgM8RivFt92NcAPLSJ2DizcUgxJzwm0qe9Qv2cYpo5tPC488=";
	uint8_t master_key[44] = {0};
	char encoded_key[64];
	struct sdp_session *session = NULL;
	struct sdp_media *media = NULL;
	struct sa address;
	const char *attribute;
	size_t encoded_len;
	int err;

	err = sdes_encode_key(encoded_key, sizeof(encoded_key), master_key,
			      sizeof(master_key), &encoded_len);
	TEST_ERR(err);
	ASSERT_EQ(60, encoded_len);
	ASSERT_EQ('=', encoded_key[59]);
	ASSERT_EQ('\0', encoded_key[60]);

	sa_set_str(&address, "127.0.0.1", 0);

	err = sdp_session_alloc(&session, &address);
	TEST_ERR(err);

	err = sdp_media_add(&media, session, sdp_media_audio, 5004,
			    sdp_proto_rtpsavp);
	TEST_ERR(err);

	err = sdes_encode_crypto(media, 1, suite, key, str_len(key));
	TEST_ERR(err);

	attribute = sdp_media_lattr_apply(media, sdp_attr_crypto, NULL, NULL);
	ASSERT_STREQ(expected, attribute);

	err = sdes_encode_crypto(media, 1, suite, padded_key,
				 str_len(padded_key));
	TEST_ERR(err);

	attribute = sdp_media_lattr_apply(media, sdp_attr_crypto, NULL, NULL);
	ASSERT_STREQ(expected, attribute);

out:
	mem_deref(session);
	return err;
}
