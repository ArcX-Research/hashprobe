#include "hashprobe.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

volatile sig_atomic_t hp_interrupted = 0;

_Noreturn void hp_fatal(const char *format, ...) {
    va_list args;
    fputs("hashprobe: ", stderr);
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
    exit(2);
}

void *hp_alloc(size_t size) {
    void *p = malloc(size ? size : 1);
    if (!p) hp_fatal("out of memory");
    return p;
}

char *hp_hex(const uint8_t *bytes, size_t size) {
    static const char digits[] = "0123456789abcdef";
    char *hex = hp_alloc(size * 2 + 1);
    for (size_t i = 0; i < size; i++) {
        hex[2 * i] = digits[bytes[i] >> 4];
        hex[2 * i + 1] = digits[bytes[i] & 15];
    }
    hex[2 * size] = '\0';
    return hex;
}

static int nibble(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int hp_unhex(const char *hex, uint8_t *bytes, size_t size) {
    if (strlen(hex) != size * 2) return -1;
    for (size_t i = 0; i < size; i++) {
        int a = nibble((unsigned char)hex[i * 2]);
        int b = nibble((unsigned char)hex[i * 2 + 1]);
        if (a < 0 || b < 0) return -1;
        bytes[i] = (uint8_t)((a << 4) | b);
    }
    return 0;
}

double hp_now_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        hp_fatal("cannot read monotonic clock");
    return (double)now.tv_sec * 1000.0 + (double)now.tv_nsec / 1000000.0;
}

int hp_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD);
    return flags < 0 ? -1 : fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

void hp_json_add(cJSON *object, const char *key, cJSON *value) {
    if (!value || !cJSON_AddItemToObject(object, key, value))
        hp_fatal("cannot construct report");
}

void hp_json_append(cJSON *array, cJSON *value) {
    if (!value || !cJSON_AddItemToArray(array, value))
        hp_fatal("cannot construct report");
}
