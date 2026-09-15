"""Conservative Git-change to native-test selection analysis."""

from __future__ import annotations

import os
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

from .errors import BuildToolError
from .native_test_registry import NativeTestRegistry, NativeTestTarget


_NON_ALNUM = re.compile(r"[^a-z0-9]")
_DOCUMENTATION_PREFIXES = ("documentation/",)
_NATIVE_TEST_PREFIX = "engine/tests/native/"
_NATIVE_TEST_INFRASTRUCTURE_PREFIXES = (
    "engine/tests/nativetestsupport/",
    "tools/durindevtool/durin_dev_tool/build/native_test_",
)
_NATIVE_TEST_INFRASTRUCTURE_FILES = {
    "cmake/project/projecttargets.cmake",
    "cmakelists.txt",
    "engine/tests/native/cmakelists.txt",
    "tools/durindevtool/durin_dev_tool/build/core.py",
    "tools/durindevtool/durin_dev_tool/build/handler.py",
    "tools/durindevtool/durin_dev_tool/build/request_validation.py",
    "tools/durindevtool/durin_dev_tool/build/requests.py",
    "tools/durindevtool/durin_dev_tool/build/runtime.py",
    "tools/durindevtool/durin_dev_tool/commands/build_specs.py",
}


@dataclass(frozen=True)
class AffectedTestSelection:
    changed_paths: tuple[str, ...]
    modules: tuple[str, ...]
    domains: tuple[str, ...]
    targets: tuple[NativeTestTarget, ...]
    run_all: bool
    reasons: tuple[str, ...]
    projects: tuple[str, ...] = ()

    @property
    def names(self) -> tuple[str, ...]:
        return tuple(target.name for target in self.targets)


def _run_git(root: Path, arguments: list[str], operation: str) -> str:
    safe_root = str(root.resolve()).replace(os.sep, "/")
    command = [
        "git.exe" if os.name == "nt" else "git",
        "-c",
        f"safe.directory={safe_root}",
        "-c",
        "core.quotePath=false",
        "-C",
        str(root),
        *arguments,
    ]
    try:
        result = subprocess.run(command, check=False, capture_output=True)
    except OSError as error:
        raise BuildToolError(f"Could not run Git while {operation}: {error}") from error
    if result.returncode != 0:
        detail = (result.stderr or result.stdout).decode("utf-8", errors="replace").strip()
        suffix = f": {detail}" if detail else ""
        raise BuildToolError(f"Git failed while {operation}{suffix}")
    return result.stdout.decode("utf-8", errors="surrogateescape")


def _nul_paths(output: str) -> set[str]:
    return {
        path.replace("\\", "/")
        for path in output.split("\0")
        if path
    }


def discover_changed_paths(root: Path, base: str = "") -> tuple[str, ...]:
    """Return tracked and untracked paths changed in this checkout."""
    changed: set[str] = set()
    if base:
        changed.update(
            _nul_paths(
                _run_git(
                    root,
                    ["diff", "--name-only", "-z", "--diff-filter=ACDMRTUXB", base, "--"],
                    f'comparing native-test impact with base "{base}"',
                )
            )
        )
    else:
        changed.update(
            _nul_paths(
                _run_git(
                    root,
                    ["diff", "--name-only", "-z", "--diff-filter=ACDMRTUXB", "--"],
                    "reading unstaged native-test impact",
                )
            )
        )
        changed.update(
            _nul_paths(
                _run_git(
                    root,
                    ["diff", "--cached", "--name-only", "-z", "--diff-filter=ACDMRTUXB", "--"],
                    "reading staged native-test impact",
                )
            )
        )
    changed.update(
        _nul_paths(
            _run_git(
                root,
                ["ls-files", "--others", "--exclude-standard", "-z", "--"],
                "reading untracked native-test impact",
            )
        )
    )
    return tuple(sorted(changed, key=str.casefold))


def _key(value: str) -> str:
    return _NON_ALNUM.sub("", value.casefold())


def _source_module(path: str, registry_modules: dict[str, str]) -> str:
    parts = PurePosixPath(path).parts
    folded_parts = tuple(part.casefold() for part in parts)
    try:
        source_index = folded_parts.index("source")
    except ValueError:
        return ""
    for part in reversed(parts[source_index + 1 :]):
        module = registry_modules.get(_key(part))
        if module:
            return module
    return ""


def analyze_affected_tests(
    registry: NativeTestRegistry,
    changed_paths: tuple[str, ...],
) -> AffectedTestSelection:
    ordinary_targets = tuple(
        target
        for target in registry.targets
        if not target.characterization and not target.qualification
    )
    registry_modules = {
        _key(module): module
        for target in ordinary_targets
        for module in target.modules
    }
    source_owners: dict[str, set[str]] = {}
    for target in registry.targets:
        for source in target.sources:
            source_owners.setdefault(source.casefold(), set()).add(target.name)
    ordinary_names = {target.name for target in ordinary_targets}
    opt_in_sources = False
    modules: set[str] = set()
    direct_target_names: set[str] = set()
    all_reasons: set[str] = set()
    projects: set[str] = set()

    for original_path in changed_paths:
        path = original_path.replace("\\", "/").casefold()
        if path.startswith(_DOCUMENTATION_PREFIXES) or PurePosixPath(path).name == "agents.md":
            continue
        if path == "durin.dworkspace":
            all_reasons.add("workspace project membership changed")
            continue
        if path.startswith(_NATIVE_TEST_INFRASTRUCTURE_PREFIXES) or path in _NATIVE_TEST_INFRASTRUCTURE_FILES:
            all_reasons.add("shared native-test discovery or execution infrastructure changed")
            continue
        owner = next((project for project in registry.projects
                      if project.test_root and path.startswith(project.test_root.casefold().rstrip("/") + "/")), None)
        matched_sources = source_owners.get(path, set())
        direct_target_names.update(matched_sources & ordinary_names)
        if owner or path.startswith(_NATIVE_TEST_PREFIX):
            if matched_sources:
                opt_in_sources |= not bool(matched_sources & ordinary_names)
            elif owner:
                # Unknown/new/deleted sources, headers and data remain project-wide.
                projects.add(owner.name)
            else:
                all_reasons.add("native-test source ownership is unknown")
            continue
        # A production-private source can be compiled into tests as well as its
        # owning module. Retain both exact test consumers and module coverage.
        if any(path == project.descriptor.casefold() for project in registry.projects):
            all_reasons.add("project module or test declaration changed")
            continue
        module = _source_module(path, registry_modules)
        if module:
            modules.add(module)
            continue
        if path.startswith("cmake/") or path.endswith("/cmakelists.txt"):
            all_reasons.add("unbounded CMake build graph changed")
            continue
        if path.startswith("engine/source/") or path.startswith("engine/content/"):
            all_reasons.add("runtime input outside a registered module changed")

    selected = tuple(
        target
        for target in ordinary_targets
        if target.name in direct_target_names
        or set(target.modules) & modules
        or target.project in projects
    )
    reasons: list[str] = []
    if projects:
        reasons.append(f"changed native-test projects: {', '.join(sorted(projects))}")
    if modules:
        reasons.append(f"changed modules: {', '.join(sorted(modules))}")
    if direct_target_names:
        reasons.append(f"changed native-test targets: {', '.join(sorted(direct_target_names))}")
    reasons.extend(sorted(all_reasons))
    if opt_in_sources:
        reasons.append("changed test sources include opt-in coverage; qualification and characterization remain excluded")
    if not reasons:
        reasons.append("no changed path requires native-test coverage")

    return AffectedTestSelection(
        changed_paths=changed_paths,
        modules=tuple(sorted(modules)),
        domains=(),
        targets=() if all_reasons else selected,
        run_all=bool(all_reasons),
        reasons=tuple(reasons),
        projects=tuple(sorted(projects)),
    )
