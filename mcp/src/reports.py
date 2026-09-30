"""Own report files by ID, bound their size, and serialize writers."""
from contextlib import contextmanager
import fcntl
import json
import os
from pathlib import Path
import re
import stat

from .config import Settings, unique_object

MAX_REPORT = 40 * 1024 * 1024
REPORT_ID = re.compile(r"[0-9a-f]{32}\Z")
DIGEST = re.compile(r"[0-9a-fA-F]{64}\Z")
CASE_ID = re.compile(r"[a-zA-Z0-9_-]{1,79}\Z")


class ReportStore:
    def __init__(self, settings: Settings):
        self.directory = Path(settings.report_dir)
        self.directory.mkdir(mode=0o700, parents=True, exist_ok=True)
        self.settings = settings

    def path(self, report_id: str) -> Path:
        if not REPORT_ID.fullmatch(report_id):
            raise ValueError("invalid report ID")
        return self.directory / f"{report_id}.json"

    @contextmanager
    def reserve_run(self):
        fd = os.open(self.directory / ".lock", os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
        try:
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                raise ValueError("another test run is using this report directory; retry when it finishes") from None
            count = size = 0
            for path in self.directory.glob("*.json"):
                if REPORT_ID.fullmatch(path.stem):
                    count += 1
                    size += path.lstat().st_size
            if count >= self.settings.max_reports:
                raise ValueError("report count limit reached; the owner must archive or remove old reports")
            if size + MAX_REPORT > self.settings.max_storage_mb * 1024 * 1024:
                raise ValueError("report storage limit reached; the owner must archive or remove old reports")
            yield
        finally:
            os.close(fd)

    def load(self, report_id: str) -> dict:
        path = self.path(report_id)
        try:
            fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
            with os.fdopen(fd, "rb") as stream:
                info = os.fstat(stream.fileno())
                if not stat.S_ISREG(info.st_mode) or info.st_size > MAX_REPORT:
                    raise ValueError("report is not a regular file of at most 40 MiB")
                data = stream.read(MAX_REPORT + 1)
            if len(data) > MAX_REPORT:
                raise ValueError("report exceeds 40 MiB")
            report = json.loads(data, object_pairs_hook=unique_object)
            validate_report(report)
            return report
        except (OSError, ValueError, TypeError, KeyError, RecursionError) as error:
            raise ValueError("report is unavailable or invalid") from error


def validate_report(report: dict) -> None:
    if (not isinstance(report, dict) or report.get("schema_version") != 1 or
            report.get("algorithm") != "sha256" or not isinstance(report.get("tool"), dict) or
            report["tool"].get("name") != "hashprobe"):
        raise ValueError("unsupported report")
    results = report["results"]
    summary = report["summary"]
    if not isinstance(results, list) or len(results) > 1024 or not isinstance(summary, dict):
        raise ValueError("invalid results")
    counts = {"pass": 0, "mismatch": 0, "error": 0}
    identifiers = set()
    total_bytes = 0
    for case in results:
        if not isinstance(case, dict):
            raise ValueError("invalid case")
        case_id, size, status = case["id"], case["input_bytes"], case["status"]
        if (not isinstance(case_id, str) or not CASE_ID.fullmatch(case_id) or case_id in identifiers or
                type(size) is not int or not 0 <= size <= 1048576 or status not in counts):
            raise ValueError("invalid case")
        identifiers.add(case_id)
        counts[status] += 1
        total_bytes += size
        if total_bytes > 16 * 1024 * 1024 or not DIGEST.fullmatch(case["expected_hex"]):
            raise ValueError("invalid case data")
        actual = case.get("actual_hex")
        if actual is not None and not DIGEST.fullmatch(actual):
            raise ValueError("invalid actual digest")
        if status in ("pass", "mismatch"):
            if actual is None or (actual.lower() == case["expected_hex"].lower()) != (status == "pass"):
                raise ValueError("digest does not match case status")
        if case.get("error") is not None and not isinstance(case["error"], dict):
            raise ValueError("invalid diagnostic")
        if status != "pass":
            encoded = case["input_hex"]
            if not isinstance(encoded, str) or len(encoded) != size * 2 or not re.fullmatch(r"[0-9a-fA-F]*", encoded):
                raise ValueError("invalid saved input")
    for key in ("planned", "executed", "passed", "mismatches", "errors", "skipped"):
        if type(summary[key]) is not int or not 0 <= summary[key] <= 1024:
            raise ValueError("invalid counts")
    if (summary["executed"] != len(results) or summary["passed"] != counts["pass"] or
            summary["mismatches"] != counts["mismatch"] or summary["errors"] != counts["error"] or
            summary["planned"] != summary["executed"] + summary["skipped"] or
            type(summary["complete"]) is not bool or
            report["status"] not in ("pass", "mismatch", "error", "interrupted")):
        raise ValueError("inconsistent report")
    interrupted = report["status"] == "interrupted"
    complete = summary["executed"] == summary["planned"] and not interrupted
    status = ("interrupted" if interrupted else "error" if counts["error"] else
              "mismatch" if counts["mismatch"] else "pass" if complete else "error")
    if summary["complete"] != complete or report["status"] != status:
        raise ValueError("status does not match results")
    target = report["target"]
    if not isinstance(target, dict) or not isinstance(target.get("executable"), dict):
        raise ValueError("invalid target metadata")
    version = target.get("version_tag")
    digest = target["executable"].get("sha256")
    if version is not None and (not isinstance(version, str) or len(version) > 1024):
        raise ValueError("invalid target version")
    if digest is not None and (not isinstance(digest, str) or not DIGEST.fullmatch(digest)):
        raise ValueError("invalid executable digest")


def failures(report: dict) -> list[dict]:
    return [case for case in report["results"] if case["status"] != "pass"]
