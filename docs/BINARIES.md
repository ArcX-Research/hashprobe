# Install prebuilt binaries

Each [release](https://github.com/ArcX-Research/hashprobe/releases) includes `hashprobe`, `hashprobe-mcp`, an example program, and licenses. No compiler or Python is needed.

## Download and verify

Choose the archive for your system:

| Archive suffix | System |
| --- | --- |
| `linux-x86_64` | Intel or AMD Linux, glibc 2.39 or newer (Ubuntu 24.04 or newer) |
| `linux-arm64` | ARM64 Linux, glibc 2.39 or newer (Ubuntu 24.04 or newer) |
| `macos-arm64` | Apple silicon, macOS 15 or newer |
| `macos-x86_64` | Intel Mac, macOS 15 or newer |

For other systems, [build from source](https://github.com/ArcX-Research/hashprobe#install). macOS binaries are not signed or notarized with an Apple Developer ID.

Download the `.tar.gz` archive and `SHA256SUMS` from the same release. From the download folder:

```sh
shasum -a 256 --ignore-missing -c SHA256SUMS
```

Your archive should say `OK`.

## Install

Extract the archive and enter its folder. Replace the version and platform below with your download:

```sh
tar -xzf hashprobe-0.2.0-macos-arm64.tar.gz
```

```sh
cd hashprobe-0.2.0-macos-arm64
```

Install for your user:

```sh
mkdir -p "$HOME/.local/bin"
```

```sh
install -m 755 bin/hashprobe bin/hashprobe-mcp "$HOME/.local/bin/"
```

Add this line to `~/.zshrc` or `~/.bashrc` and run it in your current terminal:

```sh
export PATH="$HOME/.local/bin:$PATH"
```

Check the installation:

```sh
hashprobe self-test
```

To use it with an agent, follow [agent setup](https://github.com/ArcX-Research/hashprobe#connect-an-agent). See the [MCP guide](https://github.com/ArcX-Research/hashprobe/blob/main/mcp/README.md) for other apps and custom programs.

## Try the included example

From the extracted folder:

```sh
hashprobe check --report example.json -- ./bin/sha256-target
```

Expect `117 passed`. Use a new report filename for each run. This example shares Hashprobe's reference code; it demonstrates the workflow.
