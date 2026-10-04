# Code origins

## SHA-256 reference

These files were copied unchanged from the original research workspace:

| File | Original location |
| --- | --- |
| [src/reference/sha256.c](src/reference/sha256.c) | `sha256/c/sha256.c` |
| [src/reference/sha256.h](src/reference/sha256.h) | `sha256/c/sha256.h` |

Hashprobe uses this code to calculate expected hashes. Four known-answer tests came from `sha256/python/test_sha256.py` in that workspace: empty input, `abc`, a 56-byte message, and one million `a` bytes. Hashprobe generates the remaining inputs.

## Included library

[cJSON 1.7.19](https://github.com/DaveGamble/cJSON/releases/tag/v1.7.19) handles JSON reports, settings, and MCP messages. Its source is unchanged in [vendor/cjson](vendor/cjson), with its [original MIT license](vendor/cjson/LICENSE).

## Project code

The CLI, test engine, program runner, and report handling were written for Hashprobe in [src](src). The [MCP server](mcp/src) uses the same engine.

The [official MCP Python SDK](https://github.com/modelcontextprotocol/python-sdk) is used only for tests. Its version is pinned in [mcp/tests/requirements.txt](mcp/tests/requirements.txt).

Hashprobe and the copied SHA-256 code use the project [MIT license](LICENSE). cJSON retains its own copyright notice and license.

## Verify copied files

From the repository root, check the reference and cJSON files against [CHECKSUMS.sha256](CHECKSUMS.sha256):

```sh
shasum -a 256 -c CHECKSUMS.sha256
```

Every entry should say `OK`.
