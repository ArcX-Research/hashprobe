#include "mcp.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int prepare_config(mcp_config *config, const char *path, char *error) {
    struct stat info;
    if (lstat(path, &info)) {
        if (errno != ENOENT) {
            snprintf(error, MCP_ERROR_SIZE, "cannot inspect configuration: %s", strerror(errno));
            return -1;
        }
        if (mcp_initialize(path, NULL, "binary", NULL, error)) return -1;
        fprintf(stderr, "Created %s\n", path);
    }
    return mcp_config_load(config, path, error);
}

static int run_client(const char *executable, char *const arguments[], char *error) {
    pid_t pid = fork();
    if (pid < 0) {
        snprintf(error, MCP_ERROR_SIZE, "cannot start client: %s", strerror(errno));
        return -1;
    }
    if (!pid) {
        execv(executable, arguments);
        fprintf(stderr, "hashprobe-mcp: cannot start %s: %s\n", arguments[0], strerror(errno));
        _exit(127);
    }
    int status;
    pid_t waited;
    do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        snprintf(error, MCP_ERROR_SIZE, "cannot wait for client: %s", strerror(errno));
        return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
        snprintf(error, MCP_ERROR_SIZE, "%s registration failed; see the client message above", arguments[0]);
        return -1;
    }
    return 0;
}

int mcp_setup(mcp_config *config, const char *client, const char *program, const char *path, char *error) {
    if (strcmp(client, "codex") && strcmp(client, "claude")) {
        snprintf(error, MCP_ERROR_SIZE, "unsupported client; use codex or claude, or client-config for other apps");
        return -1;
    }
    char *cwd = getcwd(NULL, 0);
    char *executable = cwd ? mcp_executable(client, cwd) : NULL;
    char *absolute = cwd ? mcp_absolute(path, cwd) : NULL;
    free(cwd);
    int result = -1;
    if (!executable) {
        snprintf(error, MCP_ERROR_SIZE, "%s is not on PATH; install its CLI first, or use client-config", client);
    } else if (!absolute) {
        snprintf(error, MCP_ERROR_SIZE, "cannot resolve configuration path");
    } else if (!prepare_config(config, absolute, error)) {
        /* Let each client manage its own settings. Pass paths as separate
         * arguments so spaces and shell characters remain ordinary data. */
        char *codex[] = {"codex", "mcp", "add", "hashprobe", "--",
                        (char *)program, "--config", config->path, NULL};
        char *claude[] = {"claude", "mcp", "add", "--scope", "user", "--transport", "stdio",
                         "hashprobe", "--", (char *)program, "--config", config->path, NULL};
        result = run_client(executable, !strcmp(client, "codex") ? codex : claude, error);
        if (!result) {
            printf("Hashprobe is registered for %s across projects.\n", client);
            puts("Restart the client, then ask it to list the Hashprobe targets.");
        }
    }
    free(executable);
    free(absolute);
    return result;
}
