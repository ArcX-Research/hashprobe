CC = cc
CFLAGS ?= -O2 -g
CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?=
PYTHON ?= python3
BUILD ?= build
PREFIX ?= /usr/local
WARNINGS = -std=c11 -Wall -Wextra -Wpedantic -Wformat=2 -Wshadow
DEFINES = -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -D_DARWIN_C_SOURCE -DCJSON_NESTING_LIMIT=32
INCLUDES = -Isrc -Ivendor/cjson
CORE_SOURCES = src/run.c src/suite.c src/target.c src/report.c src/util.c src/json.c \
          src/reference/sha256.c vendor/cjson/cJSON.c
SOURCES = src/main.c $(CORE_SOURCES)
MCP_SOURCES = mcp/src/main.c mcp/src/config.c mcp/src/util.c mcp/src/reports.c \
              mcp/src/tools.c mcp/src/protocol.c $(CORE_SOURCES)
HEADERS = src/hashprobe.h src/reference/sha256.h vendor/cjson/cJSON.h

.PHONY: all test test-mcp sanitize install package clean
all: $(BUILD)/hashprobe $(BUILD)/hashprobe-mcp $(BUILD)/sha256-target

$(BUILD):
	mkdir -p "$@"

$(BUILD)/hashprobe: $(SOURCES) $(HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(DEFINES) $(INCLUDES) $(CFLAGS) $(WARNINGS) $(SOURCES) $(LDFLAGS) $(LDLIBS) -o "$@"

$(BUILD)/mcp_schema.h: mcp/tools.json mcp/embed.awk | $(BUILD)
	awk -f mcp/embed.awk mcp/tools.json > "$@"

$(BUILD)/hashprobe-mcp: $(MCP_SOURCES) $(HEADERS) mcp/src/mcp.h $(BUILD)/mcp_schema.h
	$(CC) $(CPPFLAGS) $(DEFINES) $(INCLUDES) -I"$(BUILD)" $(CFLAGS) $(WARNINGS) $(MCP_SOURCES) $(LDFLAGS) $(LDLIBS) -o "$@"

$(BUILD)/sha256-target: examples/sha256_target.c src/reference/sha256.c src/reference/sha256.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(DEFINES) $(INCLUDES) $(CFLAGS) $(WARNINGS) examples/sha256_target.c src/reference/sha256.c $(LDFLAGS) $(LDLIBS) -o "$@"

test: all
	$(PYTHON) tests/test_hashprobe.py --build-dir "$(BUILD)"

test-mcp: all
	HASHPROBE_TEST_BUILD="$(abspath $(BUILD))" $(PYTHON) -m unittest discover -s mcp/tests -v

sanitize:
	$(MAKE) BUILD=build/sanitize CFLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined' test test-mcp

install: $(BUILD)/hashprobe $(BUILD)/hashprobe-mcp
	install -d "$(DESTDIR)$(PREFIX)/bin"
	install -m 755 "$(BUILD)/hashprobe" "$(DESTDIR)$(PREFIX)/bin/hashprobe"
	install -m 755 "$(BUILD)/hashprobe-mcp" "$(DESTDIR)$(PREFIX)/bin/hashprobe-mcp"

package: all
	sh scripts/package.sh "$(BUILD)"

clean:
	rm -rf build
