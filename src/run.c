#include "hashprobe.h"

#include <stdlib.h>
#include <string.h>

static void interrupt_handler(int signum) {
    hp_interrupted = signum;
}

static void setup_signals(void) {
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = interrupt_handler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL))
        hp_fatal("cannot install interrupt handlers");
}

int hp_run(const hp_options *settings) {
    hp_options options = *settings;
    hp_interrupted = 0;
    setup_signals();
    sigset_t mask;
    sigemptyset(&mask);
    sigprocmask(SIG_SETMASK, &mask, NULL);
    if (hp_self_test()) hp_fatal("SHA-256 reference failed its known answers");
    hp_suite *suite = hp_alloc(sizeof *suite);
    memset(suite, 0, sizeof *suite);
    char error[512], source_digest[65] = "";
    if (options.source_path) {
        if (hp_suite_replay(suite, &options, source_digest, error, sizeof error)) {
            free(suite);
            hp_fatal("%s", error);
        }
    } else hp_suite_build(suite, &options);
    hp_report_file file;
    if (hp_report_begin(&file, options.report_path, error, sizeof error)) {
        hp_suite_free(suite);
        free(suite);
        hp_fatal("%s", error);
    }
    cJSON *report = hp_report_create(&options, source_digest);
    size_t executed = 0, passed = 0, mismatches = 0, errors = 0;
    for (size_t i = 0; i < suite->count && !hp_interrupted; i++) {
        hp_result result;
        hp_target_run(&suite->cases[i], &options, &result);
        hp_report_result(report, &suite->cases[i], &result);
        executed++;
        if (!strcmp(result.status, "pass")) passed++;
        else if (!strcmp(result.status, "mismatch")) {
            mismatches++;
            fprintf(stderr, "MISMATCH %s (%zu bytes)\n", suite->cases[i].id, suite->cases[i].length);
            if (options.fail_fast) break;
        } else {
            errors++;
            fprintf(stderr, "ERROR %s: %s (%s)\n", suite->cases[i].id, result.error_kind, result.detail);
            break;
        }
    }
    hp_report_finish(report, suite->count, executed, passed, mismatches, errors, hp_interrupted != 0);
    int saved = hp_report_commit(&file, report, error, sizeof error);
    const char *status = cJSON_GetObjectItemCaseSensitive(report, "status")->valuestring;
    if (saved) fprintf(stderr, "hashprobe: %s\n", error);
    else {
        printf("%s: %zu passed, %zu mismatches, %zu errors; %zu/%zu cases executed\n",
               status, passed, mismatches, errors, executed, suite->count);
        printf("Report: %s\n", options.report_path);
    }
    int code = 0;
    if (hp_interrupted) code = 128 + hp_interrupted;
    else if (saved || errors || (!mismatches && executed != suite->count)) code = 2;
    else if (mismatches) code = 1;
    cJSON_Delete(report);
    hp_suite_free(suite);
    free(suite);
    return code;
}
