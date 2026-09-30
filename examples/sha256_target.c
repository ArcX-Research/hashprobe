/* Minimal stdin/stdout adapter for the original research SHA-256 reference.
 * It shares Hashprobe's reference, so use OpenSSL for an independent target. */
#include "reference/sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Deliberate, repeatable faults for the visualization. The input's digest
 * scatters a few failures through the suite without using process randomness. */
static void inject_demo_fault(const uint8_t *input, size_t length, uint8_t digest[32]) {
    if (length == 1 && input[0] == 0) {
        sha256(input, 0, digest);
        return;
    }
    switch (digest[0] & 31u) {
    case 0: /* Drop the last input byte. */
        if (length) sha256(input, length - 1, digest);
        break;
    case 1: /* Flip one output bit. */
        digest[0] ^= 1u;
        break;
    case 2: /* Write each 32-bit digest word in the opposite byte order. */
        for (size_t i = 0; i < 32; i += 4) {
            uint8_t a = digest[i], b = digest[i + 1];
            digest[i] = digest[i + 3];
            digest[i + 1] = digest[i + 2];
            digest[i + 2] = b;
            digest[i + 3] = a;
        }
        break;
    }
}

int main(int argc, char **argv) {
    int demo_nul = argc == 2 && !strcmp(argv[1], "--demo-bug-nul");
    int demo_bugs = argc == 2 && !strcmp(argv[1], "--demo-bugs");
    if (argc != 1 && !demo_nul && !demo_bugs) {
        fputs("usage: sha256-target [--demo-bug-nul | --demo-bugs]\n", stderr);
        return 2;
    }
    const size_t limit = 1024u * 1024u;
    uint8_t *input = malloc(limit + 1);
    if (!input) return 2;
    size_t length = fread(input, 1, limit + 1, stdin);
    if (ferror(stdin) || length > limit) {
        fputs("input exceeds 1 MiB or cannot be read\n", stderr);
        free(input);
        return 2;
    }
    /* Deliberately reproduce a C-string length bug so the tester can detect it. */
    if (demo_nul) {
        const uint8_t *first_nul = memchr(input, 0, length);
        if (first_nul) length = (size_t)(first_nul - input);
    }
    uint8_t digest[32];
    sha256(input, length, digest);
    if (demo_bugs) inject_demo_fault(input, length, digest);
    free(input);
    for (size_t i = 0; i < sizeof digest; i++) printf("%02x", digest[i]);
    putchar('\n');
    return ferror(stdout) ? 2 : 0;
}
