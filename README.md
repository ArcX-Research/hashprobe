# Hashprobe

A C command-line tool for finding SHA-256 implementation errors and replaying the inputs that expose them.

A firmware engineer can run Hashprobe after changing a compiler, crypto library, or hardware adapter. It catches incorrect digests, including mistakes around binary input, padding, and block boundaries, and saves a report that can reproduce the failure after a fix.

## Build and run

Requires a C11 compiler and Make on macOS or Linux. Runtime dependencies are the operating system and the program being tested. All library source needed to build Hashprobe is included.

```sh
make
mkdir -p reports
./build/hashprobe check --report reports/native.json -- ./build/sha256-target
```

The default suite runs 117 cases. Reports must use a new filename; existing files are preserved.

For an independent target, use an installed OpenSSL command:

```sh
./build/hashprobe check --output binary --report reports/openssl.json \
  -- openssl dgst -sha256 -binary
```

Optional installation: `make install PREFIX="$HOME/.local"` installs the `hashprobe` command in `~/.local/bin`.

## Connect an implementation

Wrap the implementation in a program that:

1. Reads the complete message as raw bytes from stdin, including NUL bytes.
2. Writes only its SHA-256 digest to stdout: 64 hex digits, or exactly 32 bytes with `--output binary`.
3. Exits with status zero. Diagnostics belong on stderr.

Hex output may use either letter case and surrounding ASCII whitespace. Binary output is used unchanged. Each case starts a fresh process. The target inherits your environment and working directory; its arguments are passed directly without a shell.

[examples/sha256_target.c](examples/sha256_target.c) is a small C adapter. It uses the same reference as Hashprobe; use OpenSSL or your own implementation for an independent comparison. Hardware requires an adapter that sends the input to the device and returns its digest.

## Find and replay a bug

The example includes a deliberate C-string bug: `--demo-bug-nul` truncates input at the first NUL byte.

```sh
# Expected exit status: 1. Saves each input that produces a wrong digest.
./build/hashprobe check --report reports/nul-bug.json \
  -- ./build/sha256-target --demo-bug-nul

# Test the saved failures against the corrected target. Expected exit status: 0.
./build/hashprobe replay reports/nul-bug.json --report reports/fixed.json \
  -- ./build/sha256-target
```

Add `--case boundary-0001-zero` to replay one failure. Replay always requires an explicit target command. It validates saved inputs and expected digests, then runs those cases; it does not rerun the full suite or execute the command recorded in the old report.

## Coverage and controls

| Cases | Purpose |
| --- | --- |
| 4 known answers | Empty input, `abc`, a standard 56-byte message, and one million `a` bytes |
| 81 boundary cases | Zero, `ff`, and incrementing bytes at 27 lengths, including 55–65, 119–129, 255–257, and 4095–4097 byte boundaries |
| 32 random cases by default | Repeatable inputs up to 4096 bytes, using seed 0 |

Generated cases use the included SHA-256 reference, which checks itself against all four known answers before a run. Development tests independently compare the generated digests with Python's `hashlib` and exercise OpenSSL when available.

| Option | Use |
| --- | --- |
| `--seed N` | Repeat a random suite; accepts a 32-bit unsigned integer |
| `--random-cases N` | Choose 0–256 random cases |
| `--max-bytes N` | Set the random input limit to 1–65536 bytes |
| `--timeout-ms N` | Set the per-case deadline; default 5000 ms |
| `--target-version TEXT` | Record a firmware, library, or build identifier |
| `--fail-fast` | Stop after the first digest mismatch |

Operational errors stop the run automatically. Output is limited to 64 KiB per case across stdout and stderr. Inputs are limited to 1 MiB per case and 16 MiB per suite. Random controls apply to `check`; replay uses the saved bytes.

## Reports and exit codes

JSON reports contain the tool and suite versions, seed, target arguments, executable fingerprint when available, platform, working directory, results, and executed/skipped counts. Failed or errored cases include the exact input bytes as hex. Successful cases omit those bytes to keep reports small.

Reports are written atomically with owner-only permissions. Create the parent directory first. An existing report is never replaced. An interrupt saves the results collected so far.

| Exit | Meaning |
| --- | --- |
| `0` | Every selected case passed |
| `1` | At least one digest mismatch |
| `2` | Target, input, configuration, or report error |
| `130` / `143` | Interrupted by SIGINT / SIGTERM |

`summary.complete` means every selected case was attempted without interruption. Check `status` and the exit code for the outcome. Errors distinguish timeouts, excessive output, nonzero exits, signals, startup failures, and invalid digest output. Diagnostic excerpts are hex encoded and limited to 512 bytes per stream.

The executable fingerprint covers the resolved command file, up to 64 MiB. It does not identify linked libraries, interpreter input scripts, or device firmware; record those with `--target-version` when needed. Reports include local paths and command arguments but do not copy the environment.

## Development

```sh
make test       # Python 3 standard library; OpenSSL check skips if unavailable
make sanitize   # AddressSanitizer and UndefinedBehaviorSanitizer, with Clang/GCC
```

| Source | Responsibility |
| --- | --- |
| [src/main.c](src/main.c) | CLI options, run control, and exit codes |
| [src/suite.c](src/suite.c) | Known answers, boundaries, and deterministic random cases |
| [src/target.c](src/target.c) | Processes, deadlines, output bounds, and digest parsing |
| [src/report.c](src/report.c) | JSON reports, metadata, and replay validation |
| [src/reference/sha256.c](src/reference/sha256.c) | Original SHA-256 reference and bitwise arithmetic |

The SHA-256 core uses fixed-width words, rotations, shifts, XOR, and unsigned addition modulo 2³². Test generation uses a deterministic SplitMix64 sequence; it is not a source of cryptographic randomness. See [PROVENANCE.md](PROVENANCE.md) for source origins and the included dependency license.

## Scope

Version 0.1 tests byte-oriented, full-round SHA-256 digest correctness on macOS and Linux. It does not test streaming API state, timing leakage, key handling, or whole-device security. Windows adapters and additional algorithms are future work.

Targets run as your user. Process time and output limits support testing faulty programs; they are not a sandbox. Passing these checks is not formal [NIST algorithm validation](https://csrc.nist.gov/Projects/Cryptographic-Algorithm-Validation-Program/Secure-Hashing).

## License

Hashprobe is licensed under [MIT](LICENSE). Vendored cJSON retains its [upstream MIT license](vendor/cjson/LICENSE).
