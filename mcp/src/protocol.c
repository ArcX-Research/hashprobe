#include "mcp.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t stopped;
static const char instructions[] =
    "Test configured SHA-256 programs. Start with list_targets, then check. "
    "A mismatch is a completed test finding. Use get_failure to inspect saved "
    "inputs and replay after a fix. Only status pass with counts.complete true "
    "confirms all selected tests passed. These tools check digest correctness.";

static cJSON *server_info(void) {
    cJSON *info = cJSON_CreateObject();
    mcp_string_add(info, "name", "hashprobe");
    mcp_string_add(info, "version", HP_VERSION);
    return info;
}

static cJSON *capabilities(void) {
    cJSON *caps = cJSON_CreateObject();
    hp_json_add(caps, "tools", cJSON_CreateObject());
    return caps;
}

static cJSON *versions(void) {
    cJSON *items = cJSON_CreateArray();
    hp_json_append(items, cJSON_CreateString(MCP_MODERN));
    return items;
}

static int valid_id(const cJSON *id) {
    if (!id || !id->valuestring) return 0;
    if (cJSON_IsString(id)) return strlen(id->valuestring) <= 1024;
    if (!cJSON_IsNumber(id)) return 0;
    const char *p = id->valuestring;
    if (*p == '-') p++;
    if (!*p) return 0;
    for (; *p; p++) if (*p < '0' || *p > '9') return 0;
    return 1;
}

static int same_id(const cJSON *left, const cJSON *right) {
    if (!valid_id(left) || !valid_id(right) || cJSON_IsString(left) != cJSON_IsString(right)) return 0;
    if (cJSON_IsNumber(left) && (!strcmp(left->valuestring, "0") || !strcmp(left->valuestring, "-0")))
        return !strcmp(right->valuestring, "0") || !strcmp(right->valuestring, "-0");
    return !strcmp(left->valuestring, right->valuestring);
}

static cJSON *copy_id(const cJSON *id) {
    if (!id || !valid_id(id)) return cJSON_CreateNull();
    /* cJSON's double cannot echo every integer exactly. Emit the validated
     * original integer spelling, never a rounded numeric approximation. */
    return cJSON_IsNumber(id) ? cJSON_CreateRaw(id->valuestring) : cJSON_Duplicate(id, 1);
}

static void send_message(mcp_server *server, cJSON *message) {
    char *text = cJSON_PrintUnformatted(message);
    cJSON_Delete(message);
    if (!text) hp_fatal("out of memory");
    size_t length = strlen(text), pending = server->output_used - server->output_sent;
    if (length + 1 > MCP_QUEUE_LIMIT - pending) {
        fprintf(stderr, "hashprobe-mcp: output queue limit reached\n");
        server->closing = 1;
    } else {
        if (server->output_sent) memmove(server->output, server->output + server->output_sent, pending);
        memcpy(server->output + pending, text, length);
        server->output[pending + length] = '\n';
        server->output_used = pending + length + 1;
        server->output_sent = 0;
    }
    free(text);
}

static void rpc_error(mcp_server *server, const cJSON *id, int code, const char *message, cJSON *data) {
    cJSON *response = cJSON_CreateObject(), *error = cJSON_CreateObject();
    mcp_string_add(response, "jsonrpc", "2.0");
    hp_json_add(response, "id", copy_id(id));
    hp_json_add(response, "error", error);
    mcp_number_add(error, "code", code);
    mcp_string_add(error, "message", message);
    if (data) hp_json_add(error, "data", data);
    send_message(server, response);
}

void mcp_result(mcp_server *server, const cJSON *id, int modern, cJSON *result) {
    if (modern) {
        mcp_string_add(result, "resultType", "complete");
        cJSON *meta = cJSON_CreateObject();
        hp_json_add(meta, "io.modelcontextprotocol/serverInfo", server_info());
        hp_json_add(result, "_meta", meta);
    }
    cJSON *response = cJSON_CreateObject();
    mcp_string_add(response, "jsonrpc", "2.0");
    hp_json_add(response, "id", copy_id(id));
    hp_json_add(response, "result", result);
    send_message(server, response);
}

static void cacheable_result(mcp_server *server, const cJSON *id, int modern, cJSON *result) {
    if (modern) {
        mcp_string_add(result, "cacheScope", "private");
        mcp_number_add(result, "ttlMs", 0);
    }
    mcp_result(server, id, modern, result);
}

void mcp_tool_result(mcp_server *server, const cJSON *id, int modern, cJSON *data, const char *error) {
    cJSON *result = cJSON_CreateObject(), *content = cJSON_CreateArray(), *text = cJSON_CreateObject();
    hp_json_add(result, "content", content);
    hp_json_add(result, "isError", cJSON_CreateBool(error != NULL));
    hp_json_append(content, text);
    mcp_string_add(text, "type", "text");
    char *encoded = data ? cJSON_PrintUnformatted(data) : NULL;
    if (data && !encoded) hp_fatal("out of memory");
    mcp_string_add(text, "text", error ? error : encoded);
    if (data) hp_json_add(result, "structuredContent", data);
    free(encoded);
    mcp_result(server, id, modern, result);
}

static void initialize(mcp_server *server, const cJSON *id, const cJSON *params) {
    const char *version = mcp_string(mcp_get(params, "protocolVersion"));
    const cJSON *client = mcp_get(params, "clientInfo");
    if (server->legacy_started) {
        rpc_error(server, id, -32600, "already initialized", NULL); return;
    }
    if (!version || !cJSON_IsObject(mcp_get(params, "capabilities")) ||
        !mcp_string(mcp_get(client, "name")) || !mcp_string(mcp_get(client, "version"))) {
        rpc_error(server, id, -32602, "invalid initialization parameters", NULL); return;
    }
    if (strcmp(version, MCP_LEGACY) && strcmp(version, "2025-06-18") &&
        strcmp(version, "2025-03-26") && strcmp(version, "2024-11-05")) version = MCP_LEGACY;
    cJSON *result = cJSON_CreateObject();
    mcp_string_add(result, "protocolVersion", version);
    hp_json_add(result, "capabilities", capabilities());
    hp_json_add(result, "serverInfo", server_info());
    mcp_string_add(result, "instructions", instructions);
    server->legacy_started = 1;
    mcp_result(server, id, 0, result);
}

static void call_tool(mcp_server *server, const cJSON *id, int modern, const cJSON *params) {
    const char *name = mcp_string(mcp_get(params, "name"));
    static const char *const keys[] = {"name", "arguments", "_meta", NULL};
    if (!name || !mcp_keys(params, keys)) {
        rpc_error(server, id, -32602, "invalid tools/call parameters", NULL); return;
    }
    const cJSON *definition = NULL, *tools = mcp_get(server->schemas, "tools");
    for (const cJSON *item = tools->child; item; item = item->next)
        if (!strcmp(name, mcp_string(mcp_get(item, "name")))) definition = item;
    if (!definition) {
        rpc_error(server, id, -32602, "unknown tool; use tools/list", NULL); return;
    }
    const cJSON *args = mcp_get(params, "arguments");
    if (args && !cJSON_IsObject(args)) {
        rpc_error(server, id, -32602, "tools/call arguments must be an object", NULL); return;
    }
    cJSON *empty = cJSON_CreateObject();
    if (!args) args = empty;
    char error[MCP_ERROR_SIZE];
    if (mcp_arguments(mcp_get(definition, "inputSchema"), args, error))
        mcp_tool_result(server, id, modern, NULL, error);
    else if (!strcmp(name, "list_targets"))
        mcp_tool_result(server, id, modern, mcp_targets(&server->config), NULL);
    else if (!strcmp(name, "get_failure")) {
        cJSON *result = mcp_failure(&server->config, args, error);
        mcp_tool_result(server, id, modern, result, result ? NULL : error);
    } else if (mcp_start(server, id, modern, name, args, error))
        mcp_tool_result(server, id, modern, NULL, error);
    cJSON_Delete(empty);
}

void mcp_dispatch(mcp_server *server, const char *data, size_t length) {
    cJSON *request = mcp_parse(data, length);
    if (!request) { rpc_error(server, NULL, -32700, "invalid JSON", NULL); return; }
    const cJSON *id = mcp_get(request, "id"), *params = mcp_get(request, "params");
    const char *version = mcp_string(mcp_get(request, "jsonrpc"));
    const char *method = mcp_string(mcp_get(request, "method"));
    if (!cJSON_IsObject(request) || !version || strcmp(version, "2.0") || !method ||
        (id && !valid_id(id)) || (params && !cJSON_IsObject(params)) ||
        mcp_get(request, "result") || mcp_get(request, "error")) {
        rpc_error(server, id, -32600, "invalid JSON-RPC request", NULL); goto done;
    }
    if (!id) {
        if (!strcmp(method, "notifications/initialized") && server->legacy_started)
            server->legacy_ready = 1;
        else if (!strcmp(method, "notifications/cancelled") && server->job.pid &&
                 same_id(mcp_get(params, "requestId"), server->job.id)) {
            server->job.cancelled = 1;
            mcp_job_stop(&server->job);
        }
        /* Notifications, including unknown ones, never execute tools or reply. */
        goto done;
    }
    if (server->job.pid && same_id(id, server->job.id)) {
        rpc_error(server, id, -32600, "request ID is already active", NULL); goto done;
    }
    const cJSON *meta = mcp_get(params, "_meta");
    const cJSON *version_node = mcp_get(meta, "io.modelcontextprotocol/protocolVersion");
    int modern = version_node != NULL;
    if (!modern && !strcmp(method, "initialize")) { initialize(server, id, params); goto done; }
    if (modern) {
        const char *requested = mcp_string(version_node);
        if (!requested || !cJSON_IsObject(mcp_get(meta, "io.modelcontextprotocol/clientCapabilities"))) {
            rpc_error(server, id, -32602, "required MCP request metadata is missing or invalid", NULL); goto done;
        }
        if (strcmp(requested, MCP_MODERN)) {
            cJSON *details = cJSON_CreateObject();
            hp_json_add(details, "supported", versions());
            mcp_string_add(details, "requested", requested);
            rpc_error(server, id, -32022, "unsupported protocol version", details); goto done;
        }
    } else if (!server->legacy_ready) {
        rpc_error(server, id, -32602, "provide MCP request metadata or complete legacy initialization", NULL); goto done;
    }
    if (!strcmp(method, "server/discover") && modern) {
        cJSON *result = cJSON_CreateObject();
        hp_json_add(result, "supportedVersions", versions());
        hp_json_add(result, "capabilities", capabilities());
        mcp_string_add(result, "instructions", instructions);
        cacheable_result(server, id, modern, result);
    } else if (!strcmp(method, "ping")) mcp_result(server, id, modern, cJSON_CreateObject());
    else if (!strcmp(method, "tools/list")) {
        const cJSON *cursor = mcp_get(params, "cursor");
        if (cursor && !cJSON_IsNull(cursor)) rpc_error(server, id, -32602, "unknown tools cursor", NULL);
        else cacheable_result(server, id, modern, cJSON_Duplicate(server->schemas, 1));
    } else if (!strcmp(method, "tools/call")) call_tool(server, id, modern, params);
    else rpc_error(server, id, -32601, "method not found", NULL);
done:
    cJSON_Delete(request);
}

static void stop_handler(int signum) {
    stopped = signum;
}

static int write_pending_output(mcp_server *server) {
    ssize_t sent = write(STDOUT_FILENO, server->output + server->output_sent,
                         server->output_used - server->output_sent);
    if (sent > 0) {
        server->output_sent += (size_t)sent;
    } else if (sent < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
        return -1;
    }
    if (server->output_sent == server->output_used) {
        server->output_sent = 0;
        server->output_used = 0;
    }
    return 0;
}

static int flush_before_exit(mcp_server *server) {
    /* A client may close input and still read its replies. Do not wait forever
     * if it stops reading as well. Standard output remains nonblocking here. */
    double deadline = hp_now_ms() + 250;
    while (server->output_used > server->output_sent && !stopped) {
        double remaining = deadline - hp_now_ms();
        if (remaining <= 0) return -1;
        struct pollfd output = {STDOUT_FILENO, POLLOUT, 0};
        int ready = poll(&output, 1, (int)remaining + 1);
        if (ready < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (output.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
        if ((output.revents & POLLOUT) && write_pending_output(server)) return -1;
    }
    return 0;
}

int mcp_serve(mcp_server *server) {
    struct sigaction action = {0};
    action.sa_handler = stop_handler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, NULL) || sigaction(SIGINT, &action, NULL)) return 2;
    action.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &action, NULL)) return 2;
    int input_flags = fcntl(STDIN_FILENO, F_GETFL);
    int output_flags = fcntl(STDOUT_FILENO, F_GETFL);
    if (input_flags < 0 || output_flags < 0 || fcntl(STDIN_FILENO, F_SETFL, input_flags | O_NONBLOCK) ||
        fcntl(STDOUT_FILENO, F_SETFL, output_flags | O_NONBLOCK)) return 2;
    char *input = hp_alloc(MCP_MESSAGE_LIMIT + 1);
    server->output = hp_alloc(MCP_QUEUE_LIMIT);
    size_t used = 0;
    int dropping = 0, failed = 0;
    while (!server->closing && !stopped) {
        mcp_job_poll(server);
        struct pollfd fds[3] = {
            {STDIN_FILENO, POLLIN, 0},
            {STDOUT_FILENO, server->output_used > server->output_sent ? POLLOUT : 0, 0},
            {server->job.pid ? server->job.pipe_fd : -1, POLLIN, 0}
        };
        int ready = poll(fds, 3, server->job.pid ? 25 : -1);
        if (ready < 0) {
            if (errno == EINTR) continue;
            failed = 1;
            break;
        }
        if (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            failed = 1;
            break;
        }
        if ((fds[1].revents & POLLOUT) && write_pending_output(server)) {
            failed = 1;
            break;
        }
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) {
            /* Bound work per iteration so input flooding cannot starve a
             * runner's deadline, pipe draining, or queued responses. */
            for (unsigned batch = 0; batch < 8 && !server->closing; batch++) {
                char chunk[4096];
                ssize_t got = read(STDIN_FILENO, chunk, sizeof chunk);
                if (got < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) break;
                if (got <= 0) {
                    failed = got < 0;
                    server->closing = 1;
                    break;
                }
                for (ssize_t i = 0; i < got && !server->closing; i++) {
                    if (chunk[i] == '\n') {
                        if (!dropping && used) {
                            input[used] = 0;
                            mcp_dispatch(server, input, used);
                        }
                        used = 0;
                        dropping = 0;
                    } else if (!dropping) {
                        if (used == MCP_MESSAGE_LIMIT) {
                            rpc_error(server, NULL, -32600, "message exceeds 256 KiB", NULL);
                            dropping = 1;
                            used = 0;
                        } else input[used++] = chunk[i];
                    }
                }
            }
        }
    }
    mcp_job_cleanup(server);
    if (!failed && !stopped && flush_before_exit(server)) failed = 1;
    fcntl(STDIN_FILENO, F_SETFL, input_flags);
    fcntl(STDOUT_FILENO, F_SETFL, output_flags);
    free(input);
    free(server->output);
    server->output = NULL;
    return failed ? 2 : 0;
}
