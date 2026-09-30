#include "mcp.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

const cJSON *mcp_get(const cJSON *object, const char *name) {
    return cJSON_GetObjectItemCaseSensitive(object, name);
}

const char *mcp_string(const cJSON *value) {
    return cJSON_IsString(value) ? value->valuestring : NULL;
}

void mcp_string_add(cJSON *object, const char *key, const char *value) {
    hp_json_add(object, key, value ? cJSON_CreateString(value) : cJSON_CreateNull());
}

void mcp_number_add(cJSON *object, const char *key, double value) {
    hp_json_add(object, key, cJSON_CreateNumber(value));
}

void mcp_copy(cJSON *object, const char *key, const cJSON *value) {
    hp_json_add(object, key, value ? cJSON_Duplicate(value, 1) : cJSON_CreateNull());
}

int mcp_integer(const cJSON *value, uint64_t minimum, uint64_t maximum, uint64_t *out) {
    if (!cJSON_IsNumber(value) || !value->valuestring || !*value->valuestring) return 0;
    uint64_t number = 0;
    for (const char *p = value->valuestring; *p; p++) {
        if (*p < '0' || *p > '9') return 0;
        unsigned digit = (unsigned)(*p - '0');
        if (number > maximum / 10 || (number == maximum / 10 && digit > maximum % 10)) return 0;
        number = number * 10 + digit;
    }
    if (number < minimum) return 0;
    if (out) *out = number;
    return 1;
}

int mcp_keys(const cJSON *object, const char *const *allowed) {
    if (!cJSON_IsObject(object)) return 0;
    for (const cJSON *item = object->child; item; item = item->next) {
        size_t i = 0;
        while (allowed[i] && strcmp(item->string, allowed[i])) i++;
        if (!allowed[i]) return 0;
    }
    return 1;
}

int mcp_name(const char *text, size_t maximum) {
    if (!text || !*text || strlen(text) > maximum) return 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return 0;
    return 1;
}

int mcp_hex(const char *text, size_t length) {
    if (!text || strlen(text) != length) return 0;
    for (size_t i = 0; i < length; i++)
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f') ||
              (text[i] >= 'A' && text[i] <= 'F'))) return 0;
    return 1;
}

static int valid_utf8(const unsigned char *p, size_t length) {
    const unsigned char *end = p + length;
    while (p < end) {
        unsigned code = *p++;
        if (code < 128) { if (!code) return 0; continue; }
        unsigned count, minimum;
        if (code >= 0xc2 && code <= 0xdf) { count = 1; minimum = 0x80; code &= 0x1f; }
        else if (code >= 0xe0 && code <= 0xef) { count = 2; minimum = 0x800; code &= 0x0f; }
        else if (code >= 0xf0 && code <= 0xf4) { count = 3; minimum = 0x10000; code &= 7; }
        else return 0;
        if ((size_t)(end - p) < count) return 0;
        while (count--) {
            if ((*p & 0xc0) != 0x80) return 0;
            code = (code << 6) | (*p++ & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return 0;
    }
    return 1;
}

/* cJSON accepts some non-JSON numbers and stores numbers as doubles. Validate
 * their spelling first and retain it for exact request IDs and strict integers.
 * Embedded NUL strings cannot be represented by cJSON and are rejected. */
static int lex_json(const char *data, size_t length, cJSON *numbers) {
    size_t i = 0, values = 0;
    while (i < length) {
        unsigned char c = (unsigned char)data[i++];
        /* Bound allocations before cJSON builds its tree. This also counts
         * object keys, leaving ample room for a full 1024-case report. */
        if (strchr("\"{[tfn-", c) || (c >= '0' && c <= '9'))
            if (++values > 65536) return 0;
        if (c == '"') {
            int closed = 0;
            while (i < length) {
                c = (unsigned char)data[i++];
                if (c == '"') { closed = 1; break; }
                if (c < 32) return 0;
                if (c != '\\') continue;
                if (i == length) return 0;
                c = (unsigned char)data[i++];
                if (c == 'u') {
                    if (length - i < 4) return 0;
                    for (size_t j = 0; j < 4; j++)
                        if (!strchr("0123456789abcdefABCDEF", data[i + j])) return 0;
                    if (!memcmp(data + i, "0000", 4)) return 0;
                    i += 4;
                } else if (!strchr("\"\\/bfnrt", c)) return 0;
            }
            if (!closed) return 0;
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            size_t start = i - 1;
            i = start;
            if (data[i] == '-') i++;
            if (i == length) return 0;
            if (data[i] == '0') i++;
            else {
                if (data[i] < '1' || data[i] > '9') return 0;
                while (i < length && data[i] >= '0' && data[i] <= '9') i++;
            }
            if (i < length && data[i] == '.') {
                i++;
                size_t begin = i;
                while (i < length && data[i] >= '0' && data[i] <= '9') i++;
                if (begin == i) return 0;
            }
            if (i < length && (data[i] == 'e' || data[i] == 'E')) {
                i++;
                if (i < length && (data[i] == '+' || data[i] == '-')) i++;
                size_t begin = i;
                while (i < length && data[i] >= '0' && data[i] <= '9') i++;
                if (begin == i) return 0;
            }
            if (i < length && !strchr(" \t\r\n,]}", data[i])) return 0;
            char *token = hp_alloc(i - start + 1);
            memcpy(token, data + start, i - start);
            token[i - start] = 0;
            cJSON *item = cJSON_CreateString("");
            free(item->valuestring);
            item->valuestring = token;
            hp_json_append(numbers, item);
        } else if (c < 32 && !strchr(" \t\r\n", c)) return 0;
    }
    return 1;
}

static int validate_tree(cJSON *node, cJSON **number) {
    if (cJSON_IsNumber(node)) {
        if (!*number) return 0;
        node->valuestring = (*number)->valuestring;
        (*number)->valuestring = NULL;
        *number = (*number)->next;
    }
    unsigned count = 0;
    for (cJSON *item = node->child; item; item = item->next) {
        if (cJSON_IsObject(node)) {
            if (++count > 128) return 0;
            for (cJSON *before = node->child; before != item; before = before->next)
                if (!strcmp(before->string, item->string)) return 0;
        }
        if (!validate_tree(item, number)) return 0;
    }
    return 1;
}

cJSON *mcp_parse(const char *data, size_t length) {
    if (!valid_utf8((const unsigned char *)data, length)) return NULL;
    cJSON *numbers = cJSON_CreateArray(), *result = NULL;
    if (lex_json(data, length, numbers)) {
        result = cJSON_ParseWithLengthOpts(data, length + 1, NULL, 1);
        cJSON *number = numbers->child;
        if (result && (!validate_tree(result, &number) || number)) {
            cJSON_Delete(result);
            result = NULL;
        }
    }
    cJSON_Delete(numbers);
    return result;
}

cJSON *mcp_read_json(const char *path, size_t limit) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return NULL;
    struct stat info;
    if (hp_cloexec(fd) || fstat(fd, &info) || !S_ISREG(info.st_mode) ||
        info.st_size < 0 || (uintmax_t)info.st_size > limit) { close(fd); return NULL; }
    size_t length = (size_t)info.st_size, used = 0;
    char *data = hp_alloc(length + 1);
    while (used < length) {
        ssize_t got = read(fd, data + used, length - used);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        used += (size_t)got;
    }
    char extra;
    ssize_t got;
    do { got = read(fd, &extra, 1); } while (got < 0 && errno == EINTR);
    close(fd);
    data[length] = 0;
    cJSON *result = used == length && got == 0 ? mcp_parse(data, length) : NULL;
    free(data);
    return result;
}

char *mcp_join(const char *base, const char *name) {
    size_t size = strlen(base) + strlen(name) + 2;
    char *result = hp_alloc(size);
    snprintf(result, size, "%s/%s", base, name);
    return result;
}

char *mcp_absolute(const char *path, const char *base) {
    char *joined;
    if (path[0] == '~' && (!path[1] || path[1] == '/')) {
        const char *home_dir = getenv("HOME");
        if (!home_dir || home_dir[0] != '/') return NULL;
        joined = mcp_join(home_dir, path + (path[1] ? 2 : 1));
    } else joined = path[0] == '/' ? strdup(path) : mcp_join(base, path);
    if (!joined) hp_fatal("out of memory");
    char *result = hp_alloc(strlen(joined) + 2), *state = NULL;
    size_t used = 0;
    result[0] = 0;
    for (char *part = strtok_r(joined, "/", &state); part; part = strtok_r(NULL, "/", &state)) {
        if (!strcmp(part, ".")) continue;
        if (!strcmp(part, "..")) {
            while (used && result[--used] != '/') {}
            result[used] = 0;
        } else {
            result[used++] = '/';
            size_t size = strlen(part);
            memcpy(result + used, part, size + 1);
            used += size;
        }
    }
    if (!used) strcpy(result, "/");
    free(joined);
    return result;
}

static int executable_file(const char *path) {
    struct stat info;
    return path && !access(path, X_OK) && !stat(path, &info) && S_ISREG(info.st_mode);
}

char *mcp_executable(const char *program, const char *cwd) {
    if (strchr(program, '/') || program[0] == '~') {
        char *path = mcp_absolute(program, cwd);
        if (executable_file(path)) return path;
        free(path);
        return NULL;
    }
    const char *search = getenv("PATH");
    if (!search) search = "/usr/bin:/bin";
    for (const char *start = search;;) {
        const char *end = strchr(start, ':');
        size_t length = end ? (size_t)(end - start) : strlen(start);
        char *dir = hp_alloc(length + 1);
        memcpy(dir, start, length); dir[length] = 0;
        char *base = mcp_absolute(*dir ? dir : ".", cwd);
        char *path = base ? mcp_join(base, program) : NULL;
        free(base); free(dir);
        if (executable_file(path)) return path;
        free(path);
        if (!end) return NULL;
        start = end + 1;
    }
}

int mcp_mkdirs(const char *path) {
    char *copy = strdup(path);
    if (!copy) hp_fatal("out of memory");
    int result = 0;
    for (char *p = copy + 1;; p++) {
        if (*p && *p != '/') continue;
        char saved = *p; *p = 0;
        struct stat info;
        if ((mkdir(copy, 0700) && errno != EEXIST) || stat(copy, &info) || !S_ISDIR(info.st_mode)) {
            result = -1; break;
        }
        *p = saved;
        if (!saved) break;
    }
    free(copy);
    return result;
}

char *mcp_self_path(const char *argv0) {
    char path[4096];
#ifdef __APPLE__
    uint32_t size = sizeof path;
    if (!_NSGetExecutablePath(path, &size)) return realpath(path, NULL);
#else
    ssize_t size = readlink("/proc/self/exe", path, sizeof path - 1);
    if (size > 0 && (size_t)size < sizeof path - 1) { path[size] = 0; return realpath(path, NULL); }
#endif
    char *cwd = getcwd(NULL, 0);
    if (!cwd) return NULL;
    char *result = mcp_executable(argv0, cwd);
    free(cwd);
    return result;
}
