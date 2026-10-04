/*
 * The mining loop on the ESP32's hardware SHA engine, with the CPU's register
 * writes overlapped with the engine's compute time.
 *
 * The engine cannot be loaded with a midstate, so one nonce is three blocks and
 * two digest loads:
 *
 *   START    block 1 = header words 0..15
 *   CONTINUE block 2 = header words 16..18, nonce, padding, length 640
 *   LOAD     first digest -> TEXT[0..7]
 *   START    block 3 = first digest, padding, length 256
 *   LOAD     second digest -> TEXT[0..7]
 *
 * The engine reads TEXT[i] about i cycles after a trigger and not again, so the
 * next block is written while the current one is computed:
 *
 *   W1 (block 1 computing)  all of block 2
 *   W2 (block 2 computing)  block 3 padding and length, TEXT[8] and TEXT[15]
 *   W3 (block 3 computing)  header words 8..15 for the next nonce
 *   tail (the second LOAD)  header words 0..7, which a LOAD overwrites
 *
 * There is no BUSY polling: a poll costs more cycles than it saves. The waits
 * are counted, in 240 MHz CPU cycles, from thresholds measured on an
 * ESP32-D0WD-V3 with the bench (bench.c), which checks every hash against
 * software. What it found:
 *
 *   - a block keeps the engine busy for 64 cycles, 65 before a LOAD;
 *   - a LOAD keeps it busy for 11. It writes TEXT[i] 3 + i cycles after its
 *     trigger, and a CPU store in the cycle it writes TEXT[7] loses that word;
 *   - TEXT[7] reads back correctly from 15 cycles after the LOAD;
 *   - traffic from the other core on the bus the SHA registers sit on (the AES
 *     and big-number engines, DPORT registers) holds a store up for a cycle or
 *     more, and loses one that comes less than three cycles after another.
 *
 * So a wait can only be a minimum if the loop is to survive that traffic.
 * sha_hw_scan_safe is built that way and came through every stress mode of
 * the bench without a wrong hash. sha_hw_scan_fast is 10% quicker: it spaces
 * stores two cycles apart, writes two header words while the LOAD is still
 * running and two more after the START, ahead of the engine's read pointer.
 * Next to this firmware it gets fewer than one hash in 2500 wrong, but under
 * that bus traffic most of them. That is why mbedTLS is kept off the hardware
 * AES and big-number engines (sdkconfig.defaults), and why miner.c drops to
 * the safe loop when candidates stop checking out.
 *
 * uint32_t fn(volatile uint32_t *text, sha_scan_t *ctx)
 *   Hashes ctx->count nonce words starting at ctx->w3. Returns 1 when the low
 *   16 bits of the last digest word were zero (the candidate is ctx->w3 - 1;
 *   the digest itself is not kept), 0 when count ran out. w3, count and sum
 *   are updated; sum grows by the 16 bits looked at for every nonce, which is
 *   what the self-test compares.
 */

#define CTX_TAIL  64
#define CTX_W3    76
#define CTX_COUNT 80
#define CTX_SUM   84
#define CTX_PAD   88

#define R_START 0x90
#define R_CONT  0x94
#define R_LOAD  0x98

    .macro NOPS n
    .rept \n
    nop.n
    .endr
    .endm

    /* longer than any engine operation */
    .macro SETTLE
    movi.n  a7, 30
1:  addi.n  a7, a7, -1
    bnez    a7, 1b
    .endm

    /* TEXT[\dst / 4] = ctx word at \src, then \g cycles before the next store */
    .macro COPY src, dst, g
    l32i    a6, a3, \src
    s32i.n  a6, a2, \dst
    NOPS    \g
    .endm

    /*
     * One instruction, one cycle, per character:
     *   n     nop
     *   0..7  TEXT[i] = header word i
     *   s     sample the low 16 bits of TEXT[7]
     *   S, L  START, LOAD
     */
    .macro OPS seq
    .irpc c,\seq
    .ifc \c,n
    nop.n
    .endif
    .ifc \c,S
    s32i    a7, a2, R_START
    .endif
    .ifc \c,L
    s32i    a7, a2, R_LOAD
    .endif
    .ifc \c,s
    l16ui   a6, a2, 28
    .endif
    .ifc \c,0
    s32i.n  a8, a2, 0
    .endif
    .ifc \c,1
    s32i.n  a9, a2, 4
    .endif
    .ifc \c,2
    s32i.n  a10, a2, 8
    .endif
    .ifc \c,3
    s32i.n  a11, a2, 12
    .endif
    .ifc \c,4
    s32i.n  a12, a2, 16
    .endif
    .ifc \c,5
    s32i.n  a13, a2, 20
    .endif
    .ifc \c,6
    s32i.n  a14, a2, 24
    .endif
    .ifc \c,7
    s32i.n  a15, a2, 28
    .endif
    .endr
    .endm

/*
 * Knobs, in cycles of nop, with the smallest value that hashed correctly on a
 * quiet bus:
 *   g     gap after each store in W1..W3
 *   p1    end of the block 2 fill -> CONTINUE (10 with the safe tail, 9 with
 *         the fast one, which goes on after its START)
 *   d2    CONTINUE -> TEXT[8], TEXT[15] rewritten for block 3 (11)
 *   p2    then -> LOAD (d2 + p2 = 55)
 *   pl1   LOAD -> START of block 3 (9)
 *   d3    START -> TEXT[8..15] rewritten for the next block 1 (7)
 *   p3    then -> the tail (d3 + p3 = 40)
 *   tail  OPS string: the second LOAD, the sample, header words 0..7 and the
 *         START of the next block 1
 * d2 and d3 cost nothing, p2 and p3 make up for them. Bus traffic moves the
 * thresholds by one cycle. Any change has to go back through the bench.
 *
 * Registers: a2 TEXT, a3 ctx, a4 sum, a5 count, a6 scratch and the sample,
 * a7 the constant 1, a8..a15 header words 0..7.
 */
    .macro SCAN name, g, p1, d2, p2, pl1, d3, p3, tail
    .section .iram1.\name, "ax"
    .global \name
    .type   \name, @function
    .balign 4
\name:
    entry   a1, 32
    l32i    a4, a3, CTX_SUM
    l32i    a5, a3, CTX_COUNT
    l32i    a8, a3, 0
    l32i    a9, a3, 4
    l32i    a10, a3, 8
    l32i    a11, a3, 12
    l32i    a12, a3, 16
    l32i    a13, a3, 20
    l32i    a14, a3, 24
    l32i    a15, a3, 28
    addi.n  a4, a4, -1          /* the first pass below adds 1 */
    SETTLE
    movi.n  a7, 1
    /* cold start: all of block 1 */
    OPS     0nn1nn2nn3nn4nn5nn6nn7nn
    COPY    32, 32, 1
    COPY    36, 36, 1
    COPY    40, 40, 1
    COPY    44, 44, 1
    COPY    48, 48, 1
    COPY    52, 52, 1
    COPY    56, 56, 1
    COPY    60, 60, 1
    movi.n  a6, 1               /* "the previous nonce was not a candidate" */
    s32i    a7, a2, R_START
    j       .Lhead_\name

    .balign 4
.Lw2b_\name:
    NOPS    \p2
    s32i    a7, a2, R_LOAD      /* first digest -> TEXT[0..7] */
    NOPS    \pl1
    s32i    a7, a2, R_START     /* block 3 */

    /* W3: header words 8..15 of the next nonce */
    NOPS    \d3
    COPY    32, 32, \g
    COPY    36, 36, \g
    COPY    40, 40, \g
    COPY    44, 44, \g
    COPY    48, 48, \g
    COPY    52, 52, \g
    COPY    56, 56, \g
    COPY    60, 60, \g
    NOPS    \p3
    OPS     \tail

.Lhead_\name:
    /* W1: block 1 of the next nonce is running; settle the one that finished */
    add.n   a4, a4, a6
    beqz    a6, .Lhit_\name
    beqz    a5, .Lstop_\name
    addi.n  a5, a5, -1

    /* block 2, behind the engine's read pointer */
    COPY    CTX_TAIL, 0, \g
    COPY    CTX_TAIL + 4, 4, \g
    COPY    CTX_TAIL + 8, 8, \g
    l32i    a6, a3, CTX_W3
    s32i.n  a6, a2, 12
    addi.n  a6, a6, 1
    s32i    a6, a3, CTX_W3
    COPY    CTX_PAD, 16, \g
    movi.n  a6, 0
    .irp off,20,24,28,32,36,40,44,48,52,56
    s32i.n  a6, a2, \off
    nop.n
    NOPS    \g
    .endr
    movi    a6, 640
    s32i.n  a6, a2, 60
    NOPS    \p1
    s32i    a7, a2, R_CONT

    /* W2: block 3 padding and length; TEXT[9..14] are still zero */
    NOPS    \d2
    COPY    CTX_PAD, 32, \g
    movi    a6, 256
    s32i.n  a6, a2, 60
    j       .Lw2b_\name

.Lhit_\name:
    movi.n  a6, 1
    j       .Lout_\name
.Lstop_\name:
    movi.n  a6, 0
.Lout_\name:
    /* a block 1 was started and is abandoned; leave the engine idle */
    SETTLE
    s32i    a4, a3, CTX_SUM
    s32i    a5, a3, CTX_COUNT
    mov.n   a2, a6
    retw.n
    .size   \name, . - \name
    .endm

    /*
     * 230 cycles a nonce, 1.04 MH/s. A cycle less on p1 and every hash is
     * wrong; a cycle less on p2, pl1 or p3 and several times as many are.
     */
    SCAN sha_hw_scan_fast, 1, 9, 14, 42, 10, 11, 30, Lnnn0n1nnnn2n3n4s5nSn67
    /* 254 cycles a nonce, 0.94 MH/s: every wait two cycles or more over its threshold */
    SCAN sha_hw_scan_safe, 1, 12, 14, 44, 12, 11, 31, Lnnnnnnnnnnnn0nn1nns2nn3nn4nn5nn6nn7nnS

#ifdef SHA_BENCH
#define V(name, g, p1, d2, p2, pl1, d3, p3, tail) SCAN name, g, p1, d2, p2, pl1, d3, p3, tail
#include "bench_variants.def"
#endif
