#include "sha256.h"
#include <string.h>

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static const uint32_t H0[8] = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

void sha256_init(uint32_t state[8])
{
    memcpy(state, H0, sizeof(H0));
}

void sha256_transform(uint32_t s[8], const uint32_t block[16])
{
    uint32_t w[16];
    uint32_t a = s[0], b = s[1], c = s[2], d = s[3], e = s[4], f = s[5], g = s[6], h = s[7];

    for (int i = 0; i < 64; i++) {
        uint32_t wi;
        if (i < 16) {
            wi = w[i] = block[i];
        } else {
            uint32_t x = w[(i + 1) & 15], y = w[(i + 14) & 15];
            uint32_t s0 = ROTR(x, 7) ^ ROTR(x, 18) ^ (x >> 3);
            uint32_t s1 = ROTR(y, 17) ^ ROTR(y, 19) ^ (y >> 10);
            wi = w[i & 15] += s0 + s1 + w[(i + 9) & 15];
        }
        uint32_t t1 = h + (ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25)) + (g ^ (e & (f ^ g))) + K[i] + wi;
        uint32_t t2 = (ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22)) + ((a & b) | (c & (a | b)));
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    s[0] += a; s[1] += b; s[2] += c; s[3] += d;
    s[4] += e; s[5] += f; s[6] += g; s[7] += h;
}

static uint32_t load_be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

void sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    uint32_t s[8], blk[16];
    uint8_t last[128] = {0};
    size_t full = len / 64, rem = len % 64;

    sha256_init(s);
    for (size_t n = 0; n < full; n++) {
        for (int i = 0; i < 16; i++) blk[i] = load_be(data + n * 64 + i * 4);
        sha256_transform(s, blk);
    }

    /* padding: 0x80, zeros, 64-bit bit length */
    memcpy(last, data + full * 64, rem);
    last[rem] = 0x80;
    size_t padded = rem < 56 ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) last[padded - 1 - i] = (uint8_t)(bits >> (8 * i));
    for (size_t n = 0; n < padded / 64; n++) {
        for (int i = 0; i < 16; i++) blk[i] = load_be(last + n * 64 + i * 4);
        sha256_transform(s, blk);
    }

    for (int i = 0; i < 8; i++) {
        out[i * 4] = s[i] >> 24;
        out[i * 4 + 1] = s[i] >> 16;
        out[i * 4 + 2] = s[i] >> 8;
        out[i * 4 + 3] = s[i];
    }
}

void sha256d(const uint8_t *data, size_t len, uint8_t out[32])
{
    uint8_t tmp[32];
    sha256(data, len, tmp);
    sha256(tmp, 32, out);
}

uint32_t sha256_mine(const uint32_t midstate[8], const uint32_t tail[3], uint32_t w3, uint32_t digest[8])
{
    uint32_t blk[16], s[8];

    /* second block of the header: 12 constant bytes, nonce, padding, length 640 bits */
    memcpy(s, midstate, sizeof(s));
    blk[0] = tail[0]; blk[1] = tail[1]; blk[2] = tail[2]; blk[3] = w3;
    blk[4] = 0x80000000;
    memset(&blk[5], 0, 10 * sizeof(uint32_t));
    blk[15] = 640;
    sha256_transform(s, blk);

    /* hash of the 32-byte first digest */
    memcpy(blk, s, sizeof(s));
    blk[8] = 0x80000000;
    memset(&blk[9], 0, 6 * sizeof(uint32_t));
    blk[15] = 256;
    memcpy(digest, H0, sizeof(H0));
    sha256_transform(digest, blk);
    return digest[7];
}
