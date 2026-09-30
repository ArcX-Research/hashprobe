#include "mcp.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static mcp_server server;

static void cleanup(void) { mcp_job_cleanup(&server); }

static void usage(void) {
    puts("Hashprobe MCP " HP_VERSION " - SHA-256 testing for agents\n\n"
         "Usage:\n"
         "  hashprobe-mcp [--config FILE]\n"
         "  hashprobe-mcp init [--config FILE] [--name NAME] [--output hex|binary] [-- COMMAND ARGS...]\n"
         "  hashprobe-mcp client-config [--config FILE]\n\n"
         "The default configuration is ~/.config/hashprobe/mcp.json.\n"
         "init defaults to OpenSSL and never replaces an existing file.\n"
         "The server communicates through stdin/stdout; agents start it automatically.");
}

int main(int argc, char **argv) {
    for (int fd = 0; fd < 3; fd++)
        if (fcntl(fd, F_GETFD) < 0 && errno == EBADF && open("/dev/null", O_RDWR) < 0) return 2;
    cJSON_Hooks hooks = {hp_alloc, free};
    cJSON_InitHooks(&hooks);
    server.job.pipe_fd = server.job.lock_fd = -1;
    if (atexit(cleanup)) return 2;
    const char *path = "~/.config/hashprobe/mcp.json", *operation = NULL, *name = NULL, *output = "hex";
    char **command = NULL;
    int output_given = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "help")) { usage(); return 0; }
        if (!strcmp(argv[i], "--version")) { puts("hashprobe-mcp " HP_VERSION); return 0; }
        if (!strcmp(argv[i], "--")) { command = argv + i + 1; break; }
        if (!strcmp(argv[i], "init") || !strcmp(argv[i], "client-config")) {
            if (operation) hp_fatal("provide only one operation");
            operation = argv[i]; continue;
        }
        if (i + 1 == argc) hp_fatal("%s requires a value", argv[i]);
        if (!strcmp(argv[i], "--config")) path = argv[++i];
        else if (!strcmp(argv[i], "--name")) name = argv[++i];
        else if (!strcmp(argv[i], "--output")) { output = argv[++i]; output_given = 1; }
        else hp_fatal("unknown option: %s; use --help", argv[i]);
    }
    int initializing = operation && !strcmp(operation, "init");
    if (!initializing && (command || name || output_given)) hp_fatal("target options belong to init");
    char error[MCP_ERROR_SIZE];
    if (initializing && mcp_initialize(path, name, output, command, error)) hp_fatal("%s", error);
    if (mcp_config_load(&server.config, path, error)) {
        fprintf(stderr, "hashprobe-mcp: %s\n", error);
        mcp_config_free(&server.config);
        return 2;
    }
    int result = 0;
    if (operation) {
        char *program = mcp_self_path(argv[0]);
        if (!program) hp_fatal("cannot find the running executable");
        if (initializing) fprintf(stderr, "Created %s. Add the following to your agent's MCP settings:\n", server.config.path);
        mcp_client_config(program, server.config.path);
        free(program);
    } else if (mcp_mkdirs(server.config.report_dir)) {
        fprintf(stderr, "hashprobe-mcp: cannot create report directory\n");
        result = 2;
    } else {
        server.schemas = mcp_schemas();
        result = mcp_serve(&server);
        cJSON_Delete(server.schemas);
    }
    mcp_config_free(&server.config);
    return result;
}
