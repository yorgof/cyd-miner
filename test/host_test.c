/*
 * Host-side check of the hashing and Stratum job code (no ESP32 needed).
 *
 *   host_test                  self-test against the genesis block
 *   host_test mine <diff> <job_id> <prevhash> <coinb1> <extranonce1> <en2_size>
 *             <coinb2> <version> <nbits> <ntime> [branch...]
 *                              finds one share, prints "<en2> <ntime> <nonce> <difficulty>"
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sha256.h"
#include "work.h"

static int self_test(void)
{
    /* genesis block header, split the way the miner sees it */
    static const uint8_t header[80] = {
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x3b, 0xa3, 0xed, 0xfd, 0x7a, 0x7b, 0x12, 0xb2, 0x7a, 0xc7, 0x2c, 0x3e,
        0x67, 0x76, 0x8f, 0x61, 0x7f, 0xc8, 0x1b, 0xc3, 0x88, 0x8a, 0x51, 0x32, 0x3a, 0x9f, 0xb8, 0xaa,
        0x4b, 0x1e, 0x5e, 0x4a, 0x29, 0xab, 0x5f, 0x49, 0xff, 0xff, 0x00, 0x1d, 0x1d, 0xac, 0x2b, 0x7c,
    };
    static const char *expect = "000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f";
    uint8_t out[32];
    char hex[65];
    int fail = 0;

    sha256d(header, 80, out);
    for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", out[31 - i]);
    printf("sha256d      %s %s\n", hex, strcmp(hex, expect) ? "FAIL" : "ok");
    fail |= strcmp(hex, expect) != 0;

    uint32_t blk[16], mid[8], tail[3], digest[8];
    for (int i = 0; i < 16; i++)
        blk[i] = (uint32_t)header[i * 4] << 24 | header[i * 4 + 1] << 16 | header[i * 4 + 2] << 8 | header[i * 4 + 3];
    sha256_init(mid);
    sha256_transform(mid, blk);
    for (int i = 0; i < 3; i++)
        tail[i] = (uint32_t)header[64 + i * 4] << 24 | header[65 + i * 4] << 16 | header[66 + i * 4] << 8 | header[67 + i * 4];
    uint32_t w3 = (uint32_t)header[76] << 24 | header[77] << 16 | header[78] << 8 | header[79];
    sha256_mine(mid, tail, w3, digest);
    for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", (uint8_t)(digest[7 - i / 4] >> (8 * (i % 4))));
    printf("sha256_mine  %s %s\n", hex, strcmp(hex, expect) ? "FAIL" : "ok");
    fail |= strcmp(hex, expect) != 0;

    double d = digest_difficulty(digest);
    int ok = d > 2536.4 && d < 2536.5;
    printf("difficulty   %.3f %s\n", d, ok ? "ok" : "FAIL");
    fail |= !ok;

    ok = __builtin_bswap32(digest[7]) <= difficulty_filter(1.0) && difficulty_filter(0.001) >= 1000 &&
         difficulty_filter(0.001) < 1010;
    printf("filter       %s\n", ok ? "ok" : "FAIL");
    fail |= !ok;
    return fail;
}

int main(int argc, char **argv)
{
    if (argc < 2) return self_test();
    if (strcmp(argv[1], "mine") || argc < 12) {
        fprintf(stderr, "bad arguments\n");
        return 2;
    }

    static job_t job;
    work_t work;
    double diff = atof(argv[2]);
    if (!job_build(&job, argv[3], argv[4], argv[5], argv[6], (size_t)atoi(argv[7]), argv[8],
                   (const char *const *)&argv[12], argc - 12, argv[9], argv[10], argv[11])) {
        fprintf(stderr, "job_build failed\n");
        return 1;
    }
    work_build(&job, 0x0102030405060708ULL, &work);

    uint32_t filter = difficulty_filter(diff), digest[8];
    for (uint32_t w3 = 0;; w3++) {
        if (__builtin_bswap32(sha256_mine(work.midstate, work.tail, w3, digest)) <= filter) {
            double d = digest_difficulty(digest);
            if (d >= diff) {
                printf("%s %s %08x %f\n", work.en2_hex, work.ntime_hex, __builtin_bswap32(w3), d);
                return 0;
            }
        }
        if (w3 == 0xFFFFFFFFu) break;
    }
    return 1;
}
