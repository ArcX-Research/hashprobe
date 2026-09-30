#include "hashprobe.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static void target_error(hp_result *result, const char *kind, const char *detail) {
    result->status = "error";
    result->error_kind = kind;
    snprintf(result->detail, sizeof result->detail, "%s", detail);
}

static int capture_pipe(int fds[2]) {
    if (pipe(fds) != 0) return -1;
    if (hp_cloexec(fds[0]) || hp_cloexec(fds[1])) return -1;
    int flags = fcntl(fds[0], F_GETFL);
    return flags < 0 ? -1 : fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);
}

static int ascii_space(uint8_t ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' || ch == '\f';
}

void hp_target_run(const hp_case *test, const hp_options *options, hp_result *result) {
    memset(result, 0, sizeof *result);
    result->exit_status = -1;
    result->status = "error";
    double start = hp_now_ms();
    FILE *input = NULL;
    int pipes[2][2] = {{-1, -1}, {-1, -1}};
    uint8_t captured[2][HP_OUTPUT_LIMIT + 1];
    size_t sizes[2] = {0, 0};
    size_t total = 0;
    pid_t pid = -1;
    int status = 0;
    int actions_ready = 0, attributes_ready = 0;
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;

    input = tmpfile();
    if (!input || hp_cloexec(fileno(input)) ||
        fwrite(test->input, 1, test->length, input) != test->length ||
        fseek(input, 0, SEEK_SET) != 0 || capture_pipe(pipes[0]) || capture_pipe(pipes[1])) {
        target_error(result, "io", strerror(errno));
        goto cleanup;
    }
    int rc = posix_spawn_file_actions_init(&actions);
    if (rc) goto spawn_error;
    actions_ready = 1;
    rc = posix_spawnattr_init(&attributes);
    if (rc) goto spawn_error;
    attributes_ready = 1;
    rc = posix_spawn_file_actions_adddup2(&actions, fileno(input), STDIN_FILENO);
    if (rc) goto spawn_error;
    rc = posix_spawn_file_actions_adddup2(&actions, pipes[0][1], STDOUT_FILENO);
    if (rc) goto spawn_error;
    rc = posix_spawn_file_actions_adddup2(&actions, pipes[1][1], STDERR_FILENO);
    if (rc) goto spawn_error;
    /* All original descriptors are close-on-exec; only the dup2 endpoints survive. */
    sigset_t defaults, mask;
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGINT);
    sigaddset(&defaults, SIGTERM);
    sigaddset(&defaults, SIGPIPE);
    sigemptyset(&mask);
    rc = posix_spawnattr_setsigdefault(&attributes, &defaults);
    if (rc) goto spawn_error;
    rc = posix_spawnattr_setsigmask(&attributes, &mask);
    if (rc) goto spawn_error;
    rc = posix_spawnattr_setpgroup(&attributes, 0);
    if (rc) goto spawn_error;
    rc = posix_spawnattr_setflags(&attributes,
        POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);
    if (rc) goto spawn_error;
    rc = posix_spawnp(&pid, options->command[0], &actions, &attributes, options->command, environ);
    if (rc) goto spawn_error;
    for (size_t i = 0; i < 2; i++) {
        close(pipes[i][1]);
        pipes[i][1] = -1;
    }

    for (;;) {
        if (hp_interrupted) {
            target_error(result, "interrupted", "run interrupted by signal");
            break;
        }
        /* Observe the child without reaping it. Its PID cannot be reused before
         * process-group cleanup, even if descendants retain the output pipes. */
        siginfo_t info;
        memset(&info, 0, sizeof info);
        if (waitid(P_PID, (id_t)pid, &info, WEXITED | WNOHANG | WNOWAIT) != 0) {
            if (errno == EINTR) continue;
            target_error(result, "io", strerror(errno));
            break;
        }
        if (info.si_pid == pid && pipes[0][0] < 0 && pipes[1][0] < 0) break;
        double remaining = (double)options->timeout_ms - (hp_now_ms() - start);
        if (remaining <= 0) {
            target_error(result, "timeout", "target exceeded the per-case timeout");
            break;
        }
        struct pollfd events[2] = {
            {pipes[0][0], POLLIN, 0}, {pipes[1][0], POLLIN, 0}
        };
        int wait_ms = remaining > 20 ? 20 : (int)remaining + 1;
        if (pipes[0][0] < 0 && pipes[1][0] < 0) wait_ms = 1;
        int ready = poll(events, 2, wait_ms);
        if (ready < 0) {
            if (errno == EINTR) continue;
            target_error(result, "io", strerror(errno));
            break;
        }
        for (size_t i = 0; i < 2; i++) {
            if (pipes[i][0] < 0 || !events[i].revents) continue;
            uint8_t chunk[4096];
            ssize_t count = read(pipes[i][0], chunk, sizeof chunk);
            if (count > 0) {
                size_t n = (size_t)count;
                size_t copy = n < HP_OUTPUT_LIMIT - sizes[i] ? n : HP_OUTPUT_LIMIT - sizes[i];
                memcpy(captured[i] + sizes[i], chunk, copy);
                sizes[i] += copy;
                total += n;
                if (total > HP_OUTPUT_LIMIT) {
                    target_error(result, "output-limit", "target exceeded 65536 output bytes across stdout and stderr");
                    break;
                }
            } else if (count == 0) {
                close(pipes[i][0]);
                pipes[i][0] = -1;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                target_error(result, "io", strerror(errno));
                break;
            }
        }
        if (result->error_kind) break;
    }
    goto cleanup;

spawn_error:
    target_error(result, "spawn", strerror(rc));

cleanup:
    if (pid > 0) {
        /* Also stop children left behind in this process group. */
        (void)kill(-pid, SIGKILL);
        /* The target may have joined another group. Always stop the direct
         * child too, before the blocking wait; it has not been reaped yet. */
        (void)kill(pid, SIGKILL);
        pid_t waited;
        do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
        if (waited < 0 && !result->error_kind) target_error(result, "io", strerror(errno));
        if (waited > 0 && WIFEXITED(status)) result->exit_status = WEXITSTATUS(status);
        if (waited > 0 && WIFSIGNALED(status)) result->signal_number = WTERMSIG(status);
    }
    if (actions_ready) posix_spawn_file_actions_destroy(&actions);
    if (attributes_ready) posix_spawnattr_destroy(&attributes);
    if (input) fclose(input);
    for (size_t i = 0; i < 2; i++)
        for (size_t j = 0; j < 2; j++)
            if (pipes[i][j] >= 0) close(pipes[i][j]);
    result->duration_ms = hp_now_ms() - start;
    result->stdout_length = sizes[0];
    result->stderr_length = sizes[1];
    memcpy(result->stdout_excerpt, captured[0], sizes[0] < HP_EXCERPT ? sizes[0] : HP_EXCERPT);
    memcpy(result->stderr_excerpt, captured[1], sizes[1] < HP_EXCERPT ? sizes[1] : HP_EXCERPT);
    if (result->error_kind) return;
    if (result->signal_number) {
        target_error(result, "signal", "target terminated by signal");
        return;
    }
    if (result->exit_status != 0) {
        target_error(result, "exit-status", "target returned a nonzero exit status");
        return;
    }
    if (options->binary) {
        if (sizes[0] != 32) {
            target_error(result, "invalid-output", "expected exactly 32 binary digest bytes on stdout");
            return;
        }
        memcpy(result->actual, captured[0], 32);
    } else {
        size_t first = 0, end = sizes[0];
        while (first < end && ascii_space(captured[0][first])) first++;
        while (end > first && ascii_space(captured[0][end - 1])) end--;
        captured[0][end] = '\0';
        if (end - first != 64 || hp_unhex((char *)captured[0] + first, result->actual, 32)) {
            target_error(result, "invalid-output", "expected 64 hexadecimal digest digits on stdout");
            return;
        }
    }
    result->has_actual = 1;
    result->status = memcmp(test->expected, result->actual, 32) ? "mismatch" : "pass";
}
