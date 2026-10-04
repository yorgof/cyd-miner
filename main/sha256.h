#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One SHA-256 compression round over a 16-word big-endian block. */
void sha256_transform(uint32_t state[8], const uint32_t block[16]);
void sha256_init(uint32_t state[8]);
void sha256(const uint8_t *data, size_t len, uint8_t out[32]);
void sha256d(const uint8_t *data, size_t len, uint8_t out[32]);

/*
 * Double SHA-256 of an 80-byte block header, given the midstate of its first
 * 64 bytes, the three remaining constant words and the nonce word (w3).
 * Writes the final digest words and returns the last one, which holds the
 * most significant bytes of the hash as a number.
 */
uint32_t sha256_mine(const uint32_t midstate[8], const uint32_t tail[3], uint32_t w3, uint32_t digest[8]);

/*
 * sha_hw_scan.S: the mining loop on the hardware engine. The layout is known
 * to the assembly.
 */
typedef struct {
    uint32_t block1[16];
    uint32_t tail[3];
    uint32_t w3;    /* next nonce word to hash */
    uint32_t count; /* nonce words left */
    uint32_t sum;   /* running sum of the 16 digest bits looked at per nonce */
    uint32_t pad;   /* 0x80000000 */
} sha_scan_t;

/*
 * sha_hw.c (ESP32 only). sha_hw_select tests the hardware loops against
 * software and returns the name of the fastest that works, NULL if none does;
 * sha_hw_step_down moves to the next slower one, NULL if there is none left.
 *
 * sha_hw_scan hashes scan->count nonce words from scan->w3 on and returns true
 * as soon as the last digest word of one has its low 16 bits clear: a
 * candidate at scan->w3 - 1, to be hashed again in software. False once count
 * is used up.
 */
const char *sha_hw_select(void);
const char *sha_hw_step_down(void);
bool sha_hw_scan(sha_scan_t *scan);
