/* sha256.h — SHA-256 (FIPS 180-4): reference, reduced-round, instrumented.
 *
 * Part of ArcX cryptanalysis tooling. Three layers sit on one compression core:
 *   1. sha256()            — the standard 64-round hash.
 *   2. sha256_reduced()    — compression truncated to R rounds (the attack target).
 *   3. sha256_*_trace()    — every internal a cryptanalyst reads: the message
 *                            schedule W, the per-round working variables a..h,
 *                            T1/T2, and the midstate after each block.
 *
 * All arithmetic is on 32-bit words, big-endian on the wire (FIPS 180-4 §5.1.1).
 */
#ifndef ARCX_SHA256_H
#define ARCX_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHA256_DIGEST_LEN 32
#define SHA256_BLOCK_LEN  64   /* bytes per message block (512 bits) */
#define SHA256_ROUNDS     64   /* rounds in the full compression function */

/* Column indices into sha256_block_trace.state[t][*]. The eight working
 * variables of FIPS 180-4 §6.2, in order. */
enum {
    SHA256_A = 0, SHA256_B, SHA256_C, SHA256_D,
    SHA256_E,     SHA256_F, SHA256_G, SHA256_H
};

extern const uint32_t SHA256_K[SHA256_ROUNDS]; /* round constants (§4.2.2) */
extern const uint32_t SHA256_IV[8];            /* initial hash value H(0) (§5.3.3) */

/* ------------------------------------------------------------------ */
/* Plain digest API                                                    */
/* ------------------------------------------------------------------ */

/* Standard SHA-256 of `len` bytes at `msg`. Writes 32 bytes to `out`. */
void sha256(const uint8_t *msg, size_t len, uint8_t out[SHA256_DIGEST_LEN]);

/* SHA-256 with the compression loop truncated to `rounds` steps (clamped to
 * 0..64). Message expansion, padding and feed-forward are unchanged; only the
 * number of round updates differs. rounds == 64 reproduces sha256() exactly.
 * This is "SHA-256 reduced to R rounds" — the experimental target. */
void sha256_reduced(const uint8_t *msg, size_t len, int rounds,
                    uint8_t out[SHA256_DIGEST_LEN]);

/* ------------------------------------------------------------------ */
/* Instrumented API                                                    */
/* ------------------------------------------------------------------ */

/* Everything computed while compressing one 512-bit block.
 *
 * Index conventions (with R = rounds actually run):
 *   w[0..63]        — full message schedule; w[0..15] are the block words,
 *                     w[16..63] the expansion. Always all 64 are computed.
 *   state[t][col]   — working variables ENTERING round t (== leaving round t-1),
 *                     col in {SHA256_A..SHA256_H}. Valid for t in 0..R.
 *                     state[0]  = h_in ; state[R] = pre-feed-forward state.
 *   t1[t], t2[t]    — the T1, T2 temporaries of round t. Valid for t in 0..R-1.
 *   h_in / h_out    — chaining value before / after this block (h_out is the
 *                     feed-forward sum h_in + state[R]).
 */
typedef struct {
    uint32_t w[SHA256_ROUNDS];
    uint32_t state[SHA256_ROUNDS + 1][8];
    uint32_t t1[SHA256_ROUNDS];
    uint32_t t2[SHA256_ROUNDS];
    uint32_t h_in[8];
    uint32_t h_out[8];
    int      rounds;
} sha256_block_trace;

/* Compress one raw 64-byte block against a chosen chaining value, recording all
 * internals into `bt`. `h_in` is left untouched (the result is in bt->h_out).
 * This is the compression-function-level entry point most attacks drive
 * directly: pick a chaining value and a message block, read the trace. */
void sha256_compress_trace(const uint32_t h_in[8],
                           const uint8_t block[SHA256_BLOCK_LEN],
                           int rounds, sha256_block_trace *bt);

/* Full-message trace: pads `msg`, compresses every block, keeps each block's
 * trace. `blocks` is heap-allocated (one entry per 512-bit block). */
typedef struct {
    sha256_block_trace *blocks;
    size_t              nblocks;
    uint32_t            midstate[8];              /* == blocks[0].h_out */
    uint8_t             digest[SHA256_DIGEST_LEN];
    int                 rounds;
} sha256_trace;

/* Populate `tr` for message `msg`. Returns 0 on success, -1 on allocation
 * failure. On success the caller must call sha256_trace_free(tr). */
int  sha256_trace_run(sha256_trace *tr, const uint8_t *msg, size_t len,
                      int rounds);
void sha256_trace_free(sha256_trace *tr);

#ifdef __cplusplus
}
#endif
#endif /* ARCX_SHA256_H */
