#ifndef HASHPROBE_H
#define HASHPROBE_H

#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "cJSON.h"

#define HP_VERSION "0.2.0"
#define HP_SUITE_VERSION "sha256-v1"
#define HP_MAX_INPUT (1024u * 1024u)
#define HP_MAX_TOTAL (16u * 1024u * 1024u)
#define HP_MAX_REPORT (40u * 1024u * 1024u)
#define HP_MAX_CASES 1024u
#define HP_OUTPUT_LIMIT 65536u
#define HP_EXCERPT 512u

typedef struct {
    char id[80];
    char category[32];
    uint8_t *input;
    size_t length;
    uint8_t expected[32];
} hp_case;

typedef struct {
    hp_case cases[HP_MAX_CASES];
    size_t count;
    size_t total_bytes;
} hp_suite;

typedef struct {
    const char *mode;
    const char *report_path;
    const char *source_path;
    const char *case_id;
    const char *target_version;
    char **command;
    uint32_t seed;
    unsigned random_cases;
    unsigned max_bytes;
    unsigned timeout_ms;
    int binary;
    int fail_fast;
} hp_options;

typedef struct {
    const char *status;
    const char *error_kind;
    char detail[256];
    uint8_t actual[32];
    int has_actual;
    int exit_status;
    int signal_number;
    double duration_ms;
    uint8_t stdout_excerpt[HP_EXCERPT];
    uint8_t stderr_excerpt[HP_EXCERPT];
    size_t stdout_length;
    size_t stderr_length;
} hp_result;

typedef struct {
    FILE *stream;
    char *temporary_path;
    const char *final_path;
} hp_report_file;

extern volatile sig_atomic_t hp_interrupted;
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 2)))
#endif
_Noreturn void hp_fatal(const char *format, ...);
void *hp_alloc(size_t size);
char *hp_hex(const uint8_t *bytes, size_t size);
int hp_unhex(const char *hex, uint8_t *bytes, size_t size);
double hp_now_ms(void);
int hp_cloexec(int fd);
void hp_json_add(cJSON *object, const char *key, cJSON *value);
void hp_json_append(cJSON *array, cJSON *value);
cJSON *hp_json_parse(const char *data, size_t length);
int hp_json_integer(const cJSON *value, uint64_t minimum, uint64_t maximum, uint64_t *out);

int hp_self_test(void);
int hp_run(const hp_options *options);
void hp_suite_build(hp_suite *suite, const hp_options *options);
int hp_suite_replay(hp_suite *suite, const hp_options *options,
                    char source_digest[65], char *error, size_t error_size);
void hp_suite_free(hp_suite *suite);
void hp_target_run(const hp_case *test, const hp_options *options, hp_result *result);

int hp_report_begin(hp_report_file *file, const char *path, char *error, size_t size);
int hp_report_commit(hp_report_file *file, const cJSON *report, char *error, size_t size);
void hp_report_abort(hp_report_file *file);
cJSON *hp_report_create(const hp_options *options, const char *source_digest);
void hp_report_result(cJSON *report, const hp_case *test, const hp_result *result);
void hp_report_finish(cJSON *report, size_t planned, size_t executed,
                      size_t passed, size_t mismatches, size_t errors, int interrupted);

#endif
