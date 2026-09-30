CC = cc
CFLAGS ?= -O2 -g
CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?=
PYTHON ?= python3
BUILD ?= build
PREFIX ?= /usr/local
WARNINGS = -std=c11 -Wall -Wextra -Wpedantic -Wformat=2 -Wshadow
DEFINES = -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -DCJSON_NESTING_LIMIT=32
INCLUDES = -Isrc -Ivendor/cjson
SOURCES = src/main.c src/suite.c src/target.c src/report.c src/util.c \
          src/reference/sha256.c vendor/cjson/cJSON.c
HEADERS = src/hashprobe.h src/reference/sha256.h vendor/cjson/cJSON.h

.PHONY: all test test-mcp sanitize install clean
all: $(BUILD)/hashprobe $(BUILD)/sha256-target

$(BUILD):
	mkdir -p "$@"

$(BUILD)/hashprobe: $(SOURCES) $(HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(DEFINES) $(INCLUDES) $(CFLAGS) $(WARNINGS) $(SOURCES) $(LDFLAGS) $(LDLIBS) -o "$@"

$(BUILD)/sha256-target: examples/sha256_target.c src/reference/sha256.c src/reference/sha256.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(DEFINES) $(INCLUDES) $(CFLAGS) $(WARNINGS) examples/sha256_target.c src/reference/sha256.c $(LDFLAGS) $(LDLIBS) -o "$@"

test: all
	$(PYTHON) tests/test_hashprobe.py --build-dir "$(BUILD)"

test-mcp: all
	HASHPROBE_TEST_BUILD="$(abspath $(BUILD))" $(PYTHON) -m unittest discover -s mcp/tests -v

sanitize:
	$(MAKE) BUILD=build/sanitize CFLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined' test

install: $(BUILD)/hashprobe
	install -d "$(DESTDIR)$(PREFIX)/bin"
	install -m 755 "$(BUILD)/hashprobe" "$(DESTDIR)$(PREFIX)/bin/hashprobe"

clean:
	rm -rf build
