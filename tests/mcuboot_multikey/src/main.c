/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * ztest for MCUboot multi-key support (test builds only).
 *
 * Exercises the real bootutil key lookup (bootutil_find_key) and ECDSA P-256
 * signature verification (bootutil_verify_sig) against the generated
 * bootutil_keys[] table, which holds the root key plus the two test keys.
 *
 * Also checks an image produced by the external-signer flow in
 * gen_roundtrip.sh (imgtool --vector-to-sign digest, openssl signing the
 * digest, imgtool --fix-sig/--fix-sig-pubkey), by doing what MCUboot does at
 * boot: hash the protected region, match the SHA-256 TLV, select the key by
 * KEYHASH, and verify the signature TLV.
 */

#include <zephyr/ztest.h>
#include <zephyr/logging/log.h>

#include <bootutil/sign_key.h>
#include <bootutil/crypto/sha.h>
#include <bootutil/fault_injection_hardening.h>

#include "vectors.h"
#include "roundtrip_image.h"

LOG_MODULE_REGISTER(mcuboot, LOG_LEVEL_WRN);

/* Key IDs follow the table order: primary (root) first, then the extras. */
#define KEY_ID_ROOT  0
#define KEY_ID_TEST_A 1
#define KEY_ID_TEST_B 2

#define HASH_LEN 32

/* From bootutil/src/bootutil_find_key.c (not declared in a public header). */
int bootutil_find_key(uint8_t *keyhash, uint8_t keyhash_len);

/* From bootutil/src/image_rsa.c. */
fih_ret bootutil_verify_sig(uint8_t *hash, uint32_t hlen, uint8_t *sig,
			    size_t slen, uint8_t key_id);

static void sha256_of_key(int idx, uint8_t out[HASH_LEN])
{
	bootutil_sha_context ctx;

	bootutil_sha_init(&ctx);
	bootutil_sha_update(&ctx, bootutil_keys[idx].key, *bootutil_keys[idx].len);
	bootutil_sha_finish(&ctx, out);
	bootutil_sha_drop(&ctx);
}

static bool verify_ok(const uint8_t *sig, size_t sig_len, uint8_t key_id)
{
	/* bootutil_verify_sig takes a non-const hash/sig pointer. */
	uint8_t digest[HASH_LEN];
	uint8_t sig_copy[256];
	fih_ret rc;

	memcpy(digest, vec_digest, sizeof(digest));
	memcpy(sig_copy, sig, sig_len);

	rc = bootutil_verify_sig(digest, sizeof(digest), sig_copy, sig_len, key_id);
	return rc == FIH_SUCCESS;
}

/* MCUboot image format constants (boot/bootutil/include/bootutil/image.h). */
#define IMG_HDR_MAGIC   0x96f3b83dU
#define TLV_INFO_MAGIC  0x6907U
#define TLV_KEYHASH     0x01U
#define TLV_SHA256      0x10U
#define TLV_ECDSA_SIG   0x22U
#define IMG_PAYLOAD_OFF 0x200U

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

/*
 * Find the TLV area as MCUboot does: header size, image size and protected
 * TLV size give the offset of the TLV info block. The bytes before it are the
 * protected region that the image hash covers.
 */
static bool rt_locate(const uint8_t *img, size_t len, uint32_t *tlv_off,
		      uint16_t *tlv_total)
{
	uint32_t hdr, prot, img_sz, off;
	uint16_t total;

	if (len < 16 || rd32(img) != IMG_HDR_MAGIC) {
		return false;
	}
	hdr = rd16(img + 8);
	prot = rd16(img + 10);
	img_sz = rd32(img + 12);
	off = hdr + img_sz + prot;
	if (off + 4 > len || rd16(img + off) != TLV_INFO_MAGIC) {
		return false;
	}
	total = rd16(img + off + 2);
	if (off + total > len) {
		return false;
	}
	*tlv_off = off;
	*tlv_total = total;
	return true;
}

static bool rt_find(const uint8_t *img, uint32_t tlv_off, uint16_t tlv_total,
		    uint16_t type, const uint8_t **val, uint16_t *vlen)
{
	uint16_t pos = 4;

	while ((uint32_t)pos + 4 <= tlv_total) {
		uint16_t t = rd16(img + tlv_off + pos);
		uint16_t l = rd16(img + tlv_off + pos + 2);

		if ((uint32_t)pos + 4 + l > tlv_total) {
			return false;
		}
		if (t == type) {
			*val = img + tlv_off + pos + 4;
			*vlen = l;
			return true;
		}
		pos += 4 + l;
	}
	return false;
}

/* Returns the key index that accepts the image, or -1. */
static int rt_verify(uint8_t *img, size_t len)
{
	uint32_t tlv_off;
	uint16_t tlv_total, hash_l, kh_l, sig_l;
	const uint8_t *hash_v, *kh_v, *sig_v;
	uint8_t digest[HASH_LEN];
	bootutil_sha_context ctx;
	fih_ret rc;
	int key_id;

	if (!rt_locate(img, len, &tlv_off, &tlv_total)) {
		return -1;
	}

	/* 1. Hash the protected region: header, payload, protected TLVs. */
	bootutil_sha_init(&ctx);
	bootutil_sha_update(&ctx, img, tlv_off);
	bootutil_sha_finish(&ctx, digest);
	bootutil_sha_drop(&ctx);

	/* 2. The SHA-256 TLV must match the computed digest. */
	if (!rt_find(img, tlv_off, tlv_total, TLV_SHA256, &hash_v, &hash_l) ||
	    hash_l != HASH_LEN || memcmp(hash_v, digest, HASH_LEN) != 0) {
		return -1;
	}

	/* 3. The key-hash TLV selects an embedded key. */
	if (!rt_find(img, tlv_off, tlv_total, TLV_KEYHASH, &kh_v, &kh_l) ||
	    kh_l != HASH_LEN) {
		return -1;
	}
	key_id = bootutil_find_key((uint8_t *)kh_v, kh_l);
	if (key_id < 0) {
		return -1;
	}

	/* 4. The signature TLV must verify the digest under that key. */
	if (!rt_find(img, tlv_off, tlv_total, TLV_ECDSA_SIG, &sig_v, &sig_l)) {
		return -1;
	}
	rc = bootutil_verify_sig(digest, HASH_LEN, (uint8_t *)sig_v, sig_l,
				 (uint8_t)key_id);
	return rc == FIH_SUCCESS ? key_id : -1;
}

ZTEST_SUITE(mcuboot_multikey, NULL, NULL, NULL, NULL, NULL);

ZTEST(mcuboot_multikey, test_key_count_is_three)
{
	zassert_equal(bootutil_key_cnt, 3, "expected root + 2 test keys, got %d",
		      bootutil_key_cnt);
}

ZTEST(mcuboot_multikey, test_each_embedded_key_is_found_by_hash)
{
	for (int i = 0; i < bootutil_key_cnt; i++) {
		uint8_t hash[HASH_LEN];

		sha256_of_key(i, hash);
		zassert_equal(bootutil_find_key(hash, sizeof(hash)), i,
			      "key %d not found by its own hash", i);
	}
}

ZTEST(mcuboot_multikey, test_unknown_key_hash_is_rejected)
{
	uint8_t hash[HASH_LEN];

	memset(hash, 0xA5, sizeof(hash));
	zassert_equal(bootutil_find_key(hash, sizeof(hash)), -1,
		      "unknown key hash was matched");
}

ZTEST(mcuboot_multikey, test_signature_by_test_key_a_accepted_under_key_a)
{
	zassert_true(verify_ok(vec_sig_a, sizeof(vec_sig_a), KEY_ID_TEST_A),
		     "test-key-a signature rejected under test-key-a");
}

ZTEST(mcuboot_multikey, test_signature_by_test_key_b_accepted_under_key_b)
{
	zassert_true(verify_ok(vec_sig_b, sizeof(vec_sig_b), KEY_ID_TEST_B),
		     "test-key-b signature rejected under test-key-b");
}

ZTEST(mcuboot_multikey, test_signature_by_test_key_b_rejected_under_root)
{
	zassert_false(verify_ok(vec_sig_b, sizeof(vec_sig_b), KEY_ID_ROOT),
		      "test-key-b signature accepted under root key");
}

ZTEST(mcuboot_multikey, test_signature_by_test_key_b_rejected_under_key_a)
{
	zassert_false(verify_ok(vec_sig_b, sizeof(vec_sig_b), KEY_ID_TEST_A),
		      "test-key-b signature accepted under test-key-a");
}

ZTEST(mcuboot_multikey, test_signature_by_test_key_a_rejected_under_key_b)
{
	zassert_false(verify_ok(vec_sig_a, sizeof(vec_sig_a), KEY_ID_TEST_B),
		      "test-key-a signature accepted under test-key-b");
}

ZTEST(mcuboot_multikey, test_roundtrip_image_verifies_under_key_b)
{
	uint8_t img[sizeof(roundtrip_image)];

	memcpy(img, roundtrip_image, sizeof(img));
	zassert_equal(rt_verify(img, sizeof(img)), KEY_ID_TEST_B,
		      "external-signer image not accepted under test-key-b");
}

ZTEST(mcuboot_multikey, test_roundtrip_tampered_payload_is_rejected)
{
	uint8_t img[sizeof(roundtrip_image)];

	memcpy(img, roundtrip_image, sizeof(img));
	img[IMG_PAYLOAD_OFF + 100] ^= 0x01;
	zassert_equal(rt_verify(img, sizeof(img)), -1,
		      "image with a modified payload was accepted");
}

ZTEST(mcuboot_multikey, test_roundtrip_tampered_signature_is_rejected)
{
	uint8_t img[sizeof(roundtrip_image)];
	uint32_t tlv_off;
	uint16_t tlv_total, sig_l;
	const uint8_t *sig_v;

	memcpy(img, roundtrip_image, sizeof(img));
	zassert_true(rt_locate(img, sizeof(img), &tlv_off, &tlv_total),
		     "round-trip image has no TLV area");
	zassert_true(rt_find(img, tlv_off, tlv_total, TLV_ECDSA_SIG, &sig_v, &sig_l),
		     "round-trip image has no signature TLV");
	img[sig_v - img + sig_l / 2] ^= 0x01;
	zassert_equal(rt_verify(img, sizeof(img)), -1,
		      "image with a modified signature was accepted");
}
