# Hashprobe binaries

Each [release](https://github.com/ArcX-Research/hashprobe/releases) includes the CLI, MCP server, an example SHA-256 program, and both MIT licenses. Python is not required.

## Choose a download

| Archive suffix | System |
| --- | --- |
| `linux-x86_64` | Intel or AMD Linux, glibc 2.39 or newer (Ubuntu 24.04 or newer) |
| `linux-arm64` | ARM64 Linux, glibc 2.39 or newer (Ubuntu 24.04 or newer) |
| `macos-arm64` | Apple silicon, macOS 15 or newer |
| `macos-x86_64` | Intel Mac, macOS 15 or newer |

For other systems, [build from source](https://github.com/ArcX-Research/hashprobe#get-started).

Download your `.tar.gz` archive and `SHA256SUMS` from the same release. In the download folder, check the archive before extracting it:

```sh
shasum -a 256 --ignore-missing -c SHA256SUMS
```

Your downloaded archive should say `OK`. The macOS binaries are not signed or notarized with an Apple Developer ID.

## Try it

Extract the archive. For example, on Apple silicon:

```sh
tar -xzf hashprobe-0.2.0-macos-arm64.tar.gz
```

Open the extracted folder, using your downloaded version and platform:

```sh
cd hashprobe-0.2.0-macos-arm64
```

Check the reference implementation:

```sh
./bin/hashprobe self-test
```

Test the included program:

```sh
./bin/hashprobe check --report example.json -- ./bin/sha256-target
```

Expect `117 passed`. Use a new report filename for each run.

## Install

Create a local binary folder:

```sh
mkdir -p "$HOME/.local/bin"
```

Install the CLI and MCP server:

```sh
install -m 755 bin/hashprobe bin/hashprobe-mcp "$HOME/.local/bin/"
```

With OpenSSL installed, create the MCP configuration:

```sh
"$HOME/.local/bin/hashprobe-mcp" init
```

Print the client connection settings:

```sh
"$HOME/.local/bin/hashprobe-mcp" client-config
```

See the [MCP setup guide](https://github.com/ArcX-Research/hashprobe/blob/main/mcp/README.md) to configure a program to test and connect your agent.
