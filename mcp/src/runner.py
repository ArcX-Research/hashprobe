"""Run the C engine without a shell and translate its reports for agents."""
import asyncio
from pathlib import Path
import signal
import uuid

import anyio

from .config import Settings
from .models import Counts, FailureBrief, FailureResult, RunResult, TargetInfo, TargetList
from .reports import ReportStore, failures


class HashprobeService:
    def __init__(self, settings: Settings, engine: Path):
        self.settings = settings
        self.engine = engine.resolve(strict=True)
        self.store = ReportStore(settings)
        self.active: asyncio.subprocess.Process | None = None
        self.busy = False

    def list_targets(self) -> TargetList:
        return TargetList(
            targets=[TargetInfo(name=name, description=target.description,
                                output=target.output, version=target.version)
                     for name, target in sorted(self.settings.targets.items())],
            timeout_ms=self.settings.timeout_ms,
            run_timeout_seconds=self.settings.run_timeout_seconds,
        )

    def get_failure(self, report_id: str, index: int, offset: int, limit: int) -> FailureResult:
        cases = failures(self.store.load(report_id))
        if not 0 <= index < len(cases):
            raise ValueError("failure index is outside this report's saved failures")
        case = cases[index]
        size = case["input_bytes"]
        if not 0 <= offset <= size or not 1 <= limit <= 4096:
            raise ValueError("input offset or page size is outside the allowed range")
        end = min(size, offset + limit)
        error = case.get("error")
        if error:
            # Only expose the bounded diagnostic fields produced by the C runner.
            error = {key: error[key] for key in ("kind", "detail", "exit_status", "signal", "stdout_hex", "stderr_hex")
                     if key in error}
            if len(str(error)) > 4096:
                raise ValueError("invalid saved diagnostic")
        return FailureResult(
            report_id=report_id, case_id=case["id"], index=index, failure_count=len(cases),
            next_failure_index=index + 1 if index + 1 < len(cases) else None,
            status=case["status"], input_bytes=size, expected_hex=case["expected_hex"],
            actual_hex=case.get("actual_hex"), offset=offset,
            input_hex=case["input_hex"][offset * 2:end * 2],
            next_offset=end if end < size else None, error=error,
        )

    async def check(self, target: str, seed: int, random_cases: int, max_bytes: int,
                    timeout_ms: int | None, fail_fast: bool) -> RunResult:
        arguments = ["--seed", str(seed), "--random-cases", str(random_cases), "--max-bytes", str(max_bytes)]
        return await self._run(target, "check", arguments, timeout_ms, fail_fast)

    async def replay(self, report_id: str, target: str, case_id: str | None,
                     timeout_ms: int | None, fail_fast: bool) -> RunResult:
        cases = failures(self.store.load(report_id))
        if not cases or (case_id is not None and not any(case["id"] == case_id for case in cases)):
            raise ValueError("report has no matching saved failure")
        arguments = [str(self.store.path(report_id))]
        if case_id is not None:
            arguments += ["--case", case_id]
        return await self._run(target, "replay", arguments, timeout_ms, fail_fast)

    async def _run(self, name: str, mode: str, arguments: list[str],
                   timeout_ms: int | None, fail_fast: bool) -> RunResult:
        target = self.settings.targets.get(name)
        if target is None:
            raise ValueError("unknown target; call list_targets to see the configured programs")
        timeout = self.settings.timeout_ms if timeout_ms is None else timeout_ms
        if not 1 <= timeout <= self.settings.timeout_ms:
            raise ValueError(f"timeout_ms must be between 1 and the configured limit {self.settings.timeout_ms}")
        if self.busy:
            raise ValueError("another test run is active; retry when it finishes")
        self.busy = True
        try:
            with self.store.reserve_run():
                report_id = uuid.uuid4().hex
                command = [str(self.engine), mode, *arguments, "--report", str(self.store.path(report_id)),
                           "--output", target.output, "--timeout-ms", str(timeout)]
                if target.version is not None:
                    command += ["--target-version", target.version]
                if fail_fast:
                    command.append("--fail-fast")
                command += ["--", *target.command]
                stderr, stopped = await self._execute(command, target.cwd)
                try:
                    report = self.store.load(report_id)
                except ValueError:
                    message = stderr.decode("utf-8", errors="backslashreplace").strip()[:800]
                    raise ValueError("Hashprobe did not save a report: " + (message or "run stopped before a report was written")) from None
                return self._summary(report_id, name, report, stopped)
        finally:
            self.busy = False

    async def _execute(self, command: list[str], cwd: str) -> tuple[bytes, bool]:
        # Shield process creation so cancellation cannot leave a child whose
        # handle we never received. Cleanup is also shielded below.
        with anyio.CancelScope(shield=True):
            process = await asyncio.create_subprocess_exec(
                *command, cwd=cwd, stdin=asyncio.subprocess.DEVNULL,
                stdout=asyncio.subprocess.DEVNULL, stderr=asyncio.subprocess.PIPE,
                start_new_session=True,
            )
            self.active = process
            reader = asyncio.create_task(self._read_stderr(process.stderr))
        stopped = False
        try:
            try:
                await asyncio.wait_for(process.wait(), self.settings.run_timeout_seconds)
            except asyncio.TimeoutError:
                stopped = True
        finally:
            with anyio.CancelScope(shield=True):
                await self._stop(process)
                diagnostics = await reader
                self.active = None
        return diagnostics, stopped

    @staticmethod
    async def _read_stderr(stream: asyncio.StreamReader) -> bytes:
        saved = bytearray()
        while chunk := await stream.read(4096):
            saved.extend(chunk[:max(0, 16384 - len(saved))])
        return bytes(saved)

    @staticmethod
    async def _stop(process: asyncio.subprocess.Process) -> None:
        if process.returncode is not None:
            return
        try:
            # The C engine handles SIGTERM by stopping its current target's
            # process group and saving the results collected so far.
            process.send_signal(signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            await asyncio.wait_for(process.wait(), 3)
        except asyncio.TimeoutError:
            try:
                process.kill()
            except ProcessLookupError:
                pass
            await process.wait()

    async def close(self) -> None:
        with anyio.CancelScope(shield=True):
            if self.active is not None:
                await self._stop(self.active)

    @staticmethod
    def _summary(report_id: str, target: str, report: dict, stopped: bool) -> RunResult:
        failed = failures(report)
        counts = Counts.model_validate(report["summary"])
        if stopped:
            counts.complete = False
        return RunResult(
            report_id=report_id, target=target, status="error" if stopped else report["status"],
            counts=counts, failure_count=len(failed),
            failures=[FailureBrief(case_id=case["id"], status=case["status"], input_bytes=case["input_bytes"])
                      for case in failed[:10]],
            next_failure_index=10 if len(failed) > 10 else None,
            target_version=report["target"].get("version_tag"),
            executable_sha256=report["target"].get("executable", {}).get("sha256"),
            stopped_reason="run_timeout" if stopped else None,
        )
