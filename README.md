# Hashprobe

Hashprobe checks whether a program calculates SHA-256 hashes correctly. It saves failing inputs so you can repeat them after a fix. Use it to test changes to a crypto library, compiler, or firmware.

The command-line tool and [MCP server for agents](mcp/README.md) are written in C.

## Get started

You need macOS or Linux, a C11 compiler, Make, and awk. Python is only needed for development tests.

From the Hashprobe folder:

```sh
make
mkdir -p reports
./build/hashprobe check --report reports/example.json -- ./build/sha256-target
```

You should see `117 passed`. The results are saved in `reports/example.json`.

Everything after `--` is the program to test and its arguments. Use a new report filename for each run; Hashprobe never overwrites an existing report.

The included example shares Hashprobe's SHA-256 code. To test a separate implementation, use OpenSSL if it is installed:

```sh
./build/hashprobe check --output binary --report reports/openssl.json \
  -- openssl dgst -sha256 -binary
```

To use the built `hashprobe-mcp` executable with an agent, follow the [agent setup guide](mcp/README.md).

## Test your own program

Hashprobe starts your program once per test. The program must:

1. Read all input bytes from standard input (`stdin`), including zero bytes.
2. Write only the SHA-256 hash to standard output (`stdout`): 64 hex characters. Uppercase letters and whitespace around the hash are accepted.
3. Exit with code `0` on success. Write diagnostics to standard error (`stderr`).

For programs that return 32 raw bytes instead of hex, use `--output binary`.

Start with [examples/sha256_target.c](examples/sha256_target.c). To test a device, write an adapter that sends it the input and prints the returned hash.

## Read the result

| Result | Meaning | Exit code |
| --- | --- | --- |
| `pass` | All selected tests returned the correct hash | `0` |
| `mismatch` | At least one test returned the wrong hash | `1` |
| `error` | A test could not finish correctly, or its report could not be saved | `2` |
| `interrupted` | The run was stopped | `130` or `143` |

The JSON report records the program tested, each result, and the inputs for failed tests. `summary.complete` tells you whether every selected test was attempted without interruption. A full success requires both `status: "pass"` and `summary.complete: true`.

Hashprobe stops when a program crashes, exceeds the time limit, or returns unreadable output. The default limit is five seconds per test.

## Repeat a failure

The example has a deliberate bug that stops hashing at the first zero byte:

```sh
./build/hashprobe check --report reports/nul-bug.json \
  -- ./build/sha256-target --demo-bug-nul
```

This reports mismatches and exits with code `1`. Repeat the saved failures with the bug turned off:

```sh
./build/hashprobe replay reports/nul-bug.json --report reports/fixed.json \
  -- ./build/sha256-target
```

The saved tests should pass. Replay uses the program named after `--` and tests only saved failures. Run `check` again for the full set.

## Test coverage and options

The default run contains 117 tests:

- **4 known answers**, from an empty input to one million `a` bytes.
- **81 size and pattern tests**, including SHA-256 block and padding boundaries.
- **32 generated inputs**, up to 4096 bytes each. The same settings and seed produce the same inputs.

Before each run, Hashprobe checks its reference SHA-256 code against the four known answers. It uses that code to calculate expected hashes for the remaining tests.

Place options before `--`:

| Option | Purpose |
| --- | --- |
| `--seed 42` | Choose a repeatable set of generated inputs |
| `--random-cases 64` | Run 64 generated tests instead of 32 |
| `--timeout-ms 10000` | Allow up to 10 seconds per test |
| `--target-version firmware-v2` | Record the tested version |
| `--fail-fast` | Stop at the first wrong hash |

Run `./build/hashprobe --help` for all options and limits.

## Development

Run the command-line tests with Python 3:

```sh
make test
```

To also test the MCP server and check for memory errors, use Python 3.10 or newer:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r mcp/tests/requirements.txt
make test test-mcp PYTHON=.venv/bin/python
make sanitize PYTHON=.venv/bin/python
```

The MCP tests use the official client to check tool calls, reports, replay, invalid requests, cancellation, and shutdown. After code changes, `make` rebuilds the executables.

The C engine is in [src](src), and the MCP server is in [mcp/src](mcp/src). See [PROVENANCE.md](PROVENANCE.md) for code origins and included libraries.

## Scope and license

Hashprobe checks SHA-256 results. It does not assess a whole product's security or provide [NIST certification](https://csrc.nist.gov/Projects/Cryptographic-Algorithm-Validation-Program/Secure-Hashing).

Tested programs run with your user permissions. Only run programs you trust; Hashprobe does not isolate them from your computer.

Hashprobe uses the [MIT license](LICENSE). cJSON retains its [original MIT license](vendor/cjson/LICENSE).
