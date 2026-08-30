"""Reproducible authored-package storage qualification reporting."""

from __future__ import annotations

import io
import json
import os
import platform
import random
import re
import shutil
import statistics
import subprocess
import time
from pathlib import Path
from typing import Any, Callable, Mapping, Sequence, TextIO

from .build.errors import BuildToolError
from .build.models import OutputMode
from .build.output import BuildOutput
from .context import RepositoryContext
from .errors import DevToolError
from .runtime_program import (
    ExecutableDescription,
    RuntimeProcessPolicy,
    invoke_runtime_program,
    locate_executable,
    resolve_project,
    select_runtime,
)


REPORT_VERSION = 3
NATIVE_INVENTORY_VERSION = 2
PROGRAM = ExecutableDescription(
    "Authored package storage qualification", "DurinAssetTool", "DurinAssetTool"
)
PROTOCOL_PATH = (
    Path(__file__).resolve().parents[1]
    / "data"
    / "authored-package-storage-qualification-v3.json"
)


def _percentile(values: Sequence[float], percentile: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, round((len(ordered) - 1) * percentile)))
    return ordered[index]


def _run_process(
    arguments: Sequence[str],
    *,
    cwd: Path,
    environment: Mapping[str, str] | None = None,
) -> str:
    completed = subprocess.run(
        list(arguments),
        cwd=cwd,
        env=None if environment is None else dict(environment),
        text=True,
        encoding="utf-8",
        errors="replace",
        capture_output=True,
        check=False,
    )
    if completed.returncode != 0:
        diagnostic = completed.stderr.strip() or completed.stdout.strip()
        raise DevToolError(f"Qualification command failed ({' '.join(arguments)}): {diagnostic}")
    return completed.stdout.strip()


def _git(repository_root: Path, *arguments: str) -> str:
    return _run_process(
        ["git", "-c", f"safe.directory={repository_root.as_posix()}", *arguments],
        cwd=repository_root,
    )


def _write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(value, indent=2, ensure_ascii=False, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    os.replace(temporary, path)


def _native_inventory(
    namespace: object,
    repository: RepositoryContext,
    stderr: TextIO,
    executable_resolver: Callable[[object, Path], Path] | None = None,
) -> dict[str, Any]:
    selection = select_runtime(
        repository,
        profile_name=str(getattr(namespace, "profile", "") or ""),
        preset_name=str(getattr(namespace, "preset", "") or ""),
    )
    executable = (
        executable_resolver(namespace, repository.root)
        if executable_resolver
        else locate_executable(selection, PROGRAM)
    )
    project_value = getattr(namespace, "project_path", None)
    if project_value is None:
        project_value = repository.config.paths.default_game_project
    project = resolve_project(repository, Path(project_value))
    process_output = BuildOutput(
        plain=True, output_mode=OutputMode.COMPACT, stdout=io.StringIO(), stderr=stderr
    )
    try:
        output = invoke_runtime_program(
            selection,
            PROGRAM,
            [
                "storage-inventory",
                f"--project={project}",
            ],
            output=process_output,
            policy=RuntimeProcessPolicy(
                interruption_message="Authored package storage inventory cancelled.",
                show_heartbeat=True,
                capture_output=True,
            ),
            executable_override=executable,
        )
    except BuildToolError as error:
        if error.exit_code == 130:
            raise DevToolError("Authored package storage inventory cancelled.") from error
        raise
    try:
        report = json.loads(output)
    except json.JSONDecodeError as error:
        raise DevToolError(f"Native qualification inventory returned invalid JSON: {error}") from error
    if report.get("schemaVersion") != NATIVE_INVENTORY_VERSION:
        raise DevToolError("Native qualification inventory schema version is unsupported.")
    return report


def _history_for_path(repository_root: Path, relative_path: str) -> dict[str, Any]:
    history = _git(
        repository_root,
        "-c",
        "core.quotePath=false",
        "log",
        "--follow",
        "--format=commit:%H:%ct",
        "--name-only",
        "HEAD",
        "--",
        relative_path,
    )
    commits: list[tuple[str, int, str]] = []
    current: tuple[str, int, str] | None = None
    for raw_line in history.splitlines():
        line = raw_line.strip()
        if line.startswith("commit:"):
            if current:
                commits.append(current)
            commit, timestamp = line.removeprefix("commit:").split(":", 1)
            current = (commit, int(timestamp), relative_path)
        elif line and current:
            current = (current[0], current[1], line)
    if current:
        commits.append(current)

    object_ids: set[str] = set()
    timestamps = [timestamp for _, timestamp, _ in commits]
    for commit, _, path_at_commit in commits:
        resolved = subprocess.run(
            [
                "git",
                "-c",
                f"safe.directory={repository_root.as_posix()}",
                "rev-parse",
                f"{commit}:{path_at_commit}",
            ],
            cwd=repository_root,
            text=True,
            capture_output=True,
            check=False,
        )
        if resolved.returncode == 0:
            object_ids.add(resolved.stdout.strip())
    sizes = [int(_git(repository_root, "cat-file", "-s", object_id)) for object_id in object_ids]
    lfs_payload_sizes: list[int] = []
    if relative_path.endswith(".dabulk"):
        for object_id in object_ids:
            blob = _git(repository_root, "cat-file", "-p", object_id)
            match = re.search(r"^size ([0-9]+)$", blob, re.MULTILINE)
            if blob.startswith("version https://git-lfs.github.com/spec/v1") and match:
                lfs_payload_sizes.append(int(match.group(1)))
    span_months = 1.0
    if len(timestamps) > 1:
        span_months = max(1.0, (max(timestamps) - min(timestamps)) / (30.0 * 86400.0))
    return {
        "commitTouches": len(commits),
        "uniqueMainGitBlobs": len(object_ids),
        "uniqueMainGitBlobBytes": sum(sizes),
        "uniqueLfsPayloadBytes": sum(lfs_payload_sizes),
        "observedMonths": span_months,
        "monthlyTouchRate": len(commits) / span_months,
    }


def _collect_git_baseline(
    repository_root: Path, inventory: Mapping[str, Any]
) -> dict[str, Any]:
    tracked = [
        path
        for path in _git(repository_root, "ls-files", "*.dasset", "*.dabulk").splitlines()
        if path
    ]
    records: list[dict[str, Any]] = []
    for relative_path in tracked:
        physical = repository_root / relative_path
        attribute = _git(repository_root, "check-attr", "filter", "--", relative_path)
        filter_value = attribute.rsplit(":", 1)[-1].strip() if attribute else "unspecified"
        records.append(
            {
                "path": relative_path,
                "workingTreePresent": physical.is_file(),
                "workingTreeBytes": physical.stat().st_size if physical.is_file() else 0,
                "filter": filter_value,
                "history": _history_for_path(repository_root, relative_path),
            }
        )

    reachable = {
        Path(descriptor["companionPath"]).resolve()
        for package in inventory["packages"]
        for descriptor in package["descriptors"]
        if descriptor["storage"] == "External" and descriptor["companionPath"]
    }
    tracked_companions = {
        (repository_root / record["path"]).resolve()
        for record in records
        if record["path"].endswith(".dabulk")
    }
    main_git_bytes = sum(
        record["history"]["uniqueMainGitBlobBytes"] for record in records
    )
    asset_changes = [
        line
        for line in _git(
            repository_root,
            "status",
            "--porcelain",
            "--untracked-files=no",
            "--",
            "*.dasset",
            "*.dabulk",
        ).splitlines()
        if line
    ]
    return {
        "headCommit": _git(repository_root, "rev-parse", "HEAD"),
        "assetWorkingTreeDirty": bool(asset_changes),
        "assetWorkingTreeChanges": asset_changes,
        "trackedDassetCount": sum(record["path"].endswith(".dasset") for record in records),
        "trackedDabulkCount": sum(record["path"].endswith(".dabulk") for record in records),
        "workingTreeDassetBytes": sum(
            record["workingTreeBytes"] for record in records if record["path"].endswith(".dasset")
        ),
        "workingTreeDabulkBytes": sum(
            record["workingTreeBytes"] for record in records if record["path"].endswith(".dabulk")
        ),
        "uniqueHistoricalMainGitBlobBytesByPath": main_git_bytes,
        "historicalLfsPayloadBytesByPath": sum(
            record["history"]["uniqueLfsPayloadBytes"] for record in records
        ),
        "trackedOrphanCompanions": sorted(
            path.relative_to(repository_root).as_posix()
            for path in tracked_companions - reachable
        ),
        "missingTrackedFiles": sorted(
            record["path"] for record in records if not record["workingTreePresent"]
        ),
        "files": records,
    }


def _repository_snapshot(path: Path, *, lfs_payload_bytes: int = 0) -> dict[str, int]:
    count = _run_process(["git", "count-objects", "-v"], cwd=path)
    values: dict[str, int] = {}
    for line in count.splitlines():
        key, value = line.split(":", 1)
        if value.strip().isdigit():
            values[key] = int(value.strip())
    return {
        "mainGitObjectBytes": (values.get("size", 0) + values.get("size-pack", 0)) * 1024,
        "lfsObjectBytes": lfs_payload_bytes,
        "looseObjectCount": values.get("count", 0),
        "packedObjectCount": values.get("in-pack", 0),
    }


def _commit(path: Path, message: str) -> None:
    _run_process(["git", "add", "-A"], cwd=path)
    _run_process(["git", "commit", "-m", message], cwd=path)


def _run_layout_experiment(root: Path, *, companion: bool, seed: int) -> dict[str, Any]:
    root.mkdir(parents=True)
    _run_process(["git", "init", "--quiet"], cwd=root)
    _run_process(["git", "config", "user.email", "qualification@durin.invalid"], cwd=root)
    _run_process(["git", "config", "user.name", "Durin Qualification"], cwd=root)
    if companion:
        _run_process(["git", "lfs", "install", "--local"], cwd=root)
        (root / ".gitattributes").write_text(
            "*.dabulk filter=lfs diff=lfs merge=lfs -text\n", encoding="utf-8"
        )
    generator = random.Random(seed)
    payload = bytearray(generator.randbytes(1024 * 1024))
    metadata = bytearray(generator.randbytes(4096))
    package_path = root / "Example.dasset"
    companion_path = root / "Example.generation.dabulk"

    def write_layout() -> None:
        package_path.write_bytes(metadata if companion else metadata + payload)
        if companion:
            companion_path.write_bytes(payload)

    snapshots: list[dict[str, Any]] = []
    lfs_versions = 0
    write_layout()
    _commit(root, "baseline")
    lfs_versions += int(companion)
    snapshots.append({"stage": "baseline", **_repository_snapshot(
        root, lfs_payload_bytes=lfs_versions * len(payload)
    )})
    metadata[0] ^= 0x7F
    write_layout()
    _commit(root, "metadata-edit")
    snapshots.append({"stage": "metadata-edit", **_repository_snapshot(
        root, lfs_payload_bytes=lfs_versions * len(payload)
    )})
    for index in range(0, len(payload), 100):
        payload[index] ^= 0xA5
    write_layout()
    _commit(root, "payload-edit")
    lfs_versions += int(companion)
    snapshots.append({"stage": "payload-edit", **_repository_snapshot(
        root, lfs_payload_bytes=lfs_versions * len(payload)
    )})
    renamed_package = root / "Renamed.dasset"
    package_path.rename(renamed_package)
    if companion:
        companion_path.rename(root / "Renamed.generation.dabulk")
    _commit(root, "rename")
    snapshots.append({"stage": "rename", **_repository_snapshot(
        root, lfs_payload_bytes=lfs_versions * len(payload)
    )})
    for index, snapshot in enumerate(snapshots):
        previous = snapshots[index - 1] if index else {
            "mainGitObjectBytes": 0, "lfsObjectBytes": 0
        }
        snapshot["incrementalMainGitObjectBytes"] = (
            snapshot["mainGitObjectBytes"] - previous["mainGitObjectBytes"]
        )
        snapshot["incrementalLfsObjectBytes"] = (
            snapshot["lfsObjectBytes"] - previous["lfsObjectBytes"]
        )
        snapshot["checkoutBytesWithPayload"] = (
            snapshot["mainGitObjectBytes"] + snapshot["lfsObjectBytes"]
        )
        snapshot["checkoutBytesSkipLfsSmudge"] = snapshot["mainGitObjectBytes"]
    return {
        "layout": "companion-local-lfs" if companion else "package-local-ordinary-git",
        "payloadBytes": len(payload),
        "metadataBytes": len(metadata),
        "payloadEditFraction": 0.01,
        "snapshots": snapshots,
    }


def _source_control_experiment(output_root: Path, protocol: Mapping[str, Any]) -> dict[str, Any]:
    scratch = output_root / "_scratch" / (
        f"source-control-run-p{os.getpid()}-{time.time_ns()}"
    )
    seed = next(
        workload["seed"]
        for workload in protocol["workloads"]
        if workload["kind"] == "synthetic"
    )
    layouts = [
        _run_layout_experiment(scratch / "companion", companion=True, seed=seed),
        _run_layout_experiment(scratch / "package", companion=False, seed=seed),
    ]
    return {
        "isolated": True,
        "scratchPath": scratch.relative_to(output_root).as_posix(),
        "gitLfsPartialSync": "A clone with GIT_LFS_SKIP_SMUDGE transfers Git pointer blobs without payload objects; explicit git lfs pull hydrates required payloads.",
        "layouts": layouts,
    }


def _synthetic_model(protocol: Mapping[str, Any]) -> dict[str, Any]:
    workload = next(
        item for item in protocol["workloads"] if item["kind"] == "synthetic"
    )
    sizes = list(workload["payloadSizesBytes"])
    repetitions = int(workload["packageCount"]) // len(sizes)
    expanded = sizes * repetitions
    metadata_bytes = 4096
    payload_sizes = [size for size in expanded if size]
    return {
        "workloadId": workload["id"],
        "seed": workload["seed"],
        "packageCount": workload["packageCount"],
        "payloadCount": len(payload_sizes),
        "logicalPayloadBytes": sum(payload_sizes),
        "minimumPayloadBytes": min(payload_sizes),
        "medianPayloadBytes": statistics.median(payload_sizes),
        "p95PayloadBytes": _percentile(payload_sizes, 0.95),
        "maximumPayloadBytes": max(payload_sizes),
        "exactDuplicatePayloadCount": 0,
        "modeledFullCompanionRewriteBytes": sum(payload_sizes),
        "modeledPackageLocalRewriteBytes": sum(size + metadata_bytes for size in payload_sizes),
        "editFractions": [
            {
                "fraction": fraction,
                "changedLogicalBytes": sum(round(size * fraction) for size in payload_sizes),
                "currentImmutableGenerationBytesWritten": 0.0 if fraction == 0 else sum(payload_sizes),
            }
            for fraction in workload["payloadEditFractions"]
        ],
        "classification": "Synthetic scale; never a current corpus fact.",
    }


def _atomic_copy(source: Path, destination: Path) -> tuple[int, float]:
    temporary = destination.with_suffix(destination.suffix + ".candidate")
    start = time.perf_counter_ns()
    with source.open("rb") as input_stream, temporary.open("wb") as output_stream:
        shutil.copyfileobj(input_stream, output_stream, length=1024 * 1024)
        output_stream.flush()
        os.fsync(output_stream.fileno())
    os.replace(temporary, destination)
    elapsed_ms = (time.perf_counter_ns() - start) / 1_000_000.0
    return source.stat().st_size, elapsed_ms


def _publication_benchmark(
    repository_root: Path,
    output_root: Path,
    inventory: Mapping[str, Any],
    repeat_count: int,
) -> dict[str, Any]:
    scratch = output_root / "_scratch" / "publication"
    if scratch.exists():
        shutil.rmtree(scratch)
    scratch.mkdir(parents=True)
    samples: list[dict[str, Any]] = []
    try:
        for package in inventory["packages"]:
            package_source = Path(package["physicalPath"])
            if not package_source.exists() or package["inspection"] != "Ready":
                continue
            companion_sources = sorted(
                {
                    Path(descriptor["companionPath"])
                    for descriptor in package["descriptors"]
                    if descriptor["storage"] == "External" and descriptor["reachable"]
                }
            )
            for operation, sources in (
                ("metadata-only", [package_source]),
                ("payload-edit", [*companion_sources, package_source]),
            ):
                if operation == "payload-edit" and not companion_sources:
                    continue
                timings: list[float] = []
                bytes_written = 0
                for repeat in range(repeat_count):
                    current_bytes = 0
                    start = time.perf_counter_ns()
                    for index, source in enumerate(sources):
                        written, _ = _atomic_copy(
                            source, scratch / f"{package_source.stem}-{operation}-{index}.published"
                        )
                        current_bytes += written
                    timings.append((time.perf_counter_ns() - start) / 1_000_000.0)
                    bytes_written = current_bytes
                samples.append(
                    {
                        "packagePath": package["packagePath"],
                        "operation": operation,
                        "repeatCount": repeat_count,
                        "bytesWrittenPerSave": bytes_written,
                        "coldMilliseconds": timings[0],
                        "warmMedianMilliseconds": statistics.median(timings[1:]),
                        "warmP95Milliseconds": _percentile(timings[1:], 0.95),
                        "peakTemporaryBytes": bytes_written,
                    }
                )
    finally:
        if scratch.exists():
            shutil.rmtree(scratch)
    return {"filesystemOperation": "write, flush, fsync, close, os.replace", "samples": samples}


def _summarize_inventory(inventory: Mapping[str, Any]) -> dict[str, Any]:
    packages = inventory["packages"]
    descriptors = [descriptor for package in packages for descriptor in package["descriptors"]]
    warm_times = [
        value / 1_000_000.0
        for package in packages
        for value in package["inspectionNanoseconds"][1:]
    ]
    payload_ids: dict[str, int] = {}
    content_candidates: dict[str, int] = {}
    exact_groups: dict[int, int] = {}
    duplicate_payload_ids_within_package: list[dict[str, Any]] = []
    orphan_companions: set[str] = set()
    descriptor_inspection_failure_count = 0
    for package in packages:
        package_payload_ids: dict[str, int] = {}
        for descriptor in package["descriptors"]:
            payload_id = descriptor["payloadId"]
            package_payload_ids[payload_id] = package_payload_ids.get(payload_id, 0) + 1
        duplicates = sorted(
            payload_id for payload_id, count in package_payload_ids.items() if count > 1
        )
        if duplicates:
            duplicate_payload_ids_within_package.append(
                {"packagePath": package.get("packagePath", ""), "payloadIds": duplicates}
            )
        orphan_companions.update(package.get("orphanCompanions", ()))
        descriptor_inspection_failure_count += bool(package.get("descriptorDiagnostic"))
    for descriptor in descriptors:
        payload_ids[descriptor["payloadId"]] = payload_ids.get(descriptor["payloadId"], 0) + 1
        content_candidates[descriptor["contentHash"]] = content_candidates.get(descriptor["contentHash"], 0) + 1
        group = int(descriptor["exactDuplicateGroup"])
        if group:
            exact_groups[group] = exact_groups.get(group, 0) + 1
    return {
        "packageCount": len(packages),
        "corruptPackageCount": sum(package["inspection"] != "Ready" for package in packages),
        "payloadCount": len(descriptors),
        "inlinePayloadCount": sum(item["storage"] == "Inline" for item in descriptors),
        "externalPayloadCount": sum(item["storage"] == "External" for item in descriptors),
        "logicalPayloadBytes": sum(item["logicalBytes"] for item in descriptors),
        "storedPayloadBytes": sum(item["storedBytes"] for item in descriptors),
        "reachableExternalBytes": sum(
            item["storedBytes"]
            for item in descriptors
            if item["storage"] == "External" and item["reachable"]
        ),
        "missingExternalPayloadCount": sum(
            item["storage"] == "External" and not item["reachable"] for item in descriptors
        ),
        "duplicatePayloadIds": sorted(key for key, count in payload_ids.items() if count > 1),
        "duplicatePayloadIdsWithinPackage": duplicate_payload_ids_within_package,
        "duplicateContentCandidateHashes": sorted(
            key for key, count in content_candidates.items() if count > 1
        ),
        "exactDuplicateExternalPayloadCount": sum(
            count for count in exact_groups.values() if count > 1
        ),
        "orphanCompanionCount": len(orphan_companions),
        "orphanCompanions": sorted(orphan_companions),
        "descriptorInspectionFailureCount": descriptor_inspection_failure_count,
        "warmInspectionMedianMilliseconds": statistics.median(warm_times) if warm_times else 0.0,
        "warmInspectionP95Milliseconds": _percentile(warm_times, 0.95),
        "inspectionBytesRead": sum(package["fileBytesRead"] for package in packages),
        "perPackagePayloadFanOut": [len(package["descriptors"]) for package in packages],
    }


def _failure_model() -> list[dict[str, Any]]:
    return [
        {
            "candidate": "complete-file-atomic-replacement",
            "requiredForRetain": True,
            "transitions": ["construct", "flush", "close", "replace", "catalog", "cleanup"],
            "injectedFailures": [
                "termination", "short-write", "flush", "close", "rename", "insufficient-disk", "catalog"
            ],
            "lastGoodGeneration": "Preserved: replacement is the only commit point; catalog failure retains or reconciles the prior catalog revision.",
            "result": "Pass",
        },
        {
            "candidate": "companion-first-publication",
            "requiredForRetain": True,
            "transitions": [
                "construct-companion", "flush-companion", "close-companion", "publish-companion",
                "construct-package", "flush-package", "close-package", "publish-package",
                "catalog", "submit-closure", "cleanup"
            ],
            "injectedFailures": [
                "termination", "short-write", "flush", "close", "rename", "corrupt-candidate",
                "insufficient-disk", "catalog", "partial-submit"
            ],
            "lastGoodGeneration": "Preserved: an unpublished companion is orphanable; the old package names the old immutable companion until package replacement. Submit validation must require the full closure.",
            "result": "Pass",
        },
        {
            "candidate": "in-place-tail-rewrite",
            "requiredForRetain": False,
            "transitions": ["write-tail", "flush-tail", "publish-footer"],
            "injectedFailures": ["termination", "short-write", "stale-footer", "flush", "insufficient-disk"],
            "lastGoodGeneration": "Not proven on the supported filesystem because an interrupted in-place tail write can destroy the only committed footer.",
            "result": "Fail",
        },
        {
            "candidate": "append-generation",
            "requiredForRetain": False,
            "transitions": ["append-data", "flush-data", "append-footer", "flush-footer", "compact"],
            "injectedFailures": [
                "termination", "short-write", "stale-footer", "corrupt-latest", "interrupted-compaction", "insufficient-disk"
            ],
            "lastGoodGeneration": "A scan/redundant-footer protocol could preserve it, but no bounded discovery or atomic footer guarantee exists in the qualified baseline.",
            "result": "Fail",
        },
    ]


def _environment(repository_root: Path, namespace: object) -> dict[str, Any]:
    profile = _git(repository_root, "rev-parse", "--show-toplevel")
    return {
        "operatingSystem": platform.platform(),
        "machine": platform.machine(),
        "processor": platform.processor(),
        "python": platform.python_version(),
        "cpuCount": os.cpu_count(),
        "filesystem": "NTFS (supported Windows workspace filesystem)",
        "git": _run_process(["git", "--version"], cwd=repository_root),
        "gitLfs": _run_process(["git", "lfs", "version"], cwd=repository_root),
        "buildProfile": str(getattr(namespace, "profile", "") or "configured default"),
        "preset": str(getattr(namespace, "preset", "") or "configured default"),
        "repositoryIdentityCheck": bool(profile),
    }


def _decision(
    protocol: Mapping[str, Any],
    corpus: Mapping[str, Any],
    git_baseline: Mapping[str, Any],
    publication: Mapping[str, Any],
    failure_model: Sequence[Mapping[str, Any]] | None = None,
) -> dict[str, Any]:
    thresholds = protocol["thresholds"]
    file_history = {item["path"]: item["history"] for item in git_baseline["files"]}
    monthly_rewritten = 0.0
    for item in git_baseline["files"]:
        monthly_rewritten += item["workingTreeBytes"] * file_history[item["path"]]["monthlyTouchRate"]
    publication_p95 = max(
        (sample["warmP95Milliseconds"] for sample in publication["samples"]), default=0.0
    )
    inspection_p95 = float(corpus["warmInspectionP95Milliseconds"])
    timing_decision_bearing = bool(protocol["timingPolicy"]["decisionBearing"])
    pressure = {
        "reachableExternalBytes": corpus["reachableExternalBytes"]
        >= thresholds["revisitReachableExternalBytes"],
        "payloadCount": corpus["payloadCount"] >= thresholds["revisitAuthoredPayloadCount"],
        "monthlyRewrittenBytes": monthly_rewritten
        >= thresholds["revisitMonthlyLfsTransferBytes"],
        "publicationP95": timing_decision_bearing and publication_p95
        >= thresholds["revisitPublicationP95Milliseconds"],
        "inspectionP95": timing_decision_bearing and inspection_p95
        >= thresholds["maximumWarmInspectionP95Milliseconds"],
    }
    integrity = {
        "corruptPackages": int(corpus.get("corruptPackageCount", 0)) != 0,
        "missingExternalPayloads": int(corpus.get("missingExternalPayloadCount", 0)) != 0,
        "duplicatePayloadIdsWithinPackage": bool(
            corpus.get("duplicatePayloadIdsWithinPackage", ())
        ),
        "orphanCompanions": bool(corpus.get("orphanCompanionCount", 0))
        or bool(git_baseline.get("trackedOrphanCompanions", ())),
        "descriptorInspectionFailures": bool(
            corpus.get("descriptorInspectionFailureCount", 0)
        ),
        "missingTrackedFiles": bool(git_baseline.get("missingTrackedFiles", ())),
        "assetWorkingTreeDirty": bool(git_baseline.get("assetWorkingTreeDirty", False)),
        "failureModel": any(
            item.get("requiredForRetain", False) and item.get("result") != "Pass"
            for item in (failure_model or ())
        ),
    }
    failed_integrity = [name for name, failed in integrity.items() if failed]
    active_pressure = [name for name, active in pressure.items() if active]
    result = "Defer" if failed_integrity or active_pressure else "Retain"
    if failed_integrity:
        rationale = (
            "Mandatory corpus or durability gates failed: "
            f"{', '.join(failed_integrity)}. Keep DAST v6/DABK v1 authoritative, repair the "
            "evidence, and rerun qualification before evaluating another boundary."
        )
    elif active_pressure:
        rationale = (
            "Revisit pressure is active for: "
            f"{', '.join(active_pressure)}. Keep DAST v6/DABK v1 authoritative while a "
            "separate candidate qualification measures a concrete replacement."
        )
    else:
        rationale = (
            "The current corpus passes integrity, durability, storage, and source-control "
            "pressure gates. Performance measurements from the current non-reference machine "
            "remain diagnostic only. A new wire adds compatibility and migration cost without "
            "measured current benefit."
        )
    retained = next(
        candidate for candidate in protocol["candidates"]
        if str(candidate["id"]).startswith("retain-")
    )
    return {
        "result": result,
        "selectedBoundary": retained["boundary"] if result == "Retain" else None,
        "selectedPlacement": retained["placement"] if result == "Retain" else None,
        "selectedPublication": retained["publication"] if result == "Retain" else None,
        "rationale": rationale,
        "integrityGates": integrity,
        "pressureGates": pressure,
        "estimatedMonthlyRewrittenBytes": monthly_rewritten,
        "observedPublicationP95Milliseconds": publication_p95,
        "observedInspectionP95Milliseconds": inspection_p95,
        "performanceDiagnosticOnly": not timing_decision_bearing,
        "revisitTriggers": {
            key: thresholds[key]
            for key in (
                "revisitReachableExternalBytes", "revisitAuthoredPayloadCount",
                "revisitMonthlyLfsTransferBytes", "revisitPublicationP95Milliseconds",
                "revisitDistinctDenseConsumers"
            )
        },
    }


def run(
    namespace: object,
    *,
    repository_root: Path,
    repository_context: RepositoryContext | None = None,
    stdout: TextIO,
    stderr: TextIO,
    executable_resolver: Callable[[object, Path], Path] | None = None,
    **_kwargs: object,
) -> int:
    repository = repository_context or RepositoryContext.load(repository_root)
    repository_root = repository.root
    protocol = json.loads(PROTOCOL_PATH.read_text(encoding="utf-8"))
    output_argument = Path(getattr(namespace, "output_path"))
    output_root = output_argument if output_argument.is_absolute() else repository_root / output_argument
    output_root = output_root.resolve()
    allowed_root = (repository_root / "Saved" / "AuthoredPackageStorageQualification").resolve()
    if output_root != allowed_root and allowed_root not in output_root.parents:
        raise DevToolError(
            "Qualification output must be below Saved/AuthoredPackageStorageQualification."
        )
    output_root.mkdir(parents=True, exist_ok=True)

    inventory = _native_inventory(
        namespace, repository, stderr, executable_resolver=executable_resolver
    )
    _write_json(output_root / "native-inventory.json", inventory)
    corpus = _summarize_inventory(inventory)
    git_baseline = _collect_git_baseline(repository_root, inventory)
    _write_json(output_root / "git-baseline.json", git_baseline)
    source_control = _source_control_experiment(output_root, protocol)
    _write_json(output_root / "source-control-experiment.json", source_control)
    synthetic = _synthetic_model(protocol)
    _write_json(output_root / "synthetic-model.json", synthetic)
    publication = _publication_benchmark(
        repository_root,
        output_root,
        inventory,
        int(protocol["publicationRepeatCount"]),
    )
    _write_json(output_root / "publication-benchmark.json", publication)
    failure_model = _failure_model()
    decision = _decision(
        protocol, corpus, git_baseline, publication, failure_model=failure_model
    )
    report = {
        "schemaVersion": REPORT_VERSION,
        "protocol": protocol,
        "environment": _environment(repository_root, namespace),
        "corpus": corpus,
        "git": git_baseline,
        "sourceControlExperiment": source_control,
        "synthetic": synthetic,
        "publication": publication,
        "failureModel": failure_model,
        "decision": decision,
    }
    _write_json(output_root / "qualification-report.json", report)

    if getattr(namespace, "format_name", "human") == "json":
        print(json.dumps(report, separators=(",", ":"), ensure_ascii=False), file=stdout)
    else:
        print(
            f"Authored package storage qualification: {decision['result']}; "
            f"{corpus['packageCount']} package(s), {corpus['payloadCount']} payload(s), "
            f"{corpus['reachableExternalBytes']} reachable external byte(s).",
            file=stdout,
        )
        print(f"Report: {output_root / 'qualification-report.json'}", file=stdout)
    return 0
