# Hashprobe binaries

Each [release](https://github.com/ArcX-Research/hashprobe/releases) includes the local CLI, MCP server, an example SHA-256 program, and both MIT licenses. These are the same programs you get by building from source. Git, a compiler, and Python are not required for a prebuilt release.

## Choose a download

| Archive suffix | System |
| --- | --- |
| `linux-x86_64` | Intel or AMD Linux, glibc 2.39 or newer (Ubuntu 24.04 or newer) |
| `linux-arm64` | ARM64 Linux, glibc 2.39 or newer (Ubuntu 24.04 or newer) |
| `macos-arm64` | Apple silicon, macOS 15 or newer |
| `macos-x86_64` | Intel Mac, macOS 15 or newer |

For other systems, [build from source](https://github.com/ArcX-Research/hashprobe#install).

Download your `.tar.gz` archive and `SHA256SUMS` from the same release. In the download folder, check the archive before extracting it:

```sh
shasum -a 256 --ignore-missing -c SHA256SUMS
```

Your downloaded archive should say `OK`. The macOS binaries are not signed or notarized with an Apple Developer ID.

## Extract

Extract the archive. For example, on Apple silicon:

```sh
tar -xzf hashprobe-0.2.0-macos-arm64.tar.gz
```

Open the extracted folder, using your downloaded version and platform:

```sh
cd hashprobe-0.2.0-macos-arm64
```

## Install

Create a local binary folder:

```sh
mkdir -p "$HOME/.local/bin"
```

Install the CLI and MCP server:

```sh
install -m 755 bin/hashprobe bin/hashprobe-mcp "$HOME/.local/bin/"
```

Add this line to `~/.zshrc` (Zsh) or `~/.bashrc` (Bash), and run it in your current terminal:

```sh
export PATH="$HOME/.local/bin:$PATH"
```

Check that the installed command works from any directory:

```sh
hashprobe self-test
```

## Connect an agent

With OpenSSL and your client's CLI installed, register Hashprobe for your user. For Codex:

```sh
hashprobe-mcp setup --client codex
```

For Claude Code:

```sh
hashprobe-mcp setup --client claude
```

Setup creates the default OpenSSL target if no Hashprobe configuration exists and adds the local server to your app's MCP settings. Restart the app; it starts the server automatically and makes the four tools available across projects.

See the [MCP guide](https://github.com/ArcX-Research/hashprobe/blob/main/mcp/README.md) to check the connection, connect other apps, and add your own programs.

## Test the included example

From the extracted folder, run the CLI directly:

```sh
hashprobe check --report example.json -- ./bin/sha256-target
```

Expect `117 passed`. This checks the included example. Use a new report filename for each run.
