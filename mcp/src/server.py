"""The four public MCP tools. Protocol handling belongs to the official SDK."""
from contextlib import asynccontextmanager, contextmanager

from mcp.server import MCPServer
from mcp.server.mcpserver.exceptions import ToolError
from mcp.types import ToolAnnotations
from pydantic import StrictBool

from . import __version__
from .config import Name
from .models import (CaseId, FailureIndex, FailureResult, MaxBytes, Offset, PageSize,
                     RandomCases, ReportId, RunResult, Seed, TargetList, Timeout)
from .runner import HashprobeService


@contextmanager
def tool_errors():
    """Give agents a useful message for expected input, file, and run errors."""
    try:
        yield
    except (ValueError, OSError) as error:
        raise ToolError(str(error)) from None


def create_server(service: HashprobeService) -> MCPServer:
    @asynccontextmanager
    async def lifespan(_server):
        try:
            yield
        finally:
            await service.close()

    server = MCPServer(
        "hashprobe", version=__version__, lifespan=lifespan, log_level="WARNING",
        instructions=(
            "Test configured SHA-256 programs. Start with list_targets, then check. "
            "A mismatch is a completed test finding, not a failed MCP call. "
            "Use get_failure to inspect saved inputs and replay after a fix. "
            "Only a complete run with status pass confirms all selected tests passed. "
            "The tools test digest correctness, not whole-product security."
        ),
    )
    read_only = ToolAnnotations(readOnlyHint=True, destructiveHint=False, openWorldHint=False)
    # Configured programs can have side effects. Do not describe execution as
    # read-only, harmless, or idempotent just because Hashprobe is a tester.
    executes = ToolAnnotations(readOnlyHint=False, destructiveHint=True,
                               idempotentHint=False, openWorldHint=True)

    @server.tool(annotations=read_only)
    def list_targets() -> TargetList:
        """List the owner-configured programs and time limits available for testing."""
        return service.list_targets()

    @server.tool(annotations=executes)
    async def check(target: Name, seed: Seed = 0, random_cases: RandomCases = 32,
                    max_bytes: MaxBytes = 4096, timeout_ms: Timeout | None = None,
                    fail_fast: StrictBool = False) -> RunResult:
        """Run SHA-256 tests on a configured target. Save a report and return counts and the first ten failures.

        The same seed and input settings reproduce a test set. timeout_ms can
        lower the owner's per-test limit. A mismatch is a result to investigate.
        Read exact failure bytes with get_failure; report_id refers to local storage.
        """
        with tool_errors():
            return await service.check(target, seed, random_cases, max_bytes, timeout_ms, fail_fast)

    @server.tool(annotations=read_only)
    def get_failure(report_id: ReportId, index: FailureIndex = 0, offset: Offset = 0,
                    limit: PageSize = 256) -> FailureResult:
        """Read one saved failure and a page of its input bytes as hex.

        index counts failed/errored cases from zero. offset and limit count bytes,
        not hex characters. Follow next_offset for more input and next_failure_index
        for another case. Reading a report never executes the command stored in it.
        """
        with tool_errors():
            return service.get_failure(report_id, index, offset, limit)

    @server.tool(annotations=executes)
    async def replay(report_id: ReportId, target: Name, case_id: CaseId | None = None,
                     timeout_ms: Timeout | None = None, fail_fast: StrictBool = False) -> RunResult:
        """Retest saved failures using the named configured target and save a new report.

        Optionally select one case_id returned by get_failure. This tests only the
        saved failures; use check afterward for the full test set. The C engine
        verifies saved input bytes against their expected SHA-256 hashes first.
        """
        with tool_errors():
            return await service.replay(report_id, target, case_id, timeout_ms, fail_fast)

    return server
