#include "work.h"
#include "sha256.h"
#include <stdio.h>
#include <string.h>

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Returns the number of bytes decoded, or -1 on bad hex or overflow. */
static int hex_decode(const char *hex, uint8_t *out, size_t max)
{
    size_t len = strlen(hex);
    if (len % 2 || len / 2 > max) return -1;
    for (size_t i = 0; i < len / 2; i++) {
        int hi = hex_nibble(hex[i * 2]), lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return (int)(len / 2);
}

static uint32_t load_be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* Decodes 8 hex chars of a big-endian value into little-endian header bytes. */
static bool hex_word_le(const char *hex, uint8_t out[4])
{
    uint8_t be[4];
    if (strlen(hex) != 8 || hex_decode(hex, be, 4) != 4) return false;
    out[0] = be[3]; out[1] = be[2]; out[2] = be[1]; out[3] = be[0];
    return true;
}

bool job_build(job_t *job, const char *job_id, const char *prevhash, const char *coinb1,
               const char *extranonce1, size_t en2_size, const char *coinb2,
               const char *const *branches, int n_branch,
               const char *version, const char *nbits, const char *ntime)
{
    uint8_t prev[32];

    if (strlen(job_id) >= JOB_ID_MAX || en2_size == 0 || en2_size > 8 || n_branch > MERKLE_MAX) return false;
    if (strlen(prevhash) != 64 || hex_decode(prevhash, prev, 32) != 32) return false;

    memset(job, 0, sizeof(*job));
    strcpy(job->job_id, job_id);

    if (!hex_word_le(version, job->header)) return false;
    /* Stratum sends prevhash as eight 4-byte words, each byte-swapped. */
    for (int i = 0; i < 32; i++) job->header[4 + i] = prev[(i & ~3) + 3 - (i & 3)];
    if (!hex_word_le(ntime, job->header + 68)) return false;
    if (!hex_word_le(nbits, job->header + 72)) return false;
    strcpy(job->ntime_hex, ntime);

    size_t off = 0;
    int n = hex_decode(coinb1, job->coinbase, COINBASE_MAX);
    if (n < 0) return false;
    off += n;
    n = hex_decode(extranonce1, job->coinbase + off, COINBASE_MAX - off);
    if (n < 0) return false;
    off += n;
    if (off + en2_size > COINBASE_MAX) return false;
    job->en2_off = off;
    job->en2_size = en2_size;
    off += en2_size;
    n = hex_decode(coinb2, job->coinbase + off, COINBASE_MAX - off);
    if (n < 0) return false;
    job->coinbase_len = off + n;

    for (int i = 0; i < n_branch; i++) {
        if (strlen(branches[i]) != 64 || hex_decode(branches[i], job->branch[i], 32) != 32) return false;
    }
    job->n_branch = n_branch;
    return true;
}

void work_build(const job_t *job, uint64_t extranonce2, work_t *work)
{
    uint8_t coinbase[COINBASE_MAX], header[80], cat[64];

    memcpy(coinbase, job->coinbase, job->coinbase_len);
    for (size_t i = 0; i < job->en2_size; i++) {
        uint8_t b = (uint8_t)(extranonce2 >> (8 * i));
        coinbase[job->en2_off + i] = b;
        sprintf(work->en2_hex + i * 2, "%02x", b);
    }

    /* merkle root: fold the coinbase hash with each branch */
    sha256d(coinbase, job->coinbase_len, cat);
    for (int i = 0; i < job->n_branch; i++) {
        memcpy(cat + 32, job->branch[i], 32);
        sha256d(cat, 64, cat);
    }

    memcpy(header, job->header, 80);
    memcpy(header + 36, cat, 32);

    for (int i = 0; i < 16; i++) work->block1[i] = load_be(header + i * 4);
    sha256_init(work->midstate);
    sha256_transform(work->midstate, work->block1);
    for (int i = 0; i < 3; i++) work->tail[i] = load_be(header + 64 + i * 4);

    strcpy(work->job_id, job->job_id);
    strcpy(work->ntime_hex, job->ntime_hex);
}

double digest_difficulty(const uint32_t digest[8])
{
    /* The hash is the digest bytes read as a little-endian 256-bit number. */
    double v = 0;
    for (int i = 7; i >= 0; i--) v = v * 4294967296.0 + (double)__builtin_bswap32(digest[i]);
    if (v == 0) return 1e300;
    /* difficulty 1 target is 0xFFFF * 2^208 */
    return 65535.0 * 0x1p208 / v;
}

uint32_t difficulty_filter(double difficulty)
{
    /* top 32 bits of the target are 0xFFFF / 65536 / difficulty */
    double top = 1.0 / difficulty + 1.0;
    return top >= 4294967295.0 ? 0xFFFFFFFFu : (uint32_t)top;
}
