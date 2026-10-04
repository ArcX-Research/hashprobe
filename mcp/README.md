# MCP guide

Hashprobe lets agents test configured SHA-256 programs on your computer. Follow the [installation](../README.md#install) and [agent setup](../README.md#connect-an-agent) instructions first.

Setup creates `~/.config/hashprobe/mcp.json` with an `openssl` target if the file is missing. It keeps existing settings. Your agent app starts the server automatically through standard input and output (`stdio`).

## Other clients

If you have no configuration yet, create one using OpenSSL:

```sh
hashprobe-mcp init
```

Print the connection settings:

```sh
hashprobe-mcp client-config
```

Merge the printed `hashprobe` entry into your app's MCP settings, then restart it. [Cursor](https://cursor.com/docs/mcp) uses `~/.cursor/mcp.json`. In a setup form, use the printed `command` and `args` with transport **stdio**.

## Add a program

Add a named entry under `targets` in `~/.config/hashprobe/mcp.json`. Keep existing entries and replace these example paths:

```json
{
  "targets": {
    "firmware": {
      "command": ["/absolute/path/to/sha256-program"],
      "cwd": "/absolute/path/to/project",
      "output": "hex"
    }
  }
}
```

`command` is an executable or wrapper script; `cwd` is its working directory. The program must [read input bytes and return a hash](../README.md#test-your-own-program). Set `output` to `hex` for 64 hex characters or `binary` for 32 raw bytes.

Restart the app, then ask:

> Use Hashprobe to check the firmware target. If it fails, show the input and the expected and actual hashes.

Agents can select configured targets, but cannot supply arbitrary commands. Configured programs run with your user permissions, without isolation.

Relative `cwd` paths start from the configuration file's directory. Relative executable paths start from `cwd`; bare names such as `openssl` use `PATH`. See the [sample configuration](examples/config.json) for more examples.

### Separate configurations

To start with your own program instead of OpenSSL:

```sh
hashprobe-mcp init --config ./my-mcp.json --name firmware \
  -- /absolute/path/to/sha256-program
```

Add `--config ./my-mcp.json` to `setup` or `client-config` to connect an app to this file.

## Tools

| Tool | Purpose | Required arguments |
| --- | --- | --- |
| `list_targets` | List available programs and time limits | None |
| `check` | Run tests and return a report ID, counts, and the first ten failures | `target` |
| `get_failure` | Read a saved input and its expected and actual hashes | `report_id` |
| `replay` | Retest saved failures and save a new report | `report_id`, `target` |

`check` runs 117 tests by default. A full pass has `status: "pass"` and `counts.complete: true`. A wrong hash returns `status: "mismatch"`.

`get_failure` reads failures by zero-based `index`. Follow `next_offset` for more input bytes and `next_failure_index` for another failure. After testing a fix with `replay`, run `check` for the full test set.

See [tools.json](tools.json) for all arguments, defaults, and response fields.

## Reports and limits

Reports default to `~/.config/hashprobe/reports`. Set these options alongside `targets`:

| Setting | Default | Purpose |
| --- | --- | --- |
| `report_dir` | `reports` | Directory relative to the configuration file, or an absolute path |
| `timeout_ms` | `5000` | Milliseconds allowed per test |
| `run_timeout_seconds` | `120` | Seconds allowed per run |
| `max_reports` | `100` | Maximum saved reports |
| `max_storage_mb` | `256` | Storage limit in MiB |

Tool calls can lower the per-test timeout, but cannot raise it above the configured limit. One run can use a report directory at a time. Cancellation or a disconnected client stops the run and its test program. Archive or remove old reports when storage is full.

## Development

See [testing instructions](../README.md#development), [server code](src), and [tool definitions](tools.json).

The server uses local `stdio`, with no HTTP endpoint. It supports MCP `2026-07-28` and initialization for `2025-11-25`, `2025-06-18`, `2025-03-26`, and `2024-11-05`. Input messages are limited to 256 KiB; queued responses to 1 MiB.
