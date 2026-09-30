"""Small, explicit results for agents; full reports stay on disk."""
from typing import Annotated, Literal
from pydantic import BaseModel, Field, StrictInt

ReportId = Annotated[str, Field(pattern=r"^[0-9a-f]{32}$")]
CaseId = Annotated[str, Field(pattern=r"^[a-zA-Z0-9_-]{1,79}$")]
Seed = Annotated[StrictInt, Field(ge=0, le=4294967295)]
RandomCases = Annotated[StrictInt, Field(ge=0, le=256)]
MaxBytes = Annotated[StrictInt, Field(ge=1, le=65536)]
Timeout = Annotated[StrictInt, Field(ge=1, le=600000)]
FailureIndex = Annotated[StrictInt, Field(ge=0, le=1023)]
Offset = Annotated[StrictInt, Field(ge=0, le=1048576)]
PageSize = Annotated[StrictInt, Field(ge=1, le=4096)]
Status = Literal["pass", "mismatch", "error", "interrupted"]


class TargetInfo(BaseModel):
    name: str
    description: str
    output: Literal["hex", "binary"]
    version: str | None


class TargetList(BaseModel):
    targets: list[TargetInfo]
    timeout_ms: int
    run_timeout_seconds: int


class Counts(BaseModel):
    planned: int
    executed: int
    passed: int
    mismatches: int
    errors: int
    skipped: int
    complete: bool


class FailureBrief(BaseModel):
    case_id: str
    status: Literal["mismatch", "error"]
    input_bytes: int


class RunResult(BaseModel):
    report_id: str
    target: str
    status: Status
    counts: Counts
    failures: list[FailureBrief]
    failure_count: int
    next_failure_index: int | None
    target_version: str | None
    executable_sha256: str | None
    stopped_reason: Literal["run_timeout"] | None = None


class FailureResult(BaseModel):
    report_id: str
    case_id: str
    index: int
    failure_count: int
    next_failure_index: int | None
    status: Literal["mismatch", "error"]
    input_bytes: int
    expected_hex: str
    actual_hex: str | None
    offset: int
    input_hex: str
    next_offset: int | None
    error: dict | None
