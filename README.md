# Hashprobe

Hashprobe checks whether a program calculates SHA-256 hashes correctly. It is written in C and saves failing inputs so you can try them again after a fix.

Use it after changing a crypto library, compiler, or firmware to catch wrong results before shipping.

## Get started

You need macOS or Linux, a C compiler with C11 support, and Make. Run these commands from the Hashprobe folder:

```sh
make
mkdir -p reports
./build/hashprobe check --report reports/example.json -- ./build/sha256-target
```

This builds Hashprobe and checks the included example program. You should see `117 passed`. The results are saved in `reports/example.json`.

Everything after `--` is the program to test and its arguments. Use a new report filename for each run; existing reports are kept.

The example uses the same SHA-256 code as Hashprobe. To check a separate implementation, try OpenSSL if it is installed:

```sh
./build/hashprobe check --output binary --report reports/openssl.json \
  -- openssl dgst -sha256 -binary
```

## Read the result

| Result | Meaning | Exit code |
| --- | --- | --- |
| `pass` | All selected tests returned the correct hash | `0` |
| `mismatch` | At least one test returned the wrong hash | `1` |
| `error` | A test could not run correctly, or a report could not be saved | `2` |
| `interrupted` | The run was stopped, for example with Ctrl+C | `130` or `143` |

The JSON report records each result, the program tested, and how many tests ran or were skipped. Failed tests include their input data so you can repeat them. Check `status` for the outcome; `summary.complete` shows whether every selected test was attempted without interruption.

Hashprobe stops if a program crashes, takes too long, or returns output it cannot read. The default time limit is five seconds per test.

## Test your own program

Hashprobe starts your program once for each test. Your program must:

1. Read all input bytes from standard input (`stdin`), including bytes with value zero.
2. Write only the SHA-256 hash to standard output (`stdout`): 64 hexadecimal characters, using `0–9` and `a–f`. Uppercase letters and spaces or line breaks around the hash are accepted.
3. Exit with code `0` on success. Send error messages to standard error (`stderr`).

If your program returns the hash as 32 raw bytes, add `--output binary`, as in the OpenSSL example.

Use [examples/sha256_target.c](examples/sha256_target.c) as a starting point. To test a device, write a small program that sends the input to the device and prints the hash it returns.

## Repeat a failure after a fix

The example has an optional, deliberate bug: it hashes only the bytes before the first zero byte. Turn it on to see Hashprobe find a problem:

```sh
./build/hashprobe check --report reports/nul-bug.json \
  -- ./build/sha256-target --demo-bug-nul
```

This should report mismatches and exit with code `1`. Now repeat the saved failures with the bug turned off:

```sh
./build/hashprobe replay reports/nul-bug.json --report reports/fixed.json \
  -- ./build/sha256-target
```

The saved tests should now pass. Replay uses the program you name after `--` and tests only the saved failures. Run `check` again for a full set of tests.

## What gets tested

The default run contains 117 tests:

- **4 fixed examples** with known correct hashes, from an empty input to one million `a` bytes.
- **81 inputs of different sizes and patterns**, chosen to catch mistakes near the points where SHA-256 splits or pads data into blocks.
- **32 generated inputs**, up to 4096 bytes each. The same settings and seed number produce the same inputs.

Hashprobe checks its own SHA-256 code against the four fixed examples before each run. It uses that code to calculate the expected hashes for the other inputs.

Useful options go before `--`:

| Option | What it changes |
| --- | --- |
| `--seed 42` | Choose a repeatable set of generated inputs |
| `--random-cases 64` | Run 64 generated tests instead of 32 |
| `--timeout-ms 10000` | Wait up to 10 seconds per test |
| `--target-version firmware-v2` | Record the tested version in the report |
| `--fail-fast` | Stop at the first wrong hash |

Run `./build/hashprobe --help` for all options and their limits.

## Test Hashprobe itself

These checks are for development and require Python 3. They also compare results with OpenSSL when it is installed.

```sh
make test       # Run the automated tests
make sanitize   # Also check for memory errors and unsafe C operations
```

Python is not needed to run Hashprobe. The C code is in [src](src); [PROVENANCE.md](PROVENANCE.md) explains where the code and included libraries came from.

## Scope

This release checks SHA-256 results. It does not assess the security of a whole product or provide [NIST certification](https://csrc.nist.gov/Projects/Cryptographic-Algorithm-Validation-Program/Secure-Hashing).

Programs run with your user permissions. Use programs you trust; the time and output limits do not isolate them from your computer.

## License

Hashprobe uses the [MIT license](LICENSE). The included cJSON library keeps its [original MIT license](vendor/cjson/LICENSE).
