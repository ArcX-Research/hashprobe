#include "hashprobe.h"
#include "reference/sha256.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

static void add_string(cJSON *object, const char *key, const char *value) {
    hp_json_add(object, key, cJSON_CreateString(value));
}

static void add_number(cJSON *object, const char *key, double value) {
    hp_json_add(object, key, cJSON_CreateNumber(value));
}

static void add_hex(cJSON *object, const char *key, const uint8_t *bytes, size_t length) {
    char *hex = hp_hex(bytes, length);
    add_string(object, key, hex);
    free(hex);
}

/* Only read regular files; replay and metadata collection must not block on a
 * FIFO or consume an unbounded input. Also recheck length after the stat. */
static char *read_file(const char *path, size_t limit, size_t *length) {
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return NULL;
    struct stat st;
    if (hp_cloexec(fd) || fstat(fd, &st) || !S_ISREG(st.st_mode) ||
        st.st_size < 0 || (uintmax_t)st.st_size > limit) {
        close(fd);
        return NULL;
    }
    size_t size = (size_t)st.st_size;
    char *data = hp_alloc(size + 1);
    size_t used = 0;
    while (used < size) {
        ssize_t count = read(fd, data + used, size - used);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) goto error;
        used += (size_t)count;
    }
    char extra;
    ssize_t count;
    do { count = read(fd, &extra, 1); } while (count < 0 && errno == EINTR);
    if (count != 0) goto error;
    close(fd);
    data[size] = '\0';
    *length = size;
    return data;
error:
    free(data);
    close(fd);
    return NULL;
}

static char *resolve_executable(const char *name) {
    if (!name || !*name) return NULL;
    if (strchr(name, '/')) return realpath(name, NULL);
    const char *path = getenv("PATH");
    if (!path) path = "/usr/bin:/bin";
    const char *start = path;
    for (;;) {
        const char *end = strchr(start, ':');
        size_t length = end ? (size_t)(end - start) : strlen(start);
        size_t size = length + strlen(name) + 3;
        char *candidate = hp_alloc(size);
        if (length) snprintf(candidate, size, "%.*s/%s", (int)length, start, name);
        else snprintf(candidate, size, "./%s", name);
        struct stat st;
        char *resolved = NULL;
        if (!access(candidate, X_OK) && !stat(candidate, &st) && S_ISREG(st.st_mode))
            resolved = realpath(candidate, NULL);
        free(candidate);
        if (resolved) return resolved;
        if (!end) return NULL;
        start = end + 1;
    }
}

static cJSON *executable_metadata(const char *name) {
    cJSON *info = cJSON_CreateObject();
    char *path = resolve_executable(name);
    if (!path) {
        hp_json_add(info, "resolved_path", cJSON_CreateNull());
        hp_json_add(info, "sha256", cJSON_CreateNull());
        return info;
    }
    add_string(info, "resolved_path", path);
    size_t length;
    char *data = read_file(path, 64u * 1024u * 1024u, &length);
    if (data) {
        uint8_t digest[32];
        sha256((uint8_t *)data, length, digest);
        add_hex(info, "sha256", digest, 32);
        add_number(info, "bytes", (double)length);
        free(data);
    } else hp_json_add(info, "sha256", cJSON_CreateNull());
    free(path);
    return info;
}

cJSON *hp_report_create(const hp_options *options, const char *source_digest) {
    cJSON *report = cJSON_CreateObject();
    add_number(report, "schema_version", 1);
    cJSON *tool = cJSON_CreateObject();
    add_string(tool, "name", "hashprobe");
    add_string(tool, "version", HP_VERSION);
    hp_json_add(report, "tool", tool);
    add_string(report, "algorithm", "sha256");
    add_string(report, "mode", options->mode);
    time_t now = time(NULL);
    struct tm utc;
    char timestamp[32] = "unknown";
    if (gmtime_r(&now, &utc)) strftime(timestamp, sizeof timestamp, "%Y-%m-%dT%H:%M:%SZ", &utc);
    add_string(report, "started_at", timestamp);
    struct utsname platform;
    if (!uname(&platform)) {
        cJSON *host = cJSON_CreateObject();
        add_string(host, "os", platform.sysname);
        add_string(host, "release", platform.release);
        add_string(host, "machine", platform.machine);
        hp_json_add(report, "platform", host);
    }
    char *cwd = getcwd(NULL, 0);
    hp_json_add(report, "cwd", cwd ? cJSON_CreateString(cwd) : cJSON_CreateNull());
    free(cwd);
    cJSON *reference = cJSON_CreateObject();
    add_string(reference, "name", "ArcX SHA-256 C reference");
    add_number(reference, "known_answers_checked", 4);
    hp_json_add(report, "reference", reference);
    cJSON *suite = cJSON_CreateObject();
    if (options->source_path) {
        add_string(suite, "version", "saved-cases-v1");
        add_string(suite, "source_report", options->source_path);
        add_string(suite, "source_sha256", source_digest);
    } else {
        add_string(suite, "version", HP_SUITE_VERSION);
        add_number(suite, "seed", options->seed);
        add_number(suite, "random_cases", options->random_cases);
        add_number(suite, "max_random_bytes", options->max_bytes);
    }
    hp_json_add(report, "suite", suite);
    cJSON *target = cJSON_CreateObject();
    cJSON *command = cJSON_CreateArray();
    for (char **arg = options->command; *arg; arg++) hp_json_append(command, cJSON_CreateString(*arg));
    hp_json_add(target, "command", command);
    hp_json_add(target, "executable", executable_metadata(options->command[0]));
    hp_json_add(target, "version_tag", options->target_version ? cJSON_CreateString(options->target_version) : cJSON_CreateNull());
    add_string(target, "output_format", options->binary ? "binary" : "hex");
    add_number(target, "timeout_ms", options->timeout_ms);
    add_number(target, "output_limit_bytes", HP_OUTPUT_LIMIT);
    hp_json_add(report, "target", target);
    hp_json_add(report, "results", cJSON_CreateArray());
    return report;
}

void hp_report_result(cJSON *report, const hp_case *test, const hp_result *result) {
    cJSON *item = cJSON_CreateObject();
    add_string(item, "id", test->id);
    add_string(item, "category", test->category);
    add_number(item, "input_bytes", (double)test->length);
    add_hex(item, "expected_hex", test->expected, 32);
    if (result->has_actual) add_hex(item, "actual_hex", result->actual, 32);
    else hp_json_add(item, "actual_hex", cJSON_CreateNull());
    add_string(item, "status", result->status);
    add_number(item, "duration_ms", result->duration_ms);
    if (strcmp(result->status, "pass")) add_hex(item, "input_hex", test->input, test->length);
    if (result->error_kind) {
        cJSON *error = cJSON_CreateObject();
        add_string(error, "kind", result->error_kind);
        add_string(error, "detail", result->detail);
        hp_json_add(error, "exit_status", result->exit_status >= 0 ? cJSON_CreateNumber(result->exit_status) : cJSON_CreateNull());
        hp_json_add(error, "signal", result->signal_number ? cJSON_CreateNumber(result->signal_number) : cJSON_CreateNull());
        add_number(error, "stdout_bytes_captured", (double)result->stdout_length);
        add_number(error, "stderr_bytes_captured", (double)result->stderr_length);
        add_hex(error, "stdout_hex", result->stdout_excerpt,
                result->stdout_length < HP_EXCERPT ? result->stdout_length : HP_EXCERPT);
        add_hex(error, "stderr_hex", result->stderr_excerpt,
                result->stderr_length < HP_EXCERPT ? result->stderr_length : HP_EXCERPT);
        hp_json_add(item, "error", error);
    }
    hp_json_append(cJSON_GetObjectItemCaseSensitive(report, "results"), item);
}

void hp_report_finish(cJSON *report, size_t planned, size_t executed,
                      size_t passed, size_t mismatches, size_t errors, int interrupted) {
    cJSON *summary = cJSON_CreateObject();
    add_number(summary, "planned", (double)planned);
    add_number(summary, "executed", (double)executed);
    add_number(summary, "passed", (double)passed);
    add_number(summary, "mismatches", (double)mismatches);
    add_number(summary, "errors", (double)errors);
    add_number(summary, "skipped", (double)(planned - executed));
    hp_json_add(summary, "complete", cJSON_CreateBool(executed == planned && !interrupted));
    hp_json_add(report, "summary", summary);
    const char *status = interrupted ? "interrupted" : errors ? "error" : mismatches ? "mismatch" :
        executed == planned ? "pass" : "error";
    add_string(report, "status", status);
}

int hp_report_begin(hp_report_file *file, const char *path, char *error, size_t size) {
    memset(file, 0, sizeof *file);
    file->final_path = path;
    struct stat st;
    if (!lstat(path, &st) || errno != ENOENT) {
        snprintf(error, size, "report path already exists or cannot be accessed; choose a new path");
        return -1;
    }
    size_t length = strlen(path) + 12;
    file->temporary_path = hp_alloc(length);
    snprintf(file->temporary_path, length, "%s.tmp.XXXXXX", path);
    int fd = mkstemp(file->temporary_path);
    if (fd < 0) {
        free(file->temporary_path);
        file->temporary_path = NULL;
        goto error;
    }
    if (hp_cloexec(fd)) {
        close(fd);
        goto error;
    }
    file->stream = fdopen(fd, "w");
    if (!file->stream) {
        close(fd);
        goto error;
    }
    return 0;
error:
    snprintf(error, size, "cannot create report: %s", strerror(errno));
    hp_report_abort(file);
    return -1;
}

void hp_report_abort(hp_report_file *file) {
    if (file->stream) fclose(file->stream);
    if (file->temporary_path) unlink(file->temporary_path);
    free(file->temporary_path);
    memset(file, 0, sizeof *file);
}

int hp_report_commit(hp_report_file *file, const cJSON *report, char *error, size_t size) {
    char *json = cJSON_Print(report);
    if (!json) {
        snprintf(error, size, "cannot encode report");
        hp_report_abort(file);
        return -1;
    }
    int failed = fputs(json, file->stream) == EOF || fputc('\n', file->stream) == EOF ||
        fflush(file->stream) || fsync(fileno(file->stream));
    free(json);
    if (fclose(file->stream)) failed = 1;
    file->stream = NULL;
    /* A hard link publishes the complete file atomically without overwriting
     * a report another process might have created after the preflight check. */
    if (!failed && link(file->temporary_path, file->final_path)) failed = 1;
    if (failed) snprintf(error, size, "cannot save report: %s", strerror(errno));
    hp_report_abort(file);
    return failed ? -1 : 0;
}

static int safe_id(const char *value, size_t capacity) {
    size_t length = strlen(value);
    if (!length || length >= capacity) return 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)value[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) return 0;
    }
    return 1;
}

static const char *string_field(const cJSON *object, const char *key) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) ? value->valuestring : NULL;
}

/* Bound structural complexity before cJSON allocates nodes. Reject embedded
 * NULs (unsupported by cJSON strings) and literal control bytes in strings. */
static int valid_json_input(const char *data, size_t length) {
    int in_string = 0;
    size_t structure = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)data[i];
        if (!c) return 0;
        if (in_string && c == '\\') {
            if (i + 1 >= length) return 0;
            if (i + 5 < length && !memcmp(data + i + 1, "u0000", 5)) return 0;
            i++;
        } else if (c == '"') {
            in_string = !in_string;
            if (++structure > 100000) return 0;
        } else if (in_string && c < 32) return 0;
        else if (!in_string && (c == '[' || c == '{' || c == ',' || c == ':')) {
            if (++structure > 100000) return 0;
        }
    }
    return !in_string;
}

static int unique_keys(const cJSON *node) {
    size_t count = 0;
    for (const cJSON *child = node->child; child; child = child->next) {
        if (cJSON_IsObject(node)) {
            if (++count > 64) return 0;
            for (const cJSON *previous = node->child; previous != child; previous = previous->next)
                if (!strcmp(previous->string, child->string)) return 0;
        }
        if (!unique_keys(child)) return 0;
    }
    return 1;
}

int hp_suite_replay(hp_suite *suite, const hp_options *options,
                    char source_digest[65], char *error, size_t error_size) {
    size_t length;
    char *data = read_file(options->source_path, HP_MAX_REPORT, &length);
    if (!data) {
        snprintf(error, error_size, "cannot read source report (must be a regular file at most 40 MiB)");
        return -1;
    }
    cJSON *report = NULL;
    const char *reason = "invalid JSON report";
    if (!valid_json_input(data, length)) goto invalid;
    report = cJSON_ParseWithLengthOpts(data, length + 1, NULL, 1);
    if (!report || !cJSON_IsObject(report) || !unique_keys(report)) goto invalid;
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(report, "schema_version");
    const char *algorithm = string_field(report, "algorithm");
    const cJSON *tool = cJSON_GetObjectItemCaseSensitive(report, "tool");
    const char *tool_name = string_field(tool, "name");
    reason = "unsupported report schema, tool, or algorithm";
    if (!cJSON_IsNumber(version) || version->valuedouble != 1 || !algorithm || strcmp(algorithm, "sha256") ||
        !tool_name || strcmp(tool_name, "hashprobe")) goto invalid;
    const cJSON *results = cJSON_GetObjectItemCaseSensitive(report, "results");
    reason = "invalid or oversized results array";
    if (!cJSON_IsArray(results) || cJSON_GetArraySize(results) > (int)HP_MAX_CASES) goto invalid;
    const char *ids[HP_MAX_CASES];
    size_t id_count = 0, total = 0;
    for (const cJSON *item = results->child; item; item = item->next) {
        const char *id = string_field(item, "id");
        const char *category = string_field(item, "category");
        const char *status = string_field(item, "status");
        const char *expected = string_field(item, "expected_hex");
        const cJSON *bytes = cJSON_GetObjectItemCaseSensitive(item, "input_bytes");
        uint8_t expected_bytes[32];
        reason = "invalid case fields in source report";
        if (!cJSON_IsObject(item) || !id || !safe_id(id, sizeof suite->cases[0].id) ||
            !category || !safe_id(category, sizeof suite->cases[0].category) || !status ||
            (strcmp(status, "pass") && strcmp(status, "mismatch") && strcmp(status, "error")) ||
            !expected || hp_unhex(expected, expected_bytes, 32) || !cJSON_IsNumber(bytes) ||
            !(bytes->valuedouble >= 0 && bytes->valuedouble <= HP_MAX_INPUT) ||
            bytes->valuedouble != (double)(size_t)bytes->valuedouble) goto invalid;
        reason = "duplicate case ID in source report";
        for (size_t i = 0; i < id_count; i++) if (!strcmp(ids[i], id)) goto invalid;
        ids[id_count++] = id;
        size_t input_length = (size_t)bytes->valuedouble;
        reason = "source report exceeds 16 MiB of test input";
        if (input_length > HP_MAX_TOTAL - total) goto invalid;
        total += input_length;
        if (!strcmp(status, "pass")) continue;
        const char *input_hex = string_field(item, "input_hex");
        reason = "missing or invalid saved input";
        if (!input_hex || strlen(input_hex) != input_length * 2) goto invalid;
        uint8_t *input = hp_alloc(input_length);
        if (hp_unhex(input_hex, input, input_length)) {
            free(input);
            goto invalid;
        }
        uint8_t digest[32];
        sha256(input, input_length, digest);
        if (memcmp(digest, expected_bytes, 32)) {
            free(input);
            reason = "saved input and expected SHA-256 digest disagree";
            goto invalid;
        }
        if (options->case_id && strcmp(options->case_id, id)) {
            free(input);
            continue;
        }
        hp_case *test = &suite->cases[suite->count++];
        snprintf(test->id, sizeof test->id, "%s", id);
        snprintf(test->category, sizeof test->category, "%s", category);
        test->input = input;
        test->length = input_length;
        memcpy(test->expected, expected_bytes, 32);
        suite->total_bytes += input_length;
    }
    reason = options->case_id ? "requested case is not a saved failure or error" : "source report contains no saved failures or errors";
    if (!suite->count) goto invalid;
    uint8_t digest[32];
    sha256((uint8_t *)data, length, digest);
    char *hex = hp_hex(digest, 32);
    memcpy(source_digest, hex, 65);
    free(hex);
    cJSON_Delete(report);
    free(data);
    return 0;
invalid:
    snprintf(error, error_size, "%s", reason);
    cJSON_Delete(report);
    free(data);
    hp_suite_free(suite);
    return -1;
}
