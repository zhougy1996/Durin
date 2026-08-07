from __future__ import annotations

import io
import json
import sys
from pathlib import Path

import pytest
from jsonschema import validate

DEV_TOOL_ROOT = Path(__file__).resolve().parents[1]
REPOSITORY_ROOT = DEV_TOOL_ROOT.parents[1]
if str(DEV_TOOL_ROOT) not in sys.path:
    sys.path.insert(0, str(DEV_TOOL_ROOT))

from durin_dev_tool import assertion_audit, cli
from durin_dev_tool.errors import DevToolError


FIXTURES = REPOSITORY_ROOT / "CMake/Tests/Fixtures/AssertionScanner"


def test_registry_exposes_assertion_audit() -> None:
    output = io.StringIO()
    assert cli.run(
        ["audit", "assertions", "CMake/Tests/Fixtures/AssertionScanner/OrderingA.cpp", "--format", "json"],
        repository_root=REPOSITORY_ROOT,
        stdout=output,
        stderr=io.StringIO(),
    ) == 0
    assert json.loads(output.getvalue())["frontend"] == "libclang-token-stream"


def test_presubmit_enforcement_rejects_unreviewed_check_and_accepts_classified_verify() -> None:
    assert cli.run(
        ["audit", "assertions", str(FIXTURES / "PresubmitUnreviewed.cpp"), "--enforce"],
        repository_root=REPOSITORY_ROOT,
        stdout=io.StringIO(),
        stderr=io.StringIO(),
    ) == 1
    assert cli.run(
        ["audit", "assertions", str(FIXTURES / "PresubmitVerify.cpp"), "--enforce"],
        repository_root=REPOSITORY_ROOT,
        stdout=io.StringIO(),
        stderr=io.StringIO(),
    ) == 0


def test_scanner_is_sorted_deterministic_and_covers_constructs() -> None:
    paths = [
        FIXTURES / "OrderingB.cpp", FIXTURES / "OrderingA.cpp",
        FIXTURES / "FalsePositives.cpp", FIXTURES / "Constructs.cpp",
        FIXTURES / "Generated.template",
    ]
    first = assertion_audit.scan(REPOSITORY_ROOT, paths)
    second = assertion_audit.scan(REPOSITORY_ROOT, list(reversed(paths)))
    assert json.dumps(first, indent=2) == json.dumps(second, indent=2)
    findings = first["findings"]
    identities = [(item["path"], item["line"], item["constructKind"]) for item in findings]
    assert identities == sorted(identities, key=lambda item: (item[0].casefold(), item[1], item[2]))
    kinds = {item["constructKind"] for item in findings}
    assert {
        "direct-call", "callback", "traversal", "assignment",
        "increment-decrement", "allocation", "deallocation",
        "coroutine-transition", "comma-expression", "macro-definition",
        "potentially-overloaded-operation",
    } <= kinds
    assert any(item["sourceKind"] == "macro-definition" for item in findings)
    assert any(item["sourceKind"] == "scaffolding-template" for item in findings)
    assert any(item["macro"] == "requiref" and item["classification"] == "enforced-runtime-contract" for item in findings)
    assert all(
        item["allowlistDisposition"] == "classified"
        for item in findings if item["macro"] in {"require", "requiref"} and item["sourceKind"] == "invocation"
    )
    check_sites = {
        (item["path"], item["line"], item["column"])
        for item in findings
        if item["macro"] == "check" and item["sourceKind"] == "invocation"
    }
    assert len(check_sites) == 9


def test_malformed_source_fails_visibly() -> None:
    with pytest.raises(DevToolError, match=r"Malformed\.cpp:.*(unterminated|mismatched delimiter)"):
        assertion_audit.scan(REPOSITORY_ROOT, [FIXTURES / "Malformed.cpp"])
    with pytest.raises(DevToolError, match=r"MalformedExpression\.cpp:3:.*libclang could not parse"):
        assertion_audit.scan(REPOSITORY_ROOT, [FIXTURES / "MalformedExpression.cpp"])


def test_allowlist_is_exact_and_rejects_stale_entries(tmp_path: Path) -> None:
    report = assertion_audit.scan(REPOSITORY_ROOT, [FIXTURES / "OrderingA.cpp"])
    identifier = report["findings"][0]["id"]
    allowlist = tmp_path / "allowlist.json"
    allowlist.write_text(json.dumps({"schemaVersion": 1, "entries": [{"id": identifier, "rationale": "fixture"}]}), encoding="utf-8")
    allowed = assertion_audit.scan(REPOSITORY_ROOT, [FIXTURES / "OrderingA.cpp"], allowlist)
    assert allowed["summary"]["allowed"] == 1
    allowlist.write_text(json.dumps({"schemaVersion": 1, "entries": [{"id": "missing", "rationale": "fixture"}]}), encoding="utf-8")
    with pytest.raises(DevToolError, match="Stale assertion allowlist"):
        assertion_audit.scan(REPOSITORY_ROOT, [FIXTURES / "OrderingA.cpp"], allowlist)


def test_reports_and_allowlists_match_tracked_schemas() -> None:
    schema_root = DEV_TOOL_ROOT / "schemas"
    report = assertion_audit.scan(REPOSITORY_ROOT, [FIXTURES / "OrderingA.cpp"])
    validate(report, json.loads((schema_root / "assertion-side-effect-findings-v1.schema.json").read_text(encoding="utf-8")))
    validate(
        {"schemaVersion": 1, "entries": []},
        json.loads((schema_root / "assertion-side-effect-allowlist-v1.schema.json").read_text(encoding="utf-8")),
    )
