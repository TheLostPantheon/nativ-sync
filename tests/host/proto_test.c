/* the shared protocol code on a computer: cc -O2 -Icommon -Ithird_party/monocypher tests/host/proto_test.c common/nsync_proto.c third_party/monocypher/monocypher.c -o build/proto_test */
#include <stdio.h>
#include <string.h>
#include "nsync_proto.h"
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
static void fill(uint8_t *p, size_t n, int seed) { for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(seed * 31 + i * 7 + 1); }
/* one pairing run; returns 1 when both tags check */
static int pair(const char *pc_code, const char *vita_code, uint8_t pk_pc[32], uint8_t pk_vita[32]) {
	uint8_t pcn[16], vn[16], r1[32], r2[32], cf1[32], cf2[32], t1[96], t2[96], tag_v[32], tag_v_expect[32], tag_p[32], tag_p_expect[32];
	fill(pcn, 16, 1); fill(vn, 16, 2); fill(r1, 32, 3); fill(r2, 32, 4);
	NsyncPair pc, vita;
	nsync_pair_start(&pc, pcn, vn, pc_code, r1); nsync_pair_start(&vita, pcn, vn, vita_code, r2);
	uint8_t a[32], b[32]; memcpy(a, pc.mine, 32); memcpy(b, vita.mine, 32);
	CHECK(nsync_pair_finish(&pc, 0, b, pk_pc, cf1, t1) == 0);
	CHECK(nsync_pair_finish(&vita, 1, a, pk_vita, cf2, t2) == 0);
	CHECK(!memcmp(t1, t2, 96));
	nsync_pair_tag(tag_v, cf2, "vita", t2); nsync_pair_tag(tag_v_expect, cf1, "vita", t1);
	nsync_pair_tag(tag_p, cf1, "pc", t1); nsync_pair_tag(tag_p_expect, cf2, "pc", t2);
	return !memcmp(tag_v, tag_v_expect, 32) && !memcmp(tag_p, tag_p_expect, 32);
}
int main(void) {
	uint8_t k1[32], k2[32];
	CHECK(pair("482913", "482913", k1, k2) == 1); CHECK(!memcmp(k1, k2, 32));
	CHECK(pair("482913", "482914", k1, k2) == 0); CHECK(memcmp(k1, k2, 32));
	uint8_t zero[32] = {0}, pk[32], cf[32], tr[96], n[16] = {0}; NsyncPair p; nsync_pair_start(&p, n, n, "000000", k1);
	CHECK(nsync_pair_finish(&p, 0, zero, pk, cf, tr) == -1);   /* a low-order point is refused */

	pair("111111", "111111", k1, k2);
	uint8_t pcn[16], vn[16]; fill(pcn, 16, 5); fill(vn, 16, 6);
	NsyncSession pc, vita; nsync_session(&pc, k1, pcn, vn, 0); nsync_session(&vita, k2, pcn, vn, 1);
	static uint8_t frame[64 + 20], frame2[64 + 20], plain[64];
	const char *msg = "HELLO Desk PC";
	size_t fl = nsync_seal(&pc, frame, (const uint8_t *)msg, strlen(msg));
	CHECK(fl == strlen(msg) + 20 && nsync_frame_length(frame) == strlen(msg) + 16);
	CHECK(nsync_open(&vita, plain, frame + 4, fl - 4) == (long)strlen(msg) && !memcmp(plain, msg, strlen(msg)));
	CHECK(nsync_open(&vita, plain, frame + 4, fl - 4) == -1);             /* a replayed frame doesn't open */
	size_t f2 = nsync_seal(&pc, frame2, (const uint8_t *)"LIST", 4);
	frame2[10] ^= 1; CHECK(nsync_open(&vita, plain, frame2 + 4, f2 - 4) == -1); frame2[10] ^= 1;   /* tampered */
	CHECK(nsync_open(&vita, plain, frame2 + 4, f2 - 4) == 4);
	size_t f3 = nsync_seal(&vita, frame, (const uint8_t *)"OK", 2);      /* the other direction has its own key */
	CHECK(nsync_open(&vita, plain, frame + 4, f3 - 4) == -1 && nsync_open(&pc, plain, frame + 4, f3 - 4) == 2);

	uint8_t h1[32], h2[32], fh[32]; NsyncHash h;
	nsync_hash_init(&h); nsync_hash_final(&h, h1);
	crypto_blake2b(h2, 32, (const uint8_t *)"", 0); CHECK(!memcmp(h1, h2, 32));   /* the empty save */
	fill(fh, 32, 9);
	nsync_hash_init(&h); nsync_manifest_file(&h, "saveData.bin", 86188, fh); nsync_manifest_slot(&h, 0, fh); nsync_hash_final(&h, h1);
	char hex[65]; nsync_hex(hex, fh, 32); char text[256];
	int tl = snprintf(text, sizeof text, "F saveData.bin\t86188\t%s\nS 0\t%s\n", hex, hex);
	crypto_blake2b(h2, 32, (const uint8_t *)text, (size_t)tl); CHECK(!memcmp(h1, h2, 32));
	uint8_t back[32]; CHECK(nsync_unhex(back, 32, hex) == 0 && !memcmp(back, fh, 32)); CHECK(nsync_unhex(back, 32, "zz") == -1);
	printf(fails ? "%d failed\n" : "all passed\n", fails);
	return fails != 0;
}
