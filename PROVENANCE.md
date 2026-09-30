# Code origins

## SHA-256 reference

These files were copied unchanged from the original research workspace:

| File | Original location |
| --- | --- |
| [src/reference/sha256.c](src/reference/sha256.c) | `sha256/c/sha256.c` |
| [src/reference/sha256.h](src/reference/sha256.h) | `sha256/c/sha256.h` |

Hashprobe uses this code to calculate expected hashes. The four known answers came from `sha256/python/test_sha256.py` in that workspace: an empty input, `abc`, a 56-byte message, and one million `a` bytes. Hashprobe generates the remaining inputs.

## JSON library

[cJSON 1.7.19](https://github.com/DaveGamble/cJSON/releases/tag/v1.7.19) reads and writes reports, configuration files, and MCP messages. Its source is included unchanged in [vendor/cjson](vendor/cjson), with its [original MIT license](vendor/cjson/LICENSE).

## Hashprobe code

The command-line interface, test generator, program runner, report handling, and JSON validation were written for Hashprobe. They live in [src](src), with [examples](examples) and [tests](tests).

The [MCP server](mcp/src) shares the same test engine and uses the [stdio protocol](https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/stdio). Its tool definitions are in [mcp/tools.json](mcp/tools.json).

The [official MCP Python SDK](https://github.com/modelcontextprotocol/python-sdk) is used only for tests. Its version is pinned in [mcp/tests/requirements.txt](mcp/tests/requirements.txt).

Hashprobe and its copied SHA-256 code use the project [MIT license](LICENSE). cJSON keeps its original copyright notice and license.

## Verify the copied files

[CHECKSUMS.sha256](CHECKSUMS.sha256) records the hashes of the copied SHA-256 and cJSON files. From the Hashprobe folder, run:

```sh
shasum -a 256 -c CHECKSUMS.sha256
```

Every entry should say `OK`.
