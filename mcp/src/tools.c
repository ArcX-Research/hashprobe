#include "mcp.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "mcp_schema.h"

cJSON *mcp_schemas(void) {
    size_t size = 0;
    for (size_t i = 0; schema_lines[i]; i++) size += strlen(schema_lines[i]);
    char *text = hp_alloc(size + 1), *cursor = text;
    for (size_t i = 0; schema_lines[i]; i++) {
        size_t length = strlen(schema_lines[i]);
        memcpy(cursor, schema_lines[i], length);
        cursor += length;
    }
    *cursor = 0;
    cJSON *result = mcp_parse(text, size);
    free(text);
    if (!result) hp_fatal("invalid compiled tool schemas");
    return result;
}

/* The four public input schemas use only these scalar forms. This is not a
 * general JSON Schema evaluator: new schema forms must add validation here. */
static int scalar_matches(const cJSON *schema, const cJSON *value) {
    const cJSON *alternatives = mcp_get(schema, "anyOf");
    if (cJSON_IsArray(alternatives)) {
        for (const cJSON *item = alternatives->child; item; item = item->next)
            if (scalar_matches(item, value)) return 1;
        return 0;
    }
    const char *type = mcp_string(mcp_get(schema, "type"));
    if (!type) return 0;
    if (!strcmp(type, "null")) return cJSON_IsNull(value);
    if (!strcmp(type, "boolean")) return cJSON_IsBool(value);
    if (!strcmp(type, "integer")) {
        uint64_t minimum = 0, maximum = UINT32_MAX;
        mcp_integer(mcp_get(schema, "minimum"), 0, UINT32_MAX, &minimum);
        mcp_integer(mcp_get(schema, "maximum"), 0, UINT32_MAX, &maximum);
        return mcp_integer(value, minimum, maximum, NULL);
    }
    if (!strcmp(type, "string")) {
        const char *text = mcp_string(value), *pattern = mcp_string(mcp_get(schema, "pattern"));
        if (!text) return 0;
        if (!pattern) return 1;
        if (!strcmp(pattern, "^[0-9a-f]{32}$")) return mcp_report_id(text);
        if (!strcmp(pattern, "^[a-zA-Z0-9_-]{1,64}$")) return mcp_name(text, 64);
        if (!strcmp(pattern, "^[a-zA-Z0-9_-]{1,79}$")) return mcp_name(text, 79);
    }
    return 0;
}

int mcp_arguments(const cJSON *schema, const cJSON *args, char *error) {
    if (!cJSON_IsObject(args)) {
        snprintf(error, MCP_ERROR_SIZE, "arguments must be an object"); return -1;
    }
    const cJSON *properties = mcp_get(schema, "properties"), *required = mcp_get(schema, "required");
    for (const cJSON *item = required ? required->child : NULL; item; item = item->next) {
        if (!mcp_get(args, item->valuestring)) {
            snprintf(error, MCP_ERROR_SIZE, "missing argument: %s", item->valuestring); return -1;
        }
    }
    for (const cJSON *item = args->child; item; item = item->next) {
        const cJSON *field = mcp_get(properties, item->string);
        if (!field || !scalar_matches(field, item)) {
            snprintf(error, MCP_ERROR_SIZE, "invalid or unknown argument: %.100s", item->string); return -1;
        }
    }
    return 0;
}

cJSON *mcp_targets(const mcp_config *config) {
    cJSON *result = cJSON_CreateObject(), *targets = cJSON_CreateArray();
    hp_json_add(result, "targets", targets);
    mcp_number_add(result, "timeout_ms", config->timeout_ms);
    mcp_number_add(result, "run_timeout_seconds", config->run_timeout_seconds);
    for (size_t i = 0; i < config->target_count; i++) {
        const mcp_target *target = &config->targets[i];
        cJSON *item = cJSON_CreateObject();
        mcp_string_add(item, "name", target->name);
        mcp_string_add(item, "description", target->description);
        mcp_string_add(item, "output", target->binary ? "binary" : "hex");
        mcp_string_add(item, "version", target->version);
        hp_json_append(targets, item);
    }
    return result;
}

static unsigned argument(const cJSON *args, const char *name, unsigned fallback) {
    const cJSON *value = mcp_get(args, name);
    uint64_t number;
    return mcp_integer(value, 0, UINT32_MAX, &number) ? (unsigned)number : fallback;
}

static int has_failure(const cJSON *report, const char *case_id) {
    const cJSON *results = mcp_get(report, "results");
    for (const cJSON *item = results->child; item; item = item->next) {
        const char *status = mcp_string(mcp_get(item, "status"));
        const char *id = mcp_string(mcp_get(item, "id"));
        if (strcmp(status, "pass") && (!case_id || !strcmp(case_id, id))) return 1;
    }
    return 0;
}

int mcp_start(mcp_server *server, const cJSON *id, int modern, const char *tool,
              const cJSON *args, char *error) {
    if (server->job.pid) {
        snprintf(error, MCP_ERROR_SIZE, "another test run is active; retry when it finishes"); return -1;
    }
    const char *name = mcp_string(mcp_get(args, "target"));
    const mcp_target *target = NULL;
    for (size_t i = 0; i < server->config.target_count; i++)
        if (!strcmp(name, server->config.targets[i].name)) target = &server->config.targets[i];
    if (!target) {
        snprintf(error, MCP_ERROR_SIZE, "unknown target; call list_targets to see the configured programs"); return -1;
    }
    unsigned timeout = argument(args, "timeout_ms", server->config.timeout_ms);
    if (timeout > server->config.timeout_ms) {
        snprintf(error, MCP_ERROR_SIZE, "timeout_ms exceeds the configured limit %u", server->config.timeout_ms); return -1;
    }
    char *source = NULL;
    const char *case_id = mcp_string(mcp_get(args, "case_id"));
    if (!strcmp(tool, "replay")) {
        const char *source_id = mcp_string(mcp_get(args, "report_id"));
        cJSON *report = mcp_report_load(&server->config, source_id);
        if (!report) { snprintf(error, MCP_ERROR_SIZE, "report is unavailable or invalid"); return -1; }
        int found = has_failure(report, case_id);
        cJSON_Delete(report);
        if (!found) { snprintf(error, MCP_ERROR_SIZE, "report has no matching saved failure"); return -1; }
        source = mcp_report_path(&server->config, source_id);
    }
    mcp_job job = {.pipe_fd = -1, .lock_fd = -1, .target = target, .modern = modern};
    job.lock_fd = mcp_reserve(&server->config, error);
    if (job.lock_fd < 0) { free(source); return -1; }
    int pipes[2] = {-1, -1};
    char *destination = NULL;
    int result = -1;
    if (mcp_random_id(job.report_id) || pipe(pipes)) {
        snprintf(error, MCP_ERROR_SIZE, "cannot prepare test runner"); goto done;
    }
    if (hp_cloexec(pipes[0]) || hp_cloexec(pipes[1]) ||
        fcntl(pipes[0], F_SETFL, O_NONBLOCK)) {
        snprintf(error, MCP_ERROR_SIZE, "cannot configure runner pipe"); goto done;
    }
    destination = mcp_report_path(&server->config, job.report_id);
    job.id = cJSON_Duplicate(id, 1);
    if (!job.id) hp_fatal("out of memory");
    hp_options options = {
        .mode = tool, .report_path = destination, .source_path = source, .case_id = case_id,
        .target_version = target->version, .command = (char **)target->command,
        .seed = argument(args, "seed", 0), .random_cases = argument(args, "random_cases", 32),
        .max_bytes = argument(args, "max_bytes", 4096), .timeout_ms = timeout,
        .binary = target->binary, .fail_fast = cJSON_IsTrue(mcp_get(args, "fail_fast"))
    };
    sigset_t blocked, previous;
    sigemptyset(&blocked); sigaddset(&blocked, SIGTERM); sigaddset(&blocked, SIGINT);
    if (sigprocmask(SIG_BLOCK, &blocked, &previous)) {
        snprintf(error, MCP_ERROR_SIZE, "cannot protect runner startup"); goto done;
    }
    pid_t pid = fork();
    if (pid == 0) {
        close(pipes[0]); close(job.lock_fd);
        int input = open("/dev/null", O_RDONLY);
        if (input < 0 || dup2(input, STDIN_FILENO) < 0 ||
            dup2(pipes[1], STDOUT_FILENO) < 0 || dup2(pipes[1], STDERR_FILENO) < 0) _exit(2);
        close(input); close(pipes[1]);
        if (chdir(target->cwd)) { perror("hashprobe-mcp: target cwd"); _exit(2); }
        /* hp_run installs its own handlers before unblocking signals. */
        int code = hp_run(&options);
        fflush(NULL);
        _exit(code);
    }
    sigprocmask(SIG_SETMASK, &previous, NULL);
    if (pid < 0) { snprintf(error, MCP_ERROR_SIZE, "cannot start test runner: %s", strerror(errno)); goto done; }
    close(pipes[1]); pipes[1] = -1;
    job.pid = pid;
    job.pipe_fd = pipes[0]; pipes[0] = -1;
    job.deadline = hp_now_ms() + server->config.run_timeout_seconds * 1000.0;
    server->job = job;
    job.lock_fd = -1;
    job.id = NULL;
    result = 0;
done:
    if (pipes[0] >= 0) close(pipes[0]);
    if (pipes[1] >= 0) close(pipes[1]);
    if (job.lock_fd >= 0) close(job.lock_fd);
    cJSON_Delete(job.id);
    free(source); free(destination);
    return result;
}

void mcp_job_stop(mcp_job *job) {
    if (!job->pid || job->terminating) return;
    job->terminating = 1;
    job->kill_deadline = hp_now_ms() + 3000;
    kill(job->pid, SIGTERM);
}

void mcp_job_poll(mcp_server *server) {
    mcp_job *job = &server->job;
    if (!job->pid) return;
    if (job->pipe_fd >= 0) {
        char buffer[4096];
        for (;;) {
            ssize_t got = read(job->pipe_fd, buffer, sizeof buffer);
            if (got < 0 && errno == EINTR) continue;
            if (got <= 0) {
                if (!got || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                    close(job->pipe_fd); job->pipe_fd = -1;
                }
                break;
            }
            size_t keep = sizeof job->diagnostics - 1 - job->diagnostic_length;
            if (keep > (size_t)got) keep = (size_t)got;
            memcpy(job->diagnostics + job->diagnostic_length, buffer, keep);
            job->diagnostic_length += keep;
            job->diagnostics[job->diagnostic_length] = 0;
        }
    }
    int status;
    pid_t finished = waitpid(job->pid, &status, WNOHANG);
    if (finished < 0 && errno == EINTR) return;
    if (!finished) {
        double now = hp_now_ms();
        if (!job->terminating && now >= job->deadline) {
            job->timed_out = 1;
            mcp_job_stop(job);
        }
        if (job->terminating && now >= job->kill_deadline) kill(job->pid, SIGKILL);
        return;
    }
    if (!job->cancelled && !server->closing) {
        cJSON *report = mcp_report_load(&server->config, job->report_id);
        if (report) {
            mcp_tool_result(server, job->id, job->modern, mcp_summary(report, job), NULL);
            cJSON_Delete(report);
        } else {
            char error[MCP_ERROR_SIZE];
            snprintf(error, sizeof error, "Hashprobe did not save a report: %.430s",
                     job->diagnostic_length ? job->diagnostics : "run stopped before a report was written");
            mcp_tool_result(server, job->id, job->modern, NULL, error);
        }
    }
    if (job->pipe_fd >= 0) close(job->pipe_fd);
    if (job->lock_fd >= 0) close(job->lock_fd);
    cJSON_Delete(job->id);
    memset(job, 0, sizeof *job);
    job->pipe_fd = job->lock_fd = -1;
}

void mcp_job_cleanup(mcp_server *server) {
    server->closing = 1;
    mcp_job_stop(&server->job);
    while (server->job.pid) {
        mcp_job_poll(server);
        struct timespec pause = {0, 10000000};
        if (server->job.pid) nanosleep(&pause, NULL);
    }
}
