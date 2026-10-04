# Hashprobe

[![CI](https://github.com/ArcX-Research/hashprobe/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/ArcX-Research/hashprobe/actions/workflows/ci.yml)
[![C11](https://img.shields.io/badge/C-C11-00599C)](Makefile)
[![Linux and macOS](https://img.shields.io/badge/platform-Linux%20%7C%20macOS-555)](docs/BINARIES.md)
[![MIT license](https://img.shields.io/badge/license-MIT-blue)](LICENSE)
[![M8ven Score](https://m8ven.ai/badge/mcp/arcx-research-hashprobe-d02ezj)](https://m8ven.ai/mcp/arcx-research-hashprobe-d02ezj?s=readme)

Hashprobe checks whether a program calculates SHA-256 hashes correctly. It saves failing inputs so you can test a fix. Use it to test changes to crypto libraries, compilers, or firmware.

Hashprobe is written in C and runs on your computer. Use `hashprobe` from a terminal, or connect `hashprobe-mcp` to an agent app. The app starts the local MCP server, which runs your configured programs and saves reports on the same computer.

For agents, the process is **install → connect your agent app → add a program → run checks**. Install once for your user and connect each app you use; add a named program for each implementation you want to test.

The [browser lab](https://hashprobe.dilate.co.ke/) provides demos and a report viewer. To use the local MCP, configure the installed command as shown below. You do not need to host a server or provide a website URL.

[![Hashprobe detects incorrect SHA-256 hashes and verifies the fix](docs/assets/hashprobe.gif)](docs/assets/hashprobe.mp4)

[Watch the video (15 seconds)](docs/assets/hashprobe.mp4): recorded tests with intentional bugs, followed by a verified fix.

## Install

Install on the computer where your agent app and the programs you want to test run. Choose a [prebuilt release](docs/BINARIES.md) or build from source below. Both give you the same two commands.

You need macOS or Linux, Git, a C11 compiler, Make, and awk. Python is only needed for development tests.

Clone the repository:

```sh
git clone https://github.com/ArcX-Research/hashprobe.git
```

Open the project folder:

```sh
cd hashprobe
```

Build the programs in `build/`:

```sh
make
```

Install both commands in `~/.local/bin` for your user:

```sh
make install PREFIX="$HOME/.local"
```

This makes the installed commands available across projects. Plain `make install` uses `/usr/local/bin` instead and may require administrator permissions.

Add this line to `~/.zshrc` (Zsh) or `~/.bashrc` (Bash), and run it in your current terminal:

```sh
export PATH="$HOME/.local/bin:$PATH"
```

You can now use `hashprobe` and `hashprobe-mcp` from any project. Check the installation:

```sh
hashprobe self-test
```

## Connect an agent

This step connects an installed MCP server to an agent app. It is required for agents to discover the tools; installing the binaries alone does not connect them.

For the first-run OpenSSL example, install OpenSSL and your chosen app's CLI. Run the matching setup command once for each app you use.

For Codex:

```sh
hashprobe-mcp setup --client codex
```

For Claude Code:

```sh
hashprobe-mcp setup --client claude
```

Setup saves the installed server's path in the app's MCP settings. It also creates `~/.config/hashprobe/mcp.json` with an `openssl` target if that file is missing. Existing Hashprobe settings are kept. A **target** is a named program Hashprobe can test.

Restart the app to load the tools. The app starts and stops the local server automatically; you do not need to leave a server running in another terminal.

With the default configuration, ask the agent:

> Use Hashprobe to list the configured targets, check the openssl target, and show the result.

Expect `117` passed tests and a complete report. This checks OpenSSL. To test your own code, [add its program as a target](mcp/README.md#add-a-program).

For Cursor or another app, follow the [manual connection steps](mcp/README.md#other-clients).

## Use the terminal

You can also run checks directly with `hashprobe`; this does not require MCP registration. From any project, create a report folder:

```sh
mkdir -p reports
```

Test OpenSSL, if it is installed:

```sh
hashprobe check --output binary --report reports/openssl.json \
  -- openssl dgst -sha256 -binary
```

Expect `117 passed`. Use a new report filename for each run; existing reports are never overwritten.

## Test your own program

A target is an executable or wrapper script that calculates SHA-256. Point Hashprobe to that program, not the project directory. The program must:

1. Read all input bytes from standard input (`stdin`), including zero bytes.
2. Write the SHA-256 hash to standard output (`stdout`): 64 hex characters, with no other text. Uppercase letters and surrounding whitespace are accepted.
3. Exit with code `0` on success. Send diagnostics to standard error (`stderr`).

For programs that return 32 raw digest bytes, use `--output binary`.

For agents, [add a target](mcp/README.md#add-a-program) in the Hashprobe configuration and restart the agent app. From a terminal, put the program and its arguments after `--`:

```sh
hashprobe check --report reports/project.json -- /absolute/path/to/sha256-program
```

Hashprobe starts the program once per test. Replace the example path with your built program or wrapper.

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

From the Hashprobe source folder, create the report folder:

```sh
mkdir -p reports
```

Run the included example with its intentional bug, which stops hashing at the first zero byte:

```sh
hashprobe check --report reports/nul-bug.json \
  -- ./build/sha256-target --demo-bug-nul
```

This reports mismatches and exits with code `1`. Replay the saved failures with the bug disabled:

```sh
hashprobe replay reports/nul-bug.json --report reports/fixed.json \
  -- ./build/sha256-target
```

The saved tests should pass. Replay tests only saved failures against the program after `--`. Run `check` again to test the full set.

Use `--demo-bugs` for several repeatable faults: dropped input bytes, a flipped output bit, and reversed byte order.

The example shares Hashprobe's reference code; OpenSSL provides a separate implementation to compare.

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
hashprobe --help
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

Check installation and agent setup in temporary directories:

```sh
make test-install
```

Run all test suites with extra checks for memory errors and invalid C operations:

```sh
make sanitize PYTHON=.venv/bin/python
```

The shared C engine is in [src](src), and the MCP server is in [mcp/src](mcp/src). See [code origins](PROVENANCE.md) for the reference implementation and included libraries.

During development, use `./build/hashprobe` and `./build/hashprobe-mcp` to run the current build without installing it. After an update, run `make` and the install command again. Client registration uses the installed path, so it only needs to be done once.

To remove the installed commands:

```sh
make uninstall PREFIX="$HOME/.local"
```

Your configuration and reports are kept. Remove the `hashprobe` entry from your client's MCP settings if you no longer use it.

## CI and releases

[CI](.github/workflows/ci.yml) runs on pull requests and pushes to `main`. It checks the CLI and MCP server with GCC and Clang on Linux and macOS, verifies the copied source files, checks installation and release archives, and runs all test suites with AddressSanitizer and UndefinedBehaviorSanitizer. Compiler warnings fail the normal builds.

Each successful platform build saves a downloadable archive in the workflow run. [Dependabot](.github/dependabot.yml) checks weekly for updates to GitHub Actions and the MCP test client.

To make a local archive for your current system:

```sh
make package
```

The archive is saved in `dist/`. Local packaging does not run the tests.

To publish a release, set `HP_VERSION` in [src/hashprobe.h](src/hashprobe.h), commit the change, and push `main`. Tag that commit with the matching version, for example:

```sh
git tag -a v0.2.0 -m "Hashprobe 0.2.0"
```

Push the tag:

```sh
git push origin v0.2.0
```

The [release workflow](.github/workflows/release.yml) runs the same CI checks before publishing four archives and `SHA256SUMS` on [GitHub Releases](https://github.com/ArcX-Research/hashprobe/releases). Tags must use `vMAJOR.MINOR.PATCH` and match the source version. No extra repository secrets are needed.

## Scope and license

Hashprobe checks SHA-256 results. It does not assess a whole product's security or provide [NIST certification](https://csrc.nist.gov/Projects/Cryptographic-Algorithm-Validation-Program/Secure-Hashing).

Tested programs run with your user permissions. Only run programs you trust; Hashprobe does not isolate them from your computer.

Hashprobe uses the [MIT license](LICENSE). cJSON retains its [original MIT license](vendor/cjson/LICENSE).
