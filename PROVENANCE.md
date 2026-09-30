# Where the code comes from

Hashprobe combines SHA-256 code from the original research project, a library for reading and writing JSON, and new code for running tests and saving results.

## SHA-256 code

These two files were copied without changes from the original research workspace:

| File in Hashprobe | Original location |
| --- | --- |
| [src/reference/sha256.c](src/reference/sha256.c) | `sha256/c/sha256.c` |
| [src/reference/sha256.h](src/reference/sha256.h) | `sha256/c/sha256.h` |

Hashprobe uses this code to calculate the expected SHA-256 hashes for its generated tests.

The four fixed test examples came from `sha256/python/test_sha256.py` in the research workspace. They cover an empty input, `abc`, a 56-byte message, and one million `a` bytes. Hashprobe generates the remaining inputs itself.

## JSON library

[cJSON](https://github.com/DaveGamble/cJSON/releases/tag/v1.7.19) reads and writes the report files. Version **1.7.19** is included in [vendor/cjson](vendor/cjson), so it does not need a separate installation.

Its source files and [MIT license](vendor/cjson/LICENSE) are unchanged from the original release.

## Code written for Hashprobe

The command-line interface, test generator, program runner, reports, failure replay, example program, and automated tests were written for Hashprobe. They are in [src](src), [examples](examples), and [tests](tests).

The agent connection code in [mcp/src](mcp/src) was also written for Hashprobe. It uses the [official MCP Python SDK](https://github.com/modelcontextprotocol/python-sdk), installed as a package dependency. The connection is tested with SDK version 2.2.0; supported dependencies are listed in [pyproject.toml](pyproject.toml).

Hashprobe and its copied SHA-256 code use the project [MIT license](LICENSE). cJSON keeps its original copyright notice and MIT license.

## Check the copied files

[CHECKSUMS.sha256](CHECKSUMS.sha256) records a value for each copied file. If a file changes, its calculated value changes too.

To check those files against the recorded values, run this from the Hashprobe folder:

```sh
shasum -a 256 -c CHECKSUMS.sha256
```

An unchanged file is listed as `OK`.
