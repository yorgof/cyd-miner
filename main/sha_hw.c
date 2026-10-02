/* SHA-256 on the ESP32's hardware accelerator, driven directly for mining. */
#include "soc/dport_reg.h"
#include "soc/hwcrypto_reg.h"
#include "sha256.h"

#define TEXT ((volatile uint32_t *)SHA_TEXT_BASE)
#define REG(addr) (*(volatile uint32_t *)(addr))

static inline void run(uint32_t reg)
{
    REG(reg) = 1;
    while (REG(SHA_256_BUSY_REG)) {
    }
}

void sha_hw_init(void)
{
    DPORT_SET_PERI_REG_MASK(DPORT_PERI_CLK_EN_REG, DPORT_PERI_EN_SHA);
    DPORT_SET_PERI_REG_MASK(DPORT_PERI_RST_EN_REG, DPORT_PERI_EN_SHA);
    DPORT_CLEAR_PERI_REG_MASK(DPORT_PERI_RST_EN_REG, DPORT_PERI_EN_SHA | DPORT_PERI_EN_SECUREBOOT);
}

/*
 * Same result as sha256_mine. This engine cannot be loaded with a midstate,
 * so the first header block is hashed again for every nonce.
 */
uint32_t sha_hw_mine(const uint32_t block1[16], const uint32_t tail[3], uint32_t w3, uint32_t digest[8])
{
    for (int i = 0; i < 16; i++) TEXT[i] = block1[i];
    run(SHA_256_START_REG);

    TEXT[0] = tail[0]; TEXT[1] = tail[1]; TEXT[2] = tail[2]; TEXT[3] = w3;
    TEXT[4] = 0x80000000;
    for (int i = 5; i < 15; i++) TEXT[i] = 0;
    TEXT[15] = 640;
    run(SHA_256_CONTINUE_REG);
    run(SHA_256_LOAD_REG);

    /* the first digest is now in TEXT[0..7]; pad it into a block and hash again */
    TEXT[8] = 0x80000000;
    for (int i = 9; i < 15; i++) TEXT[i] = 0;
    TEXT[15] = 256;
    run(SHA_256_START_REG);
    run(SHA_256_LOAD_REG);

    for (int i = 0; i < 8; i++) digest[i] = TEXT[i];
    return digest[7];
}
