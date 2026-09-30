# Where the code comes from

## SHA-256 reference

These files were copied unchanged from the original research workspace:

| File | Original location |
| --- | --- |
| [src/reference/sha256.c](src/reference/sha256.c) | `sha256/c/sha256.c` |
| [src/reference/sha256.h](src/reference/sha256.h) | `sha256/c/sha256.h` |

Hashprobe uses this code to calculate expected hashes. The four known answers came from `sha256/python/test_sha256.py` in the research workspace: an empty input, `abc`, a 56-byte message, and one million `a` bytes. Hashprobe generates the other inputs.

## JSON library

[cJSON 1.7.19](https://github.com/DaveGamble/cJSON/releases/tag/v1.7.19) reads and writes reports, configuration files, and MCP messages. Its source and [MIT license](vendor/cjson/LICENSE) are included unchanged in [vendor/cjson](vendor/cjson).

## Hashprobe code

The command-line interface, test generator, program runner, reports, and replay code were written for Hashprobe. They are in [src](src), with [examples](examples) and [tests](tests).

The C server in [mcp/src](mcp/src) implements the [MCP stdio protocol](https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/stdio) and shares the command-line tool's test engine. Tool definitions are in [mcp/tools.json](mcp/tools.json).

The [official MCP Python SDK](https://github.com/modelcontextprotocol/python-sdk) is used only as a test client. Its version is pinned in [mcp/tests/requirements.txt](mcp/tests/requirements.txt).

Hashprobe and its copied SHA-256 code use the project [MIT license](LICENSE). cJSON retains its original copyright notice and license.

## Verify the copied files

[CHECKSUMS.sha256](CHECKSUMS.sha256) records the original files' hashes. From the Hashprobe folder, run:

```sh
shasum -a 256 -c CHECKSUMS.sha256
```

Every entry should say `OK`.
