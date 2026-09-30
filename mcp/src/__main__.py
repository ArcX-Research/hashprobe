"""Install-time helpers and the stdio MCP entry point."""
import argparse
import json
import os
from pathlib import Path
import shutil
import sys

from . import __version__
from .config import Settings, load_settings, resolve_target

DEFAULT_CONFIG = "~/.config/hashprobe/mcp.json"


def client_config(path: Path) -> dict:
    return {"mcpServers": {"hashprobe": {
        "command": sys.executable,
        "args": ["-m", "hashprobe", "--config", str(path)],
    }}}


def initialize(args) -> None:
    path = Path(args.config).expanduser().resolve()
    command = args.command
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        openssl = shutil.which("openssl")
        if not openssl:
            raise ValueError("OpenSSL was not found; provide your program after --")
        command = [openssl, "dgst", "-sha256", "-binary"]
        output, name = "binary", args.name or "openssl"
    else:
        output, name = args.output, args.name or "custom"
    settings = Settings.model_validate({
        "report_dir": "reports",
        "targets": {name: {"command": command, "cwd": str(Path.cwd()), "output": output}},
    })
    resolve_target(settings.targets[name], path.parent)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w") as stream:
        json.dump(settings.model_dump(exclude_defaults=True), stream, indent=2)
        stream.write("\n")
    print(f"Created {path}. Add the following to your agent's MCP settings:", file=sys.stderr)
    print(json.dumps(client_config(path), indent=2))


def main() -> None:
    parser = argparse.ArgumentParser(description="Let agents test configured SHA-256 programs with Hashprobe.")
    parser.add_argument("--version", action="version", version=f"hashprobe-mcp {__version__}")
    parser.add_argument("--config", default=DEFAULT_CONFIG, help="owner-controlled target configuration")
    parser.add_argument("--engine", type=Path, help="use an existing Hashprobe executable (development)")
    commands = parser.add_subparsers(dest="operation")
    init = commands.add_parser("init", help="create a configuration and print agent connection settings")
    init.add_argument("--config", default=argparse.SUPPRESS)
    init.add_argument("--name", help="name for the configured program")
    init.add_argument("--output", choices=("hex", "binary"), default="hex")
    init.add_argument("command", nargs=argparse.REMAINDER, help="optional program and arguments after --")
    show = commands.add_parser("client-config", help="print agent connection settings for an existing configuration")
    show.add_argument("--config", default=argparse.SUPPRESS)
    args = parser.parse_args()
    try:
        if args.operation == "init":
            initialize(args)
            return
        path = Path(args.config).expanduser().resolve()
        settings = load_settings(path)
        if args.operation == "client-config":
            print(json.dumps(client_config(path), indent=2))
            return
        if sys.platform not in ("darwin", "linux"):
            raise ValueError("Hashprobe supports macOS and Linux")
        engine = args.engine or Path(__file__).parent / "bin" / "hashprobe"
        if not engine.is_file() or not os.access(engine, os.X_OK):
            raise ValueError("C engine not found; install the package or pass --engine /path/to/hashprobe")
        from .runner import HashprobeService
        from .server import create_server
        create_server(HashprobeService(settings, engine)).run(transport="stdio")
    except (OSError, ValueError) as error:
        print(f"hashprobe-mcp: {error}", file=sys.stderr)
        raise SystemExit(2) from error


if __name__ == "__main__":
    main()
