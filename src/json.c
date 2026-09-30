#include "hashprobe.h"

#include <stdlib.h>
#include <string.h>

int hp_json_integer(const cJSON *value, uint64_t minimum, uint64_t maximum, uint64_t *out) {
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

cJSON *hp_json_parse(const char *data, size_t length) {
    if (!valid_utf8((const unsigned char *)data, length)) return NULL;
    cJSON *numbers = cJSON_CreateArray(), *result = NULL;
    if (lex_json(data, length, numbers)) {
        const char *end = NULL;
        result = cJSON_ParseWithLengthOpts(data, length, &end, 0);
        if (result) while (end < data + length && strchr(" \t\r\n", *end)) end++;
        cJSON *number = numbers->child;
        if (result && (end != data + length || !validate_tree(result, &number) || number)) {
            cJSON_Delete(result);
            result = NULL;
        }
    }
    cJSON_Delete(numbers);
    return result;
}
