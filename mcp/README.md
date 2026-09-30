# Use Hashprobe with agents

The MCP server lets other people's agents run Hashprobe on their own computers. Each user chooses which programs can be tested. Reports and failing inputs stay on that computer.

The hash tests run in C. A small Python layer uses the [official MCP SDK](https://github.com/modelcontextprotocol/python-sdk) to connect agent applications to the C program.

## Install

You need macOS 11 or newer, or Linux, plus Python 3.10 or newer and a C compiler. Run these commands from the main Hashprobe folder:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install .
```

Installation builds and includes the C executable. You do not need to run `make` or install the C command separately.

If you use `uv`, `uv tool install .` is an alternative. The commands below use the virtual environment shown above.

## Connect an agent

Create a configuration using your installed OpenSSL program:

```sh
.venv/bin/hashprobe-mcp init
```

This creates `~/.config/hashprobe/mcp.json` and prints connection settings with the correct paths for your installation. Existing configuration files are preserved.

Add the printed `hashprobe` entry to your agent application's MCP settings. Applications with a setup form need the printed `command` and `args`, using the **stdio** connection type. Restart or reconnect the MCP server in that application.

The server runs locally when your agent application starts it. There is no account, API key, or hosted service to configure.

Ask your agent:

> List the Hashprobe programs I can test, then check OpenSSL and explain the result.

To print the connection settings again:

```sh
.venv/bin/hashprobe-mcp client-config
```

## Add your own program

Edit the configuration file and add another named program. For example:

```json
{
  "report_dir": "reports",
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

The program must read input bytes and print a SHA-256 hash, as described in the [main README](../README.md#test-your-own-program). Use `"output": "binary"` if it returns 32 raw bytes instead of 64 hex characters.

Relative directories are resolved from the configuration file's location. A relative executable path is resolved from the target's `cwd`. Restart the MCP server after editing the configuration.

You can also create a separate configuration from a command:

```sh
.venv/bin/hashprobe-mcp init --config ./my-mcp.json --name firmware \
  -- /absolute/path/to/sha256-program
```

Agent tool calls choose a configured name such as `firmware`. Commands are set by the owner in the configuration, and reports are looked up by generated IDs.

## Available tools

| Tool | What it does |
| --- | --- |
| `list_targets` | Lists configured programs and time limits |
| `check` | Runs tests and returns counts, a report ID, and the first ten failures |
| `get_failure` | Reads a saved failure, including expected and actual hashes |
| `replay` | Tests saved failures against a chosen program and saves a new report |

`check` accepts `target`, `seed`, `random_cases`, `max_bytes`, `timeout_ms`, and `fail_fast`. Defaults match the C command's 117 tests. Agents can reduce the configured time limit.

For `get_failure`, `index` selects a failure starting at zero. Large inputs are read in pieces: `offset` and `limit` count bytes. The default piece is 256 bytes; the maximum is 4096. Follow `next_offset` or `next_failure_index` to continue.

For `replay`, provide `report_id` and `target`. Add `case_id` to repeat just one failure. Run `check` afterward to test the full set again.

A wrong hash produces a normal tool result with `status: "mismatch"`. It is evidence for the agent to investigate. Invalid arguments and unavailable reports produce MCP tool errors. A full success requires both `status: "pass"` and `counts.complete: true`.

## Limits and reports

By default, the server allows one run at a time, five seconds per test, and 120 seconds for a whole run. Cancellation or a disconnected client stops the active C runner, which stops its test program. The server keeps at most 100 reports and reserves space within a 256 MiB report budget. It asks the owner to remove or archive reports when storage is full.

These settings can be changed in the configuration: `timeout_ms`, `run_timeout_seconds`, `max_reports`, and `max_storage_mb`. Reports are stored under `report_dir`, which defaults to `reports` beside the configuration file.

Configured programs run with your user permissions. Choose programs you trust. The server's limits do not provide an isolated environment for running unknown code.

## Development and sharing

The MCP code is in [src](src), with a sample configuration in [examples/config.json](examples/config.json). Packaging files stay at the repository root so installation can include the C sources too.

From the main Hashprobe folder:

```sh
make test-mcp PYTHON=.venv/bin/python
```

The tests use the official MCP client to check discovery, tool calls, reports, replay, and error handling.

After changing `mcp/src`, reinstall with `.venv/bin/python -m pip install .` before testing.

To build an installable source archive and a wheel for your current operating system:

```sh
.venv/bin/python -m pip install build
.venv/bin/python -m build
```

The files appear in `dist/`. A source archive can be shared with macOS and Linux users who have a C compiler. A wheel includes the compiled engine and must match the user's operating system and CPU. Linux wheels also need a compatible C library; use the source archive for wider compatibility. Install either file with `python -m pip install /path/to/file`.

This server uses local stdio connections. It has not been deployed as a public HTTP service or published to a package registry.
