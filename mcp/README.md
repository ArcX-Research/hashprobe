# Use Hashprobe with agents

Hashprobe is a local MCP server for testing SHA-256 programs. Your agent app starts it on the same computer, and Hashprobe runs the programs you configure there. It saves reports locally and returns results to the agent. Python and web hosting are not needed to use it.

## Install

Follow the [installation instructions](../README.md#install) or use a [prebuilt release](../docs/BINARIES.md). Both install `hashprobe` and `hashprobe-mcp` in `~/.local/bin`; add that directory to your `PATH`.

Install once for your user, then connect each agent app below. You can use the installed commands from any project.

## Connect an agent

You need your chosen app's CLI installed. For a new configuration, setup also needs OpenSSL to create the first example target. Choose the command for your app.

For Codex:

```sh
hashprobe-mcp setup --client codex
```

For Claude Code:

```sh
hashprobe-mcp setup --client claude
```

Setup uses the client's CLI to register Hashprobe for your user across projects. It saves absolute paths, so the client can launch the server even when its `PATH` differs from your terminal's. See the client documentation for [Codex](https://developers.openai.com/codex/mcp/) and [Claude Code](https://code.claude.com/docs/en/mcp).

There are two separate settings:

- `~/.config/hashprobe/mcp.json` defines the programs Hashprobe can test. Setup creates it with an `openssl` target if missing, or validates and keeps the existing file.
- Your app's MCP settings tell it how to start the installed `hashprobe-mcp` executable. Setup adds the `hashprobe` entry there.

Restart the app. It starts the server automatically and discovers `list_targets`, `check`, `get_failure`, and `replay`. You do not need to run the server yourself or enter a server URL.

With the default configuration, ask:

> Use Hashprobe to list the configured targets, check the openssl target, and show the result.

Expect `117` passed tests and a complete report. That confirms the connection and checks OpenSSL. Add your own program below to test your code.

### Other clients

Create a configuration if you do not have one:

```sh
hashprobe-mcp init
```

Print the connection settings:

```sh
hashprobe-mcp client-config
```

Add the printed `hashprobe` entry to the client's user-level MCP settings, then restart it. [Cursor](https://cursor.com/docs/mcp) uses `~/.cursor/mcp.json`. For a setup form, enter the printed `command` and `args`, then choose **stdio**. Installing a command on `PATH` does not register it with an MCP client.

## Add a program

Open `~/.config/hashprobe/mcp.json` and add a named entry under `targets`, keeping your existing settings. Replace the example paths with your program and its working directory:

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

`command` points to an executable or wrapper script; `cwd` is the folder it runs in. A project folder alone is not a test target. The program must [read input bytes and return a SHA-256 hash](../README.md#test-your-own-program). Use `output: "binary"` for 32 raw digest bytes, or `output: "hex"` for 64 hex characters.

Restart the agent app to load the updated targets, then ask:

> Use Hashprobe to check the firmware target. If it fails, show the input and compare the expected and actual hashes.

Agents select the configured name, such as `firmware`. MCP calls cannot supply arbitrary commands. You only need to install and connect Hashprobe once; adding another target makes another program available to agents.

Relative `cwd` paths start from the configuration file's directory. A relative executable path starts from `cwd`; a name such as `openssl` is looked up on `PATH`.

Optional: create a separate configuration, or use this command to start with your own program when OpenSSL is unavailable:

```sh
hashprobe-mcp init --config ./my-mcp.json --name firmware \
  -- /absolute/path/to/sha256-program
```

This prints connection settings for that configuration. Register it with `hashprobe-mcp setup --client codex --config ./my-mcp.json`, or copy the settings into your client. See the [sample configuration](examples/config.json) for more examples.

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

With the default configuration, MCP reports are saved in `~/.config/hashprobe/reports`. The agent receives a report ID and uses `get_failure` to inspect saved failures or `replay` to check a fix.

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

## Web-based MCP audits

[M8ven Pre-Flight](https://m8ven.ai/preflight) requires a deployed HTTPS MCP endpoint using Streamable HTTP. This server uses local `stdio`; a GitHub URL or the browser lab URL cannot serve as that endpoint. Local compatibility checks run with `make test-mcp PYTHON=.venv/bin/python`.

A hosted service needs its own HTTP transport and tool scope. Paths on the server refer to the server's files, not an agent's project. A hosted version could check inputs and hash results submitted by agents that run their code locally.
