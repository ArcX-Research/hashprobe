#ifndef HASHPROBE_MCP_H
#define HASHPROBE_MCP_H

#include "hashprobe.h"
#include <sys/types.h>

#define MCP_MESSAGE_LIMIT (256u * 1024u)
#define MCP_QUEUE_LIMIT (1024u * 1024u)
#define MCP_ERROR_SIZE 512
#define MCP_MODERN "2026-07-28"
#define MCP_LEGACY "2025-11-25"

typedef struct {
    const char *name, *description, *version;
    char *cwd;
    char *command[129];
    int binary;
} mcp_target;

typedef struct {
    cJSON *json;
    char *path, *report_dir;
    mcp_target targets[32];
    size_t target_count;
    unsigned timeout_ms, run_timeout_seconds, max_reports, max_storage_mb;
} mcp_config;

typedef struct {
    pid_t pid;
    int pipe_fd, lock_fd;
    cJSON *id;
    int modern, cancelled, timed_out, terminating;
    double deadline, kill_deadline;
    char report_id[33];
    const mcp_target *target;
    char diagnostics[1024];
    size_t diagnostic_length;
} mcp_job;

typedef struct {
    mcp_config config;
    mcp_job job;
    cJSON *schemas;
    int legacy_ready, legacy_started, closing;
    char *output;
    size_t output_used, output_sent;
} mcp_server;

/* JSON helpers own values added to an object; copies are explicit. */
const cJSON *mcp_get(const cJSON *object, const char *name);
const char *mcp_string(const cJSON *value);
void mcp_string_add(cJSON *object, const char *key, const char *value);
void mcp_number_add(cJSON *object, const char *key, double value);
void mcp_copy(cJSON *object, const char *key, const cJSON *value);
int mcp_integer(const cJSON *value, uint64_t minimum, uint64_t maximum, uint64_t *out);
int mcp_keys(const cJSON *object, const char *const *allowed);
int mcp_name(const char *text, size_t maximum);
int mcp_hex(const char *text, size_t length);
cJSON *mcp_parse(const char *data, size_t length);
cJSON *mcp_read_json(const char *path, size_t limit);
char *mcp_join(const char *base, const char *name);
char *mcp_absolute(const char *path, const char *base);
char *mcp_executable(const char *program, const char *cwd);
int mcp_mkdirs(const char *path);
char *mcp_self_path(const char *argv0);

int mcp_config_load(mcp_config *config, const char *path, char *error);
void mcp_config_free(mcp_config *config);
int mcp_initialize(const char *path, const char *name, const char *output, char **command, char *error);
void mcp_client_config(const char *program, const char *path);

int mcp_report_id(const char *id);
char *mcp_report_path(const mcp_config *config, const char *id);
cJSON *mcp_report_load(const mcp_config *config, const char *id);
int mcp_reserve(const mcp_config *config, char *error);
int mcp_random_id(char id[33]);
cJSON *mcp_summary(const cJSON *report, const mcp_job *job);
cJSON *mcp_failure(const mcp_config *config, const cJSON *args, char *error);

cJSON *mcp_schemas(void);
int mcp_arguments(const cJSON *schema, const cJSON *args, char *error);
cJSON *mcp_targets(const mcp_config *config);
int mcp_start(mcp_server *server, const cJSON *id, int modern, const char *tool,
              const cJSON *args, char *error);
void mcp_job_poll(mcp_server *server);
void mcp_job_stop(mcp_job *job);
void mcp_job_cleanup(mcp_server *server);

void mcp_result(mcp_server *server, const cJSON *id, int modern, cJSON *result);
void mcp_tool_result(mcp_server *server, const cJSON *id, int modern, cJSON *data, const char *error);
void mcp_dispatch(mcp_server *server, const char *data, size_t length);
int mcp_serve(mcp_server *server);

#endif
