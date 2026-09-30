# Use Hashprobe with agents

Hashprobe lets agents test programs you choose, inspect failing inputs, and replay tests after a fix. Reports are stored locally.

The server runs on macOS or Linux as one C executable, `hashprobe-mcp`. It includes the test engine and needs no Python installation.

## Build and install

With a C11 compiler, Make, and awk installed, run these commands from the main Hashprobe folder:

```sh
make
make install PREFIX="$HOME/.local"
```

This installs `hashprobe` and `hashprobe-mcp` into `~/.local/bin`. To use the build directly, replace the installed path below with `./build/hashprobe-mcp`.

## Connect an agent

Hashprobe keeps its configuration at `~/.config/hashprobe/mcp.json`.

**New setup:** create a configuration using an installed OpenSSL program:

```sh
"$HOME/.local/bin/hashprobe-mcp" init
```

**Existing setup:** keep your configuration and print its connection settings:

```sh
"$HOME/.local/bin/hashprobe-mcp" client-config
```

Both commands print a `hashprobe` entry for your agent application's MCP settings. Add that entry, or replace the old Python entry when upgrading. If the application uses a setup form, enter the printed `command` and `args` and choose **stdio**.

Reconnect the agent. It starts the server automatically. Existing configurations and reports work with the C version; `init` never replaces a configuration file.

Try this prompt:

> List the Hashprobe programs I can test, then check OpenSSL and explain the result.

## Add a program

Add a named entry under `targets` in the configuration. For example:

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

The program must [read input bytes and return a SHA-256 hash](../README.md#test-your-own-program). Set `output` to `binary` if it returns 32 raw bytes instead of 64 hex characters.

Relative directories are resolved from the configuration file's location. Relative executable paths are resolved from the program's `cwd`. Restart the server after editing the configuration.

To create a separate configuration from a command:

```sh
"$HOME/.local/bin/hashprobe-mcp" init --config ./my-mcp.json --name firmware \
  -- /absolute/path/to/sha256-program
```

Agents select configured names such as `firmware`. Only the configuration owner chooses the commands to run. See the [sample configuration](examples/config.json) for more examples.

## Tools

| Tool | Purpose |
| --- | --- |
| `list_targets` | List configured programs and time limits |
| `check` | Run tests; return counts, a report ID, and the first ten failures |
| `get_failure` | Read a saved failure, its input, and expected and actual hashes |
| `replay` | Repeat saved failures against a chosen program and save a new report |

Call `check` with a `target` name. Optional settings are `seed`, `random_cases`, `max_bytes`, `timeout_ms`, and `fail_fast`. The default run has 117 tests; agents can lower the owner's time limit.

Call `get_failure` with a `report_id`. `index` selects a failure, starting at zero. `offset` and `limit` read input bytes in pieces: 256 bytes by default, up to 4096. Use `next_offset` or `next_failure_index` to continue.

Call `replay` with `report_id` and `target`. Add `case_id` to repeat one failure. Run `check` afterward for the full set.

A wrong hash returns `status: "mismatch"`, a finding for the agent to investigate. Invalid arguments or unavailable reports return tool errors. A full success requires `status: "pass"` and `counts.complete: true`.

## Reports and limits

These settings can be changed in the configuration:

| Setting | Default | Purpose |
| --- | --- | --- |
| `report_dir` | `reports` | Report directory, relative to the configuration file |
| `timeout_ms` | `5000` | Time allowed per test, in milliseconds |
| `run_timeout_seconds` | `120` | Time allowed for a whole run |
| `max_reports` | `100` | Maximum number of saved reports |
| `max_storage_mb` | `256` | Report storage budget, in MiB |

One run is allowed at a time per report directory. Cancellation or a disconnected client stops the run and its test program. When storage is full, archive or remove old reports before running more tests.

Configured programs run with your user permissions. Only configure programs you trust; Hashprobe does not isolate them from your computer.

## Development and compatibility

See the [development instructions](../README.md#development) for tests and memory checks. The C server is in [src](src), and its tool definitions are in [tools.json](tools.json).

The server uses local stdio connections. It supports MCP `2026-07-28` and the initialization handshake used by `2025-11-25`, `2025-06-18`, `2025-03-26`, and `2024-11-05` clients. Requests are limited to 256 KiB; queued responses are limited to 1 MiB.

To share Hashprobe, provide the source and build instructions, or a binary for the recipient's operating system and CPU. Linux binaries also need a compatible C library. Each user creates their own configuration.
