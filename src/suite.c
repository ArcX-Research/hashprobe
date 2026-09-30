#include "hashprobe.h"
#include "reference/sha256.h"

#include <stdlib.h>
#include <string.h>

static const char *known_digests[] = {
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
    "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
    "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"
};

static hp_case *add_case(hp_suite *suite, const char *id, const char *category, size_t length) {
    if (suite->count >= HP_MAX_CASES || length > HP_MAX_INPUT ||
        length > HP_MAX_TOTAL - suite->total_bytes)
        hp_fatal("suite exceeds case or input size limits");
    hp_case *test = &suite->cases[suite->count++];
    memset(test, 0, sizeof *test);
    snprintf(test->id, sizeof test->id, "%s", id);
    snprintf(test->category, sizeof test->category, "%s", category);
    test->input = hp_alloc(length);
    test->length = length;
    suite->total_bytes += length;
    return test;
}

static void known_answers(hp_suite *suite) {
    const char *messages[] = {"", "abc", "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"};
    const char *ids[] = {"kat-empty", "kat-abc", "kat-multiblock", "kat-million-a"};
    for (size_t i = 0; i < 4; i++) {
        size_t length = i == 3 ? 1000000 : strlen(messages[i]);
        hp_case *test = add_case(suite, ids[i], "known-answer", length);
        if (i == 3) memset(test->input, 'a', length);
        else memcpy(test->input, messages[i], length);
        if (hp_unhex(known_digests[i], test->expected, 32))
            hp_fatal("invalid built-in known answer");
    }
}

int hp_self_test(void) {
    hp_suite *suite = hp_alloc(sizeof *suite);
    memset(suite, 0, sizeof *suite);
    known_answers(suite);
    int ok = 1;
    for (size_t i = 0; i < suite->count; i++) {
        uint8_t digest[32];
        sha256(suite->cases[i].input, suite->cases[i].length, digest);
        if (memcmp(digest, suite->cases[i].expected, 32)) ok = 0;
    }
    hp_suite_free(suite);
    free(suite);
    return ok ? 0 : -1;
}

/* SplitMix64, with explicit unsigned arithmetic and little-endian byte
 * extraction below. This generator is for repeatable tests, not secrets. */
static uint64_t next_random(uint64_t *state) {
    uint64_t z = (*state += UINT64_C(0x9e3779b97f4a7c15));
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}

void hp_suite_build(hp_suite *suite, const hp_options *options) {
    static const size_t lengths[] = {
        1, 2, 3, 7, 8, 15, 16, 31, 32, 55, 56, 57, 63, 64, 65,
        119, 120, 121, 127, 128, 129, 255, 256, 257, 4095, 4096, 4097
    };
    static const char *patterns[] = {"zero", "ff", "ramp"};
    known_answers(suite);
    for (size_t i = 0; i < sizeof lengths / sizeof lengths[0]; i++) {
        for (size_t p = 0; p < 3; p++) {
            char id[80];
            snprintf(id, sizeof id, "boundary-%04zu-%s", lengths[i], patterns[p]);
            hp_case *test = add_case(suite, id, "boundary", lengths[i]);
            for (size_t j = 0; j < test->length; j++)
                test->input[j] = p == 0 ? 0 : p == 1 ? 255 : (uint8_t)j;
            sha256(test->input, test->length, test->expected);
        }
    }
    uint64_t state = options->seed;
    for (unsigned i = 0; i < options->random_cases; i++) {
        size_t length = (size_t)(next_random(&state) % ((uint64_t)options->max_bytes + 1));
        char id[80];
        snprintf(id, sizeof id, "random-%04u", i);
        hp_case *test = add_case(suite, id, "random", length);
        uint64_t word = 0;
        for (size_t j = 0; j < length; j++) {
            if (j % 8 == 0) word = next_random(&state);
            test->input[j] = (uint8_t)(word >> ((j % 8) * 8));
        }
        sha256(test->input, test->length, test->expected);
    }
}

void hp_suite_free(hp_suite *suite) {
    for (size_t i = 0; i < suite->count; i++) free(suite->cases[i].input);
    memset(suite, 0, sizeof *suite);
}
