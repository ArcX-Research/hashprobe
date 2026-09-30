#include "hashprobe.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(FILE *stream) {
    fputs(
        "Hashprobe " HP_VERSION " - test SHA-256 implementations\n\n"
        "Usage:\n"
        "  hashprobe check --report FILE [OPTIONS] -- COMMAND [ARGS...]\n"
        "  hashprobe replay SOURCE --report FILE [OPTIONS] -- COMMAND [ARGS...]\n"
        "  hashprobe self-test\n\n"
        "Options:\n"
        "  --report FILE          Save JSON to a new file (required)\n"
        "  --output hex|binary    Target stdout format (default: hex)\n"
        "  --timeout-ms N         Per-case deadline, 1..600000 (default: 5000)\n"
        "  --target-version TEXT  Label the tested build\n"
        "  --fail-fast            Stop on the first digest mismatch\n"
        "  --seed N               Check: 32-bit seed (default: 0)\n"
        "  --random-cases N       Check: 0..256 random cases (default: 32)\n"
        "  --max-bytes N          Check: random size limit, 1..65536 (default: 4096)\n"
        "  --case ID              Replay: select one saved failure or error\n"
        "  --help                 Show this help\n"
        "  --version              Show version\n\n"
        "Send raw message bytes to target stdin; read one digest from stdout.\n"
        "Exit codes: 0 pass, 1 mismatch, 2 error, 128 + signal when interrupted.\n",
        stream);
}

static uint32_t unsigned_option(const char *name, const char *value, uint32_t maximum) {
    if (!*value) hp_fatal("%s requires an unsigned decimal integer", name);
    uint64_t number = 0;
    for (const char *p = value; *p; p++) {
        if (*p < '0' || *p > '9') hp_fatal("%s requires an unsigned decimal integer", name);
        number = number * 10 + (unsigned)(*p - '0');
        if (number > maximum) hp_fatal("%s exceeds its maximum (%u)", name, maximum);
    }
    return (uint32_t)number;
}

static hp_options parse_options(int argc, char **argv) {
    hp_options options = {0};
    options.mode = argv[1];
    options.random_cases = 32;
    options.max_bytes = 4096;
    options.timeout_ms = 5000;
    int replay = !strcmp(options.mode, "replay");
    if (!replay && strcmp(options.mode, "check")) hp_fatal("unknown command; use --help");
    int index = 2;
    if (replay) {
        if (index == argc || !strcmp(argv[index], "--")) hp_fatal("replay requires a source report");
        options.source_path = argv[index++];
    }
    for (; index < argc && strcmp(argv[index], "--"); index++) {
        const char *name = argv[index];
        if (!strcmp(name, "--fail-fast")) {
            options.fail_fast = 1;
            continue;
        }
        if (!strcmp(name, "--help")) {
            usage(stdout);
            exit(0);
        }
        if (index + 1 >= argc || !strcmp(argv[index + 1], "--")) hp_fatal("%s requires a value", name);
        const char *value = argv[++index];
        if (!strcmp(name, "--report")) options.report_path = value;
        else if (!strcmp(name, "--output")) {
            if (strcmp(value, "hex") && strcmp(value, "binary")) hp_fatal("--output must be hex or binary");
            options.binary = !strcmp(value, "binary");
        } else if (!strcmp(name, "--timeout-ms")) {
            options.timeout_ms = unsigned_option(name, value, 600000);
            if (!options.timeout_ms) hp_fatal("--timeout-ms must be positive");
        } else if (!strcmp(name, "--target-version")) {
            if (strlen(value) > 1024) hp_fatal("--target-version exceeds 1024 bytes");
            options.target_version = value;
        } else if (!strcmp(name, "--case") && replay) options.case_id = value;
        else if (!strcmp(name, "--seed") && !replay) options.seed = unsigned_option(name, value, UINT32_MAX);
        else if (!strcmp(name, "--random-cases") && !replay) options.random_cases = unsigned_option(name, value, 256);
        else if (!strcmp(name, "--max-bytes") && !replay) {
            options.max_bytes = unsigned_option(name, value, 65536);
            if (!options.max_bytes) hp_fatal("--max-bytes must be positive");
        } else hp_fatal("unknown option for %s: %s", options.mode, name);
    }
    if (!options.report_path || !*options.report_path) hp_fatal("--report FILE is required");
    if (index >= argc || index + 1 >= argc) hp_fatal("provide a target command after --");
    options.command = argv + index + 1;
    size_t command_bytes = 0, command_count = 0;
    for (char **arg = options.command; *arg; arg++) {
        command_bytes += strlen(*arg) + 1;
        command_count++;
    }
    if (!options.command[0] || !*options.command[0] || command_count > 128 || command_bytes > 65536)
        hp_fatal("target command is empty or exceeds 128 arguments / 65536 bytes");
    return options;
}

static void ensure_standard_fds(void) {
    /* Keep temporary files and pipes above descriptors 0, 1, and 2 even when
     * called by a parent with a closed standard stream. */
    for (int fd = 0; fd < 3; fd++) {
        if (fcntl(fd, F_GETFD) < 0 && errno == EBADF) {
            if (open("/dev/null", O_RDWR) < 0) hp_fatal("cannot open standard stream");
        }
    }
}

int main(int argc, char **argv) {
    ensure_standard_fds();
    cJSON_Hooks hooks = {hp_alloc, free};
    cJSON_InitHooks(&hooks);
    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    if (!strcmp(argv[1], "--help") || !strcmp(argv[1], "help")) {
        usage(stdout);
        return 0;
    }
    if (!strcmp(argv[1], "--version")) {
        puts("hashprobe " HP_VERSION);
        return 0;
    }
    if (!strcmp(argv[1], "self-test")) {
        if (argc != 2) hp_fatal("self-test does not accept arguments");
        if (hp_self_test()) hp_fatal("SHA-256 reference failed its known answers");
        puts("PASS: SHA-256 reference matches 4 known answers");
        return 0;
    }
    hp_options options = parse_options(argc, argv);
    return hp_run(&options);
}
