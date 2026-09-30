/* sha256.c — SHA-256 (FIPS 180-4) reference / reduced-round / instrumented.
 * See sha256.h for the API and index conventions. */
#include "sha256.h"

#include <stdlib.h>
#include <string.h>

/* First 32 bits of the fractional parts of the cube roots of the first 64
 * primes (FIPS 180-4 §4.2.2). */
const uint32_t SHA256_K[SHA256_ROUNDS] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

/* First 32 bits of the fractional parts of the square roots of the first 8
 * primes (FIPS 180-4 §5.3.3). */
const uint32_t SHA256_IV[8] = {
    0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
};

/* ---- primitive functions (FIPS 180-4 §4.1.2) ---- */

static inline uint32_t rotr(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32u - n));
}
static inline uint32_t ch (uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (~x & z); }
static inline uint32_t maj(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }
static inline uint32_t bsig0(uint32_t x) { return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22); }
static inline uint32_t bsig1(uint32_t x) { return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25); }
static inline uint32_t ssig0(uint32_t x) { return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3); }
static inline uint32_t ssig1(uint32_t x) { return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10); }

static inline uint32_t load_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}
static inline void store_be32(uint8_t *p, uint32_t x) {
    p[0] = (uint8_t)(x >> 24); p[1] = (uint8_t)(x >> 16);
    p[2] = (uint8_t)(x >> 8);  p[3] = (uint8_t)x;
}

static inline int clamp_rounds(int rounds) {
    if (rounds < 0)             return 0;
    if (rounds > SHA256_ROUNDS) return SHA256_ROUNDS;
    return rounds;
}

/* The single compression core. Reads the chaining value from H, overwrites H
 * with the block output, and — when bt != NULL — records every internal.
 * This is the only place the round function lives; plain, reduced and traced
 * paths all route through here so they can never diverge. */
static void compress(uint32_t H[8], const uint8_t block[SHA256_BLOCK_LEN],
                     int rounds, sha256_block_trace *bt) {
    rounds = clamp_rounds(rounds);

    uint32_t w[SHA256_ROUNDS];
    for (int t = 0; t < 16; t++)
        w[t] = load_be32(block + 4 * t);
    for (int t = 16; t < SHA256_ROUNDS; t++)
        w[t] = ssig1(w[t - 2]) + w[t - 7] + ssig0(w[t - 15]) + w[t - 16];

    uint32_t a = H[0], b = H[1], c = H[2], d = H[3],
             e = H[4], f = H[5], g = H[6], h = H[7];

    if (bt) {
        memcpy(bt->w, w, sizeof w);
        memcpy(bt->h_in, H, 8 * sizeof(uint32_t));
        bt->rounds = rounds;
        bt->state[0][SHA256_A] = a; bt->state[0][SHA256_B] = b;
        bt->state[0][SHA256_C] = c; bt->state[0][SHA256_D] = d;
        bt->state[0][SHA256_E] = e; bt->state[0][SHA256_F] = f;
        bt->state[0][SHA256_G] = g; bt->state[0][SHA256_H] = h;
    }

    for (int t = 0; t < rounds; t++) {
        uint32_t T1 = h + bsig1(e) + ch(e, f, g) + SHA256_K[t] + w[t];
        uint32_t T2 = bsig0(a) + maj(a, b, c);
        h = g; g = f; f = e; e = d + T1;
        d = c; c = b; b = a; a = T1 + T2;
        if (bt) {
            bt->t1[t] = T1; bt->t2[t] = T2;
            bt->state[t + 1][SHA256_A] = a; bt->state[t + 1][SHA256_B] = b;
            bt->state[t + 1][SHA256_C] = c; bt->state[t + 1][SHA256_D] = d;
            bt->state[t + 1][SHA256_E] = e; bt->state[t + 1][SHA256_F] = f;
            bt->state[t + 1][SHA256_G] = g; bt->state[t + 1][SHA256_H] = h;
        }
    }

    H[0] += a; H[1] += b; H[2] += c; H[3] += d;
    H[4] += e; H[5] += f; H[6] += g; H[7] += h;

    if (bt) memcpy(bt->h_out, H, 8 * sizeof(uint32_t));
}

/* Build the 1 or 2 trailing padded blocks for a message whose last `rem`
 * (0..63) bytes start at `tail`. Writes into `out` (>=128 bytes) and returns
 * the padded length in bytes (64 or 128). FIPS 180-4 §5.1.1. */
static size_t build_padding(const uint8_t *tail, size_t rem, uint64_t msg_bits,
                            uint8_t out[128]) {
    memcpy(out, tail, rem);
    out[rem] = 0x80;
    size_t padded = (rem < 56) ? 64 : 128;
    memset(out + rem + 1, 0, padded - rem - 1 - 8);
    for (int j = 0; j < 8; j++)
        out[padded - 1 - j] = (uint8_t)(msg_bits >> (8 * j));
    return padded;
}

/* ------------------------------------------------------------------ */
/* Plain API                                                           */
/* ------------------------------------------------------------------ */

void sha256_reduced(const uint8_t *msg, size_t len, int rounds,
                    uint8_t out[SHA256_DIGEST_LEN]) {
    uint32_t H[8];
    memcpy(H, SHA256_IV, sizeof H);

    size_t i = 0;
    for (; i + SHA256_BLOCK_LEN <= len; i += SHA256_BLOCK_LEN)
        compress(H, msg + i, rounds, NULL);

    uint8_t pad[128];
    size_t rem = len - i;
    size_t padded = build_padding(msg + i, rem, (uint64_t)len * 8, pad);
    for (size_t off = 0; off < padded; off += SHA256_BLOCK_LEN)
        compress(H, pad + off, rounds, NULL);

    for (int k = 0; k < 8; k++)
        store_be32(out + 4 * k, H[k]);
}

void sha256(const uint8_t *msg, size_t len, uint8_t out[SHA256_DIGEST_LEN]) {
    sha256_reduced(msg, len, SHA256_ROUNDS, out);
}

/* ------------------------------------------------------------------ */
/* Instrumented API                                                    */
/* ------------------------------------------------------------------ */

void sha256_compress_trace(const uint32_t h_in[8],
                           const uint8_t block[SHA256_BLOCK_LEN],
                           int rounds, sha256_block_trace *bt) {
    uint32_t H[8];
    memcpy(H, h_in, sizeof H);       /* leave caller's chaining value intact */
    compress(H, block, rounds, bt);  /* bt->h_in / bt->h_out carry the result */
}

int sha256_trace_run(sha256_trace *tr, const uint8_t *msg, size_t len,
                     int rounds) {
    size_t full = len / SHA256_BLOCK_LEN;
    size_t rem  = len % SHA256_BLOCK_LEN;
    size_t nblocks = full + ((rem < 56) ? 1 : 2);

    tr->blocks = calloc(nblocks, sizeof *tr->blocks);
    if (!tr->blocks) return -1;
    tr->nblocks = nblocks;
    tr->rounds  = clamp_rounds(rounds);

    uint32_t H[8];
    memcpy(H, SHA256_IV, sizeof H);

    size_t bi = 0, i = 0;
    for (; i + SHA256_BLOCK_LEN <= len; i += SHA256_BLOCK_LEN) {
        sha256_compress_trace(H, msg + i, rounds, &tr->blocks[bi]);
        memcpy(H, tr->blocks[bi].h_out, sizeof H);
        bi++;
    }

    uint8_t pad[128];
    size_t padded = build_padding(msg + i, len - i, (uint64_t)len * 8, pad);
    for (size_t off = 0; off < padded; off += SHA256_BLOCK_LEN) {
        sha256_compress_trace(H, pad + off, rounds, &tr->blocks[bi]);
        memcpy(H, tr->blocks[bi].h_out, sizeof H);
        bi++;
    }

    memcpy(tr->midstate, tr->blocks[0].h_out, sizeof tr->midstate);
    for (int k = 0; k < 8; k++)
        store_be32(tr->digest + 4 * k, H[k]);
    return 0;
}

void sha256_trace_free(sha256_trace *tr) {
    free(tr->blocks);
    tr->blocks = NULL;
    tr->nblocks = 0;
}
