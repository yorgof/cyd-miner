#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define JOB_ID_MAX 32
#define COINBASE_MAX 1024
#define MERKLE_MAX 20

/* A Stratum mining.notify job, decoded and ready to turn into block headers. */
typedef struct {
    char job_id[JOB_ID_MAX];
    char ntime_hex[9];
    uint8_t header[80]; /* everything but merkle root and nonce filled in */
    uint8_t coinbase[COINBASE_MAX];
    size_t coinbase_len, en2_off, en2_size;
    uint8_t branch[MERKLE_MAX][32];
    int n_branch;
} job_t;

/* One header template: all that the hashing loop and a share submit need. */
typedef struct {
    char job_id[JOB_ID_MAX];
    char ntime_hex[9];
    char en2_hex[17];
    uint32_t block1[16]; /* first 64 header bytes as big-endian words */
    uint32_t midstate[8]; /* SHA-256 state after block1 */
    uint32_t tail[3];
} work_t;

bool job_build(job_t *job, const char *job_id, const char *prevhash, const char *coinb1,
               const char *extranonce1, size_t en2_size, const char *coinb2,
               const char *const *branches, int n_branch,
               const char *version, const char *nbits, const char *ntime);
void work_build(const job_t *job, uint64_t extranonce2, work_t *work);

/* Difficulty of a hash given as the digest words from sha256_mine. */
double digest_difficulty(const uint32_t digest[8]);

/* Largest value of bswap32(digest[7]) that can still meet the difficulty. */
uint32_t difficulty_filter(double difficulty);
