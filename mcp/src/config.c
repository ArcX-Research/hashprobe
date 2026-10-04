#include "mcp.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *const config_keys[] = {
    "targets", "report_dir", "timeout_ms", "run_timeout_seconds", "max_reports", "max_storage_mb", NULL
};
static const char *const target_keys[] = {"command", "cwd", "output", "description", "version", NULL};

static int setting(const cJSON *json, const char *key, unsigned fallback,
                   unsigned minimum, unsigned maximum, unsigned *out, char *error) {
    const cJSON *value = mcp_get(json, key);
    uint64_t number;
    if (!value) { *out = fallback; return 0; }
    if (!mcp_integer(value, minimum, maximum, &number)) {
        snprintf(error, MCP_ERROR_SIZE, "%s must be an integer between %u and %u", key, minimum, maximum);
        return -1;
    }
    *out = (unsigned)number;
    return 0;
}

static void target_free(mcp_target *target) {
    for (size_t i = 0; i < 128; i++) free(target->command[i]);
    free(target->cwd);
}

static int target_load(mcp_target *target, const char *name, const cJSON *json,
                       const char *base, char *error) {
    snprintf(error, MCP_ERROR_SIZE, "invalid configuration for target %s", name);
    if (!mcp_name(name, 64) || !mcp_keys(json, target_keys)) return -1;
    const cJSON *command = mcp_get(json, "command"), *value;
    if (!cJSON_IsArray(command)) return -1;
    int argc = cJSON_GetArraySize(command);
    if (argc < 1 || argc > 128) return -1;
    target->name = name;
    value = mcp_get(json, "description");
    target->description = value ? mcp_string(value) : "";
    if (!target->description || strlen(target->description) > 512) return -1;
    value = mcp_get(json, "version");
    target->version = mcp_string(value);
    if (value && !cJSON_IsNull(value) && (!target->version || strlen(target->version) > 1024)) return -1;
    value = mcp_get(json, "output");
    const char *output = value ? mcp_string(value) : "hex";
    if (!output || (strcmp(output, "hex") && strcmp(output, "binary"))) return -1;
    target->binary = !strcmp(output, "binary");
    value = mcp_get(json, "cwd");
    const char *directory = value ? mcp_string(value) : ".";
    if (!directory) return -1;
    char *absolute = mcp_absolute(directory, base);
    if (!absolute) return -1;
    target->cwd = realpath(absolute, NULL);
    free(absolute);
    struct stat info;
    if (!target->cwd || stat(target->cwd, &info) || !S_ISDIR(info.st_mode)) {
        snprintf(error, MCP_ERROR_SIZE, "target working directory does not exist: %.300s", directory);
        return -1;
    }
    size_t bytes = 0, index = 0;
    for (const cJSON *arg = command->child; arg; arg = arg->next, index++) {
        const char *text = mcp_string(arg);
        if (!text || (!index && !*text)) return -1;
        bytes += strlen(text) + 1;
        if (bytes > 65536) return -1;
        target->command[index] = index ? strdup(text) : mcp_executable(text, target->cwd);
        if (!target->command[index]) {
            if (index) hp_fatal("out of memory");
            snprintf(error, MCP_ERROR_SIZE, "target program is not executable: %.300s", text);
            return -1;
        }
    }
    return 0;
}

static int target_order(const void *left, const void *right) {
    return strcmp(((const mcp_target *)left)->name, ((const mcp_target *)right)->name);
}

int mcp_config_load(mcp_config *config, const char *path, char *error) {
    memset(config, 0, sizeof *config);
    char *cwd = getcwd(NULL, 0);
    if (!cwd) { snprintf(error, MCP_ERROR_SIZE, "cannot read working directory"); return -1; }
    char *absolute = mcp_absolute(path, cwd);
    free(cwd);
    config->path = absolute ? realpath(absolute, NULL) : NULL;
    free(absolute);
    snprintf(error, MCP_ERROR_SIZE, "cannot read configuration: %.300s", path);
    if (!config->path) return -1;
    config->json = mcp_read_json(config->path, 65536);
    if (!config->json || !mcp_keys(config->json, config_keys)) {
        snprintf(error, MCP_ERROR_SIZE, "configuration must be valid JSON, use known settings, and be at most 65536 bytes");
        return -1;
    }
    if (setting(config->json, "timeout_ms", 5000, 1, 600000, &config->timeout_ms, error) ||
        setting(config->json, "run_timeout_seconds", 120, 1, 600, &config->run_timeout_seconds, error) ||
        setting(config->json, "max_reports", 100, 1, 10000, &config->max_reports, error) ||
        setting(config->json, "max_storage_mb", 256, 40, 4096, &config->max_storage_mb, error)) return -1;
    const cJSON *targets = mcp_get(config->json, "targets");
    if (!cJSON_IsObject(targets) || cJSON_GetArraySize(targets) < 1 || cJSON_GetArraySize(targets) > 32) {
        snprintf(error, MCP_ERROR_SIZE, "configure between 1 and 32 named targets");
        return -1;
    }
    char *base = strdup(config->path);
    if (!base) hp_fatal("out of memory");
    char *slash = strrchr(base, '/');
    if (slash == base) slash[1] = 0; else *slash = 0;
    const cJSON *value = mcp_get(config->json, "report_dir");
    const char *directory = value ? mcp_string(value) : "reports";
    config->report_dir = directory ? mcp_absolute(directory, base) : NULL;
    int result = -1;
    if (!config->report_dir) snprintf(error, MCP_ERROR_SIZE, "invalid report_dir");
    else {
        result = 0;
        for (const cJSON *item = targets->child; item; item = item->next) {
            mcp_target *target = &config->targets[config->target_count++];
            if (target_load(target, item->string, item, base, error)) { result = -1; break; }
        }
    }
    free(base);
    if (!result) qsort(config->targets, config->target_count, sizeof config->targets[0], target_order);
    return result;
}

void mcp_config_free(mcp_config *config) {
    for (size_t i = 0; i < config->target_count; i++) target_free(&config->targets[i]);
    cJSON_Delete(config->json);
    free(config->path);
    free(config->report_dir);
    memset(config, 0, sizeof *config);
}

int mcp_initialize(const char *path, const char *name, const char *output, char **command, char *error) {
    char *defaults[] = {"openssl", "dgst", "-sha256", "-binary", NULL};
    int default_command = !command || !*command;
    if (default_command) {
        command = defaults;
        output = "binary";
        if (!name) name = "openssl";
    } else if (!name) name = "custom";
    cJSON *root = cJSON_CreateObject(), *targets = cJSON_CreateObject();
    cJSON *target_json = cJSON_CreateObject(), *argv = cJSON_CreateArray();
    hp_json_add(root, "targets", targets);
    hp_json_add(targets, name, target_json);
    hp_json_add(target_json, "command", argv);
    for (size_t i = 0; command[i]; i++) hp_json_append(argv, cJSON_CreateString(command[i]));
    char *cwd = getcwd(NULL, 0);
    mcp_string_add(target_json, "cwd", cwd);
    mcp_string_add(target_json, "output", output);
    mcp_target target = {0};
    int result = -1;
    char *absolute = cwd ? mcp_absolute(path, cwd) : NULL;
    char *parent = NULL, *text = NULL;
    if (!cwd || !absolute) snprintf(error, MCP_ERROR_SIZE, "cannot resolve configuration path");
    else if (!target_load(&target, name, target_json, cwd, error)) {
        cJSON_ReplaceItemInArray(argv, 0, cJSON_CreateString(target.command[0]));
        /* OpenSSL needs no project files. Keep it usable after the checkout
         * used during installation has been moved or removed. */
        if (default_command)
            cJSON_ReplaceItemInObjectCaseSensitive(target_json, "cwd", cJSON_CreateString("."));
        text = cJSON_Print(root);
        parent = strdup(absolute);
        if (!text || !parent) hp_fatal("out of memory");
        char *slash = strrchr(parent, '/');
        if (slash == parent) slash[1] = 0; else *slash = 0;
        if (strlen(text) + 1 > 65536) snprintf(error, MCP_ERROR_SIZE, "configuration exceeds 65536 bytes");
        else if (mcp_mkdirs(parent)) snprintf(error, MCP_ERROR_SIZE, "cannot create configuration directory");
        else {
            int fd = open(absolute, O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (fd < 0) snprintf(error, MCP_ERROR_SIZE, "cannot create configuration: %s", strerror(errno));
            else {
                FILE *stream = fdopen(fd, "w");
                int failed = !stream;
                if (stream) {
                    if (fprintf(stream, "%s\n", text) < 0) failed = 1;
                    if (fclose(stream)) failed = 1;
                } else close(fd);
                if (failed) {
                    unlink(absolute);
                    snprintf(error, MCP_ERROR_SIZE, "cannot write configuration");
                } else result = 0;
            }
        }
    }
    target_free(&target);
    cJSON_Delete(root);
    free(cwd); free(absolute); free(parent); free(text);
    return result;
}

void mcp_client_config(const char *program, const char *path) {
    cJSON *root = cJSON_CreateObject(), *servers = cJSON_CreateObject();
    cJSON *server = cJSON_CreateObject(), *args = cJSON_CreateArray();
    hp_json_add(root, "mcpServers", servers);
    hp_json_add(servers, "hashprobe", server);
    mcp_string_add(server, "command", program);
    hp_json_add(server, "args", args);
    hp_json_append(args, cJSON_CreateString("--config"));
    hp_json_append(args, cJSON_CreateString(path));
    char *text = cJSON_Print(root);
    if (!text) hp_fatal("out of memory");
    puts(text);
    free(text);
    cJSON_Delete(root);
}
