"""Load owner-controlled target commands and server limits."""
from pathlib import Path
import json
import os
import shutil
from typing import Annotated, Literal

from pydantic import BaseModel, ConfigDict, Field, StrictInt, field_validator

Name = Annotated[str, Field(pattern=r"^[a-zA-Z0-9_-]{1,64}$")]


class Target(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)
    command: list[str] = Field(min_length=1, max_length=128)
    cwd: str = "."
    output: Literal["hex", "binary"] = "hex"
    description: str = Field(default="", max_length=512)
    version: str | None = Field(default=None, max_length=1024)

    @field_validator("command")
    @classmethod
    def valid_command(cls, command):
        if not command[0] or any("\0" in part for part in command):
            raise ValueError("command must name a program and contain no zero bytes")
        if sum(len(part.encode()) + 1 for part in command) > 65536:
            raise ValueError("command exceeds 65536 bytes")
        return command


class Settings(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)
    targets: dict[Name, Target] = Field(min_length=1, max_length=32)
    report_dir: str = "reports"
    timeout_ms: StrictInt = Field(default=5000, ge=1, le=600000)
    run_timeout_seconds: StrictInt = Field(default=120, ge=1, le=600)
    max_reports: StrictInt = Field(default=100, ge=1, le=10000)
    max_storage_mb: StrictInt = Field(default=256, ge=40, le=4096)


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def load_settings(path: Path) -> Settings:
    path = path.expanduser().resolve(strict=True)
    with path.open("rb") as stream:
        data = stream.read(65537)
    if len(data) > 65536:
        raise ValueError("configuration exceeds 65536 bytes")
    settings = Settings.model_validate(json.loads(data, object_pairs_hook=unique_object))
    settings.report_dir = str(resolve_path(settings.report_dir, path.parent))
    for target in settings.targets.values():
        resolve_target(target, path.parent)
    return settings


def resolve_target(target: Target, base: Path) -> None:
    target.cwd = str(resolve_path(target.cwd, base))
    if not Path(target.cwd).is_dir():
        raise ValueError(f"target working directory does not exist: {target.cwd}")
    program = target.command[0]
    if "/" in program or program.startswith("~"):
        executable = Path(program).expanduser()
        if not executable.is_absolute():
            executable = Path(target.cwd) / executable
    else:
        found = shutil.which(program)
        if not found:
            raise ValueError(f"target program not found: {program}")
        executable = Path(found)
    if not executable.is_file() or not os.access(executable, os.X_OK):
        raise ValueError(f"target program is not executable: {executable}")
    # Keep executable symlinks: resolving a virtual environment's Python to
    # its base interpreter would silently discard the environment's packages.
    target.command[0] = os.path.abspath(executable)


def resolve_path(value: str, base: Path) -> Path:
    path = Path(value).expanduser()
    return (path if path.is_absolute() else base / path).resolve()
