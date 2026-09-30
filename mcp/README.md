# Use Hashprobe with agents

Hashprobe's MCP server lets agents test programs you configure, inspect failing inputs, and check fixes. It stores reports locally and runs as one C executable on macOS or Linux. Python is not needed to use it.

## Build and install

You need a C11 compiler, Make, and awk. Run these commands from the main Hashprobe folder.

Build:

```sh
make
```

Install for your user:

```sh
make install PREFIX="$HOME/.local"
```

This puts `hashprobe` and `hashprobe-mcp` in `~/.local/bin`. To use the build without installing it, run `./build/hashprobe-mcp` from the repository instead of the installed path below.

## Connect an agent

With OpenSSL installed, create a configuration:

```sh
"$HOME/.local/bin/hashprobe-mcp" init
```

This saves `~/.config/hashprobe/mcp.json` and prints the connection settings. It never replaces an existing configuration.

If you already have a configuration, print its connection settings:

```sh
"$HOME/.local/bin/hashprobe-mcp" client-config
```

Add the printed `hashprobe` entry to your agent application's MCP settings. If the application uses a setup form, enter the printed `command` and `args`, then choose **stdio**.

Reconnect the agent. It starts Hashprobe automatically. Try this prompt:

> List the Hashprobe programs I can test, then check OpenSSL and explain the result.

For your own program, follow the next section.

## Add a program

Add a named entry under `targets` in your configuration:

```json
{
  "targets": {
    "firmware": {
      "command": ["/absolute/path/to/sha256-program"],
      "cwd": "/absolute/path/to/project",
      "output": "hex",
      "version": "firmware-v2",
      "description": "SHA-256 code from our firmware build"
    }
  }
}
```

The program must [read input bytes and return a SHA-256 hash](../README.md#test-your-own-program). Use `output: "binary"` for 32 raw digest bytes, or `output: "hex"` for 64 hex characters.

Relative `cwd` paths start from the configuration file's directory. A relative executable path starts from `cwd`; a name such as `openssl` is looked up on `PATH`. Restart the server after editing the configuration.

To create a separate configuration for a program:

```sh
"$HOME/.local/bin/hashprobe-mcp" init --config ./my-mcp.json --name firmware \
  -- /absolute/path/to/sha256-program
```

This prints connection settings for that configuration. Agents choose names such as `firmware`; the configuration determines which commands they can run. See the [sample configuration](examples/config.json) for more examples.

## Tools

| Tool | Purpose | Required arguments |
| --- | --- | --- |
| `list_targets` | List configured programs and time limits | None |
| `check` | Run tests and return a report ID, counts, and the first ten failures | `target` |
| `get_failure` | Read a saved input and its expected and actual hashes | `report_id` |
| `replay` | Repeat saved failures and save a new report | `report_id`, `target` |

`check` runs 117 tests by default. Optional arguments are `seed`, `random_cases`, `max_bytes`, `timeout_ms`, and `fail_fast`. The requested timeout must be at or below the configured limit.

For `get_failure`, `index` selects a failure starting at zero. `offset` and `limit` read the input in pieces: 256 bytes by default, up to 4096. Use `next_offset` or `next_failure_index` to continue.

For `replay`, add `case_id` to select one failure. Run `check` afterward to test the full set.

A wrong hash returns `status: "mismatch"` for the agent to investigate. Invalid arguments or unavailable reports return tool errors. A successful run has both `status: "pass"` and `counts.complete: true`.

## Reports and limits

Set these options alongside `targets` in your configuration:

| Setting | Default | Purpose |
| --- | --- | --- |
| `report_dir` | `reports` | Report directory, relative to the configuration file |
| `timeout_ms` | `5000` | Time allowed per test, in milliseconds |
| `run_timeout_seconds` | `120` | Time allowed for a whole run, in seconds |
| `max_reports` | `100` | Maximum number of saved reports |
| `max_storage_mb` | `256` | Storage budget, in MiB |

One run can use a report directory at a time. Cancellation or a disconnected client stops the run and its test program. When storage is full, archive or remove old reports before starting another run.

Configured programs run with your user permissions. Only configure programs you trust; Hashprobe does not isolate them from your computer.

## Development and compatibility

See the [development instructions](../README.md#development) for tests and memory checks. Server code is in [src](src); tool definitions are in [tools.json](tools.json).

The server communicates through standard input and output (`stdio`). It supports MCP `2026-07-28` and the initialization handshake used by `2025-11-25`, `2025-06-18`, `2025-03-26`, and `2024-11-05` clients. Messages are limited to 256 KiB on input, with up to 1 MiB of queued responses.

Share the source and build instructions, or a binary matching the recipient's operating system and CPU. Linux binaries also need a compatible C library. Each user configures their own programs.
