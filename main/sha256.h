#pragma once
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

/* sha_hw.c (ESP32 only): the same double hash on the hardware SHA engine. */
void sha_hw_init(void);
uint32_t sha_hw_mine(const uint32_t block1[16], const uint32_t tail[3], uint32_t w3, uint32_t digest[8]);
