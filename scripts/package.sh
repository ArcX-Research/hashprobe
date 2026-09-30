#!/bin/sh
# Package existing native binaries. Run from the repository root.
set -eu

build=${1:-build}
out=${2:-dist}
version=$("$build/hashprobe" --version | awk '{print $2}')
printf '%s\n' "$version" | LC_ALL=C grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$'
test "$("$build/hashprobe-mcp" --version)" = "hashprobe-mcp $version"

case $(uname -s) in
    Linux) platform=linux ;;
    Darwin) platform=macos ;;
    *) echo 'Packaging supports Linux and macOS.' >&2; exit 1 ;;
esac
case $(uname -m) in
    x86_64) arch=x86_64 ;;
    aarch64|arm64) arch=arm64 ;;
    *) echo 'Packaging supports x86_64 and ARM64.' >&2; exit 1 ;;
esac

name="hashprobe-$version-$platform-$arch"
stage=$(mktemp -d "${TMPDIR:-/tmp}/hashprobe-package.XXXXXX")
trap 'rm -rf "$stage"' 0
trap 'exit 1' 1 2 15

mkdir -p "$stage/$name/bin" "$stage/$name/licenses" "$out"
for binary in hashprobe hashprobe-mcp sha256-target; do
    install -m 755 "$build/$binary" "$stage/$name/bin/$binary"
done
install -m 644 LICENSE "$stage/$name/LICENSE"
install -m 644 vendor/cjson/LICENSE "$stage/$name/licenses/cJSON.txt"
install -m 644 docs/BINARIES.md "$stage/$name/README.md"
COPYFILE_DISABLE=1 tar -czf "$stage/$name.tar.gz" -C "$stage" "$name"
mv "$stage/$name.tar.gz" "$out/$name.tar.gz"
printf '%s\n' "$out/$name.tar.gz"
