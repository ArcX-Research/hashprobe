/* Minimal stdin/stdout adapter for the original research SHA-256 reference.
 * It shares Hashprobe's reference, so use OpenSSL for an independent target. */
#include "reference/sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    int demo_bug = argc == 2 && !strcmp(argv[1], "--demo-bug-nul");
    if (argc != 1 && !demo_bug) {
        fputs("usage: sha256-target [--demo-bug-nul]\n", stderr);
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
    if (demo_bug) {
        const uint8_t *first_nul = memchr(input, 0, length);
        if (first_nul) length = (size_t)(first_nul - input);
    }
    uint8_t digest[32];
    sha256(input, length, digest);
    free(input);
    for (size_t i = 0; i < sizeof digest; i++) printf("%02x", digest[i]);
    putchar('\n');
    return ferror(stdout) ? 2 : 0;
}
