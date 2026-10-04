/* SHA-256 on the ESP32's hardware accelerator, driven directly for mining. */
#include "soc/dport_reg.h"
#include "soc/hwcrypto_reg.h"
#include "sha256.h"

#define TEXT ((volatile uint32_t *)SHA_TEXT_BASE)
#define REG(addr) (*(volatile uint32_t *)(addr))

typedef uint32_t (*scan_fn)(volatile uint32_t *text, sha_scan_t *ctx);

/* sha_hw_scan.S */
uint32_t sha_hw_scan_fast(volatile uint32_t *text, sha_scan_t *ctx);
uint32_t sha_hw_scan_safe(volatile uint32_t *text, sha_scan_t *ctx);

static inline void run(uint32_t reg)
{
    REG(reg) = 1;
    while (REG(SHA_256_BUSY_REG)) {
    }
}

/*
 * The same scan with nothing overlapped: fill, trigger, poll. A third of the
 * speed, and no timing to get wrong.
 */
static uint32_t scan_plain(volatile uint32_t *text, sha_scan_t *c)
{
    while (c->count) {
        for (int i = 0; i < 16; i++) text[i] = c->block1[i];
        run(SHA_256_START_REG);

        text[0] = c->tail[0]; text[1] = c->tail[1]; text[2] = c->tail[2]; text[3] = c->w3;
        text[4] = 0x80000000;
        for (int i = 5; i < 15; i++) text[i] = 0;
        text[15] = 640;
        run(SHA_256_CONTINUE_REG);
        run(SHA_256_LOAD_REG);

        /* the first digest is now in TEXT[0..7]; pad it into a block and hash again */
        text[8] = 0x80000000;
        for (int i = 9; i < 15; i++) text[i] = 0;
        text[15] = 256;
        run(SHA_256_START_REG);
        run(SHA_256_LOAD_REG);

        uint32_t low = text[7] & 0xFFFF;
        c->w3++;
        c->count--;
        c->sum += low;
        if (!low) return 1;
    }
    return 0;
}

static const struct {
    scan_fn fn;
    const char *name;
} loops[] = {
    {sha_hw_scan_fast, "fast"},
    {sha_hw_scan_safe, "safe"},
    {scan_plain, "plain"},
};
#define NLOOP ((int)(sizeof(loops) / sizeof(loops[0])))
static int loop;

/*
 * Known-answer test against the software hash. With the header words below,
 * 4096 nonce words from TEST_FIRST hold exactly one candidate, so a loop
 * passes only if it gets every hash right and reports that candidate.
 */
#define TEST_FIRST 0xB000u
#define TEST_COUNT 4096

static bool self_test(scan_fn fn)
{
    static uint32_t ref_sum, ref_hits;
    sha_scan_t c = {.w3 = TEST_FIRST, .count = TEST_COUNT, .pad = 0x80000000};
    uint32_t mid[8], digest[8], hits = 0;

    for (int i = 0; i < 16; i++) c.block1[i] = 0x9E3779B9u * (i + 1);
    for (int i = 0; i < 3; i++) c.tail[i] = 0x9E3779B9u * (i + 17);
    if (!ref_hits) {
        sha256_init(mid);
        sha256_transform(mid, c.block1);
        for (uint32_t i = 0; i < TEST_COUNT; i++) {
            uint32_t low = sha256_mine(mid, c.tail, TEST_FIRST + i, digest) & 0xFFFF;
            ref_sum += low;
            if (!low) ref_hits += i + 1;
        }
    }
    while (fn(TEXT, &c)) hits += c.w3 - TEST_FIRST;
    return c.sum == ref_sum && hits == ref_hits && c.w3 == TEST_FIRST + TEST_COUNT;
}

const char *sha_hw_select(void)
{
    DPORT_SET_PERI_REG_MASK(DPORT_PERI_CLK_EN_REG, DPORT_PERI_EN_SHA);
    DPORT_SET_PERI_REG_MASK(DPORT_PERI_RST_EN_REG, DPORT_PERI_EN_SHA);
    DPORT_CLEAR_PERI_REG_MASK(DPORT_PERI_RST_EN_REG, DPORT_PERI_EN_SHA | DPORT_PERI_EN_SECUREBOOT);

    /* Best of three: one stray wrong hash should not cost a loop for the whole uptime. */
    for (loop = 0; loop < NLOOP; loop++) {
        for (int try = 0; try < 3; try++) {
            if (self_test(loops[loop].fn)) return loops[loop].name;
        }
    }
    return NULL;
}

const char *sha_hw_step_down(void)
{
    if (loop + 1 >= NLOOP) return NULL;
    return loops[++loop].name;
}

bool sha_hw_scan(sha_scan_t *ctx)
{
    return loops[loop].fn(TEXT, ctx);
}
