#include "mcp.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

int mcp_report_id(const char *id) {
    if (!mcp_hex(id, 32)) return 0;
    for (size_t i = 0; i < 32; i++) if (id[i] >= 'A' && id[i] <= 'F') return 0;
    return 1;
}

char *mcp_report_path(const mcp_config *config, const char *id) {
    if (!mcp_report_id(id)) return NULL;
    char name[38];
    snprintf(name, sizeof name, "%s.json", id);
    return mcp_join(config->report_dir, name);
}

static int same_string(const cJSON *node, const char *key, const char *expected) {
    const char *actual = mcp_string(mcp_get(node, key));
    return actual && !strcmp(actual, expected);
}

static int valid_report(const cJSON *report) {
    if (!cJSON_IsObject(report) || !mcp_integer(mcp_get(report, "schema_version"), 1, 1, NULL) ||
        !same_string(report, "algorithm", "sha256") ||
        !same_string(mcp_get(report, "tool"), "name", "hashprobe")) return 0;
    const cJSON *results = mcp_get(report, "results"), *summary = mcp_get(report, "summary");
    if (!cJSON_IsArray(results) || !cJSON_IsObject(summary) || cJSON_GetArraySize(results) > (int)HP_MAX_CASES) return 0;
    uint64_t counts[3] = {0}, total = 0;
    const char *ids[HP_MAX_CASES];
    size_t count = 0;
    for (const cJSON *item = results->child; item; item = item->next) {
        const char *id = mcp_string(mcp_get(item, "id"));
        const char *status = mcp_string(mcp_get(item, "status"));
        const char *expected = mcp_string(mcp_get(item, "expected_hex"));
        const cJSON *actual_node = mcp_get(item, "actual_hex");
        const char *actual = mcp_string(actual_node);
        uint64_t size;
        if (!cJSON_IsObject(item) || !mcp_name(id, 79) || !status || !mcp_hex(expected, 64) ||
            !mcp_integer(mcp_get(item, "input_bytes"), 0, HP_MAX_INPUT, &size)) return 0;
        for (size_t i = 0; i < count; i++) if (!strcmp(id, ids[i])) return 0;
        ids[count++] = id;
        total += size;
        if (total > HP_MAX_TOTAL || (actual_node && !cJSON_IsNull(actual_node) && !mcp_hex(actual, 64))) return 0;
        if (!strcmp(status, "pass") || !strcmp(status, "mismatch")) {
            uint8_t wanted[32], got[32];
            if (!actual || hp_unhex(expected, wanted, 32) || hp_unhex(actual, got, 32)) return 0;
            if ((!memcmp(wanted, got, 32)) != (!strcmp(status, "pass"))) return 0;
            counts[!strcmp(status, "pass") ? 0 : 1]++;
        } else if (!strcmp(status, "error")) counts[2]++;
        else return 0;
        if (strcmp(status, "pass") && !mcp_hex(mcp_string(mcp_get(item, "input_hex")), (size_t)size * 2)) return 0;
        const cJSON *error = mcp_get(item, "error");
        if (error && !cJSON_IsNull(error) && !cJSON_IsObject(error)) return 0;
    }
    static const char *const keys[] = {"planned", "executed", "passed", "mismatches", "errors", "skipped"};
    uint64_t values[6];
    for (size_t i = 0; i < 6; i++)
        if (!mcp_integer(mcp_get(summary, keys[i]), 0, HP_MAX_CASES, &values[i])) return 0;
    if (values[1] != count || values[2] != counts[0] || values[3] != counts[1] ||
        values[4] != counts[2] || values[0] != values[1] + values[5]) return 0;
    const char *status = mcp_string(mcp_get(report, "status"));
    if (!status) return 0;
    int interrupted = !strcmp(status, "interrupted");
    int complete = values[0] == values[1] && !interrupted;
    const char *expected_status = interrupted ? "interrupted" : counts[2] ? "error" :
                                  counts[1] ? "mismatch" : complete ? "pass" : "error";
    const cJSON *finished = mcp_get(summary, "complete");
    if (strcmp(status, expected_status) || !cJSON_IsBool(finished) || cJSON_IsTrue(finished) != complete) return 0;
    const cJSON *target = mcp_get(report, "target"), *executable = mcp_get(target, "executable");
    const cJSON *version = mcp_get(target, "version_tag"), *digest = mcp_get(executable, "sha256");
    if (!cJSON_IsObject(target) || !cJSON_IsObject(executable)) return 0;
    if (version && !cJSON_IsNull(version) && (!mcp_string(version) || strlen(version->valuestring) > 1024)) return 0;
    if (digest && !cJSON_IsNull(digest) && !mcp_hex(mcp_string(digest), 64)) return 0;
    return 1;
}

cJSON *mcp_report_load(const mcp_config *config, const char *id) {
    char *path = mcp_report_path(config, id);
    if (!path) return NULL;
    cJSON *report = mcp_read_json(path, HP_MAX_REPORT);
    free(path);
    if (report && !valid_report(report)) { cJSON_Delete(report); report = NULL; }
    return report;
}

int mcp_reserve(const mcp_config *config, char *error) {
    char *path = mcp_join(config->report_dir, ".lock");
    int fd = open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK, 0600);
    free(path);
    struct stat info;
    if (fd < 0 || hp_cloexec(fd) || fstat(fd, &info) || !S_ISREG(info.st_mode)) {
        if (fd >= 0) close(fd);
        snprintf(error, MCP_ERROR_SIZE, "cannot open report directory lock");
        return -1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB)) {
        snprintf(error, MCP_ERROR_SIZE, "another test run is using this report directory; retry when it finishes");
        close(fd); return -1;
    }
    DIR *directory = opendir(config->report_dir);
    uint64_t count = 0, bytes = 0;
    int failed = !directory;
    if (directory) {
        struct dirent *entry;
        for (;;) {
            errno = 0;
            entry = readdir(directory);
            if (!entry) { if (errno) failed = 1; break; }
            if (strlen(entry->d_name) != 37 || strcmp(entry->d_name + 32, ".json")) continue;
            char id[33]; memcpy(id, entry->d_name, 32); id[32] = 0;
            if (!mcp_report_id(id)) continue;
            path = mcp_join(config->report_dir, entry->d_name);
            if (lstat(path, &info) || !S_ISREG(info.st_mode) || info.st_size < 0) failed = 1;
            else { count++; bytes += (uint64_t)info.st_size; }
            free(path);
            if (failed || count >= config->max_reports || bytes > (uint64_t)config->max_storage_mb * 1048576) break;
        }
        closedir(directory);
    }
    if (failed) snprintf(error, MCP_ERROR_SIZE, "cannot inspect report storage; remove invalid report entries");
    else if (count >= config->max_reports)
        snprintf(error, MCP_ERROR_SIZE, "report count limit reached; the owner must archive or remove old reports");
    else if (bytes + HP_MAX_REPORT > (uint64_t)config->max_storage_mb * 1048576)
        snprintf(error, MCP_ERROR_SIZE, "report storage limit reached; the owner must archive or remove old reports");
    else return fd;
    close(fd);
    return -1;
}

int mcp_random_id(char id[33]) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    uint8_t bytes[16];
    size_t used = 0;
    while (used < sizeof bytes) {
        ssize_t got = read(fd, bytes + used, sizeof bytes - used);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) { close(fd); return -1; }
        used += (size_t)got;
    }
    close(fd);
    bytes[6] = (uint8_t)((bytes[6] & 15) | 0x40);
    bytes[8] = (uint8_t)((bytes[8] & 63) | 0x80);
    char *hex = hp_hex(bytes, sizeof bytes);
    memcpy(id, hex, 33);
    free(hex);
    return 0;
}

cJSON *mcp_summary(const cJSON *report, const mcp_job *job) {
    cJSON *result = cJSON_CreateObject(), *failures = cJSON_CreateArray();
    mcp_string_add(result, "report_id", job->report_id);
    mcp_string_add(result, "target", job->target->name);
    mcp_string_add(result, "status", job->timed_out ? "error" : mcp_string(mcp_get(report, "status")));
    cJSON *counts = cJSON_Duplicate(mcp_get(report, "summary"), 1);
    if (job->timed_out) cJSON_ReplaceItemInObjectCaseSensitive(counts, "complete", cJSON_CreateFalse());
    hp_json_add(result, "counts", counts);
    hp_json_add(result, "failures", failures);
    unsigned count = 0;
    const cJSON *results = mcp_get(report, "results");
    for (const cJSON *item = results->child; item; item = item->next) {
        if (same_string(item, "status", "pass")) continue;
        if (count++ >= 10) continue;
        cJSON *failure = cJSON_CreateObject();
        mcp_copy(failure, "case_id", mcp_get(item, "id"));
        mcp_copy(failure, "status", mcp_get(item, "status"));
        mcp_copy(failure, "input_bytes", mcp_get(item, "input_bytes"));
        hp_json_append(failures, failure);
    }
    mcp_number_add(result, "failure_count", count);
    hp_json_add(result, "next_failure_index", count > 10 ? cJSON_CreateNumber(10) : cJSON_CreateNull());
    const cJSON *target = mcp_get(report, "target");
    mcp_copy(result, "target_version", mcp_get(target, "version_tag"));
    mcp_copy(result, "executable_sha256", mcp_get(mcp_get(target, "executable"), "sha256"));
    mcp_string_add(result, "stopped_reason", job->timed_out ? "run_timeout" : NULL);
    return result;
}

cJSON *mcp_failure(const mcp_config *config, const cJSON *args, char *error) {
    const char *id = mcp_string(mcp_get(args, "report_id"));
    cJSON *report = mcp_report_load(config, id);
    if (!report) { snprintf(error, MCP_ERROR_SIZE, "report is unavailable or invalid"); return NULL; }
    uint64_t index = 0, offset = 0, limit = 256;
    if (mcp_get(args, "index")) mcp_integer(mcp_get(args, "index"), 0, 1023, &index);
    if (mcp_get(args, "offset")) mcp_integer(mcp_get(args, "offset"), 0, HP_MAX_INPUT, &offset);
    if (mcp_get(args, "limit")) mcp_integer(mcp_get(args, "limit"), 1, 4096, &limit);
    const cJSON *selected = NULL, *results = mcp_get(report, "results");
    uint64_t count = 0;
    for (const cJSON *item = results->child; item; item = item->next) {
        if (same_string(item, "status", "pass")) continue;
        if (count++ == index) selected = item;
    }
    cJSON *result = NULL;
    snprintf(error, MCP_ERROR_SIZE, "failure index is outside this report's saved failures");
    if (!selected) goto done;
    uint64_t size = 0;
    mcp_integer(mcp_get(selected, "input_bytes"), 0, HP_MAX_INPUT, &size);
    if (offset > size) { snprintf(error, MCP_ERROR_SIZE, "input offset is outside the saved input"); goto done; }
    uint64_t end = offset + limit < size ? offset + limit : size;
    result = cJSON_CreateObject();
    mcp_string_add(result, "report_id", id);
    mcp_copy(result, "case_id", mcp_get(selected, "id"));
    mcp_number_add(result, "index", (double)index);
    mcp_number_add(result, "failure_count", (double)count);
    hp_json_add(result, "next_failure_index", index + 1 < count ? cJSON_CreateNumber((double)(index + 1)) : cJSON_CreateNull());
    mcp_copy(result, "status", mcp_get(selected, "status"));
    mcp_number_add(result, "input_bytes", (double)size);
    mcp_copy(result, "expected_hex", mcp_get(selected, "expected_hex"));
    mcp_copy(result, "actual_hex", mcp_get(selected, "actual_hex"));
    mcp_number_add(result, "offset", (double)offset);
    const char *hex = mcp_string(mcp_get(selected, "input_hex"));
    char page[8193];
    size_t length = (size_t)(end - offset) * 2;
    memcpy(page, hex + offset * 2, length); page[length] = 0;
    mcp_string_add(result, "input_hex", page);
    hp_json_add(result, "next_offset", end < size ? cJSON_CreateNumber((double)end) : cJSON_CreateNull());
    const cJSON *diagnostic = mcp_get(selected, "error");
    cJSON *exposed = cJSON_IsObject(diagnostic) ? cJSON_CreateObject() : cJSON_CreateNull();
    hp_json_add(result, "error", exposed);
    static const char *const fields[] = {"kind", "detail", "exit_status", "signal", "stdout_hex", "stderr_hex"};
    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++) {
        const cJSON *value = mcp_get(diagnostic, fields[i]);
        if (!value) continue;
        if ((cJSON_IsString(value) && strlen(value->valuestring) <= 1024) ||
            cJSON_IsNull(value) || mcp_integer(value, 0, 2147483647, NULL)) mcp_copy(exposed, fields[i], value);
        else {
            cJSON_Delete(result); result = NULL;
            snprintf(error, MCP_ERROR_SIZE, "invalid saved diagnostic");
            break;
        }
    }
done:
    cJSON_Delete(report);
    return result;
}
