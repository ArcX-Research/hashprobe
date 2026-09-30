# Hashprobe

Hashprobe checks whether a program calculates SHA-256 hashes correctly. It saves failing inputs so you can test a fix. Use it to test changes to crypto libraries, compilers, or firmware.

The command-line tool and [MCP server for agents](mcp/README.md) are written in C.

## Get started

You need macOS or Linux, a C11 compiler, Make, and awk. Python is only needed for development tests. Run these commands from the Hashprobe folder.

Build the programs:

```sh
make
```

Create a folder for reports:

```sh
mkdir -p reports
```

Test the included example:

```sh
./build/hashprobe check --report reports/example.json -- ./build/sha256-target
```

Expect `117 passed`. The results are saved in `reports/example.json`. Use a new report filename for each run; existing reports are never overwritten.

The example shares Hashprobe's SHA-256 code. To check a separate implementation, use OpenSSL if it is installed:

```sh
./build/hashprobe check --output binary --report reports/openssl.json \
  -- openssl dgst -sha256 -binary
```

## Test your own program

Everything after `--` is the program to test and its arguments. Hashprobe starts it once per test. The program must:

1. Read all input bytes from standard input (`stdin`), including zero bytes.
2. Write the SHA-256 hash to standard output (`stdout`): 64 hex characters, with no other text. Uppercase letters and surrounding whitespace are accepted.
3. Exit with code `0` on success. Send diagnostics to standard error (`stderr`).

For programs that return 32 raw digest bytes, use `--output binary`.

Start with [the C example](examples/sha256_target.c). To test a device, write a small program that sends it the input and prints the returned hash.

## Read the result

| Result | Meaning | Exit code |
| --- | --- | --- |
| `pass` | All selected tests returned the correct hash | `0` |
| `mismatch` | At least one test returned the wrong hash | `1` |
| `error` | A test could not complete or its report could not be saved | `2` |
| `interrupted` | The run was stopped | `130` or `143` |

The JSON report contains the tested command, each result, and the inputs for failed tests. A successful run has both `status: "pass"` and `summary.complete: true`.

Hashprobe stops if the program crashes, exceeds its time limit, or returns unreadable output. The default limit is five seconds per test.

## Repeat a failure

Run the example with its intentional bug, which stops hashing at the first zero byte:

```sh
./build/hashprobe check --report reports/nul-bug.json \
  -- ./build/sha256-target --demo-bug-nul
```

This reports mismatches and exits with code `1`. Replay the saved failures with the bug disabled:

```sh
./build/hashprobe replay reports/nul-bug.json --report reports/fixed.json \
  -- ./build/sha256-target
```

The saved tests should pass. Replay tests only saved failures against the program after `--`. Run `check` again to test the full set.

Use `--demo-bugs` for several repeatable faults: dropped input bytes, a flipped output bit, and reversed byte order.

## Coverage and options

The default run contains 117 tests:

- **4 known answers**, from an empty input to one million `a` bytes.
- **81 size and pattern tests**, including SHA-256 block and padding boundaries.
- **32 generated inputs**, up to 4096 bytes each.

The same settings and seed produce the same inputs. Before each run, Hashprobe checks its reference implementation against the four known answers, then uses it to calculate expected hashes for the other tests.

Place options before `--`:

| Option | Purpose |
| --- | --- |
| `--seed 42` | Choose a repeatable set of generated inputs |
| `--random-cases 64` | Generate 64 inputs instead of 32 |
| `--max-bytes 8192` | Set the size limit for generated inputs |
| `--timeout-ms 10000` | Allow up to 10 seconds per test |
| `--target-version firmware-v2` | Label the tested build |
| `--fail-fast` | Stop at the first wrong hash |
| `--case boundary-0001-zero` | Replay one saved failure |

Show all options and limits:

```sh
./build/hashprobe --help
```

## Development

Run the command-line tests with Python 3:

```sh
make test
```

The MCP tests need Python 3.10 or newer. Create a development environment:

```sh
python3 -m venv .venv
```

Install the test client:

```sh
.venv/bin/python -m pip install -r mcp/tests/requirements.txt
```

Run the MCP tests:

```sh
make test-mcp PYTHON=.venv/bin/python
```

Run both test suites with extra checks for memory errors and invalid C operations:

```sh
make sanitize PYTHON=.venv/bin/python
```

The shared C engine is in [src](src), and the MCP server is in [mcp/src](mcp/src). See [code origins](PROVENANCE.md) for the reference implementation and included libraries.

## Scope and license

Hashprobe checks SHA-256 results. It does not assess a whole product's security or provide [NIST certification](https://csrc.nist.gov/Projects/Cryptographic-Algorithm-Validation-Program/Secure-Hashing).

Tested programs run with your user permissions. Only run programs you trust; Hashprobe does not isolate them from your computer.

Hashprobe uses the [MIT license](LICENSE). cJSON retains its [original MIT license](vendor/cjson/LICENSE).
