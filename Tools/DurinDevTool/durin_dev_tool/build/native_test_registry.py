"""Configured native-test registry loading and bounded selection."""

from __future__ import annotations

import json
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

from .config import BuildContext, BuildToolError, REPO_ROOT, preset_build_directory


SCHEMA_VERSION = 1
REGISTRY_FILE_NAME = "DurinNativeTestRegistry.json"
SELECTOR_DIMENSIONS = {
    "kind": "kind",
    "domain": "domains",
    "module": "modules",
    "backend": "backends",
    "stack": "stacks",
}
VALUE_PATTERN = re.compile(r"^[a-z][a-z0-9]*(?:-[a-z0-9]+)*$")


@dataclass(frozen=True)
class NativeTestTarget:
    name: str
    metadata_mode: str
    kind: str
    domains: tuple[str, ...]
    modules: tuple[str, ...]
    backends: tuple[str, ...]
    stacks: tuple[str, ...]
    direct_lifecycle: bool
    timeout_seconds: int
    resource_locks: tuple[str, ...]
    heavy_runtime: bool
    private_source_owner: str
    private_source_rationale: str

    @property
    def characterization(self) -> bool:
        return self.kind == "characterization"


@dataclass(frozen=True)
class NativeTestRegistry:
    path: Path
    preset: str
    targets: tuple[NativeTestTarget, ...]

    def target(self, name: str) -> NativeTestTarget | None:
        return next((target for target in self.targets if target.name == name), None)


@dataclass(frozen=True)
class ResolvedSelection:
    expression: str
    targets: tuple[NativeTestTarget, ...]
    explanation: str

    @property
    def names(self) -> tuple[str, ...]:
        return tuple(target.name for target in self.targets)


def registry_path(context: BuildContext) -> Path:
    return preset_build_directory(context.preset) / REGISTRY_FILE_NAME


def _string_list(record: dict[str, object], key: str, *, target: str) -> tuple[str, ...]:
    value = record.get(key)
    if not isinstance(value, list) or not all(isinstance(item, str) for item in value):
        raise BuildToolError(
            f'Native-test registry target "{target}" field "{key}" must be a string array.'
        )
    return tuple(value)


def load_native_test_registry(context: BuildContext) -> NativeTestRegistry:
    path = registry_path(context)
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as error:
        raise BuildToolError(
            f'Configured native-test registry was not found: "{path}".',
            recovery=f"Run .\\DevTool.bat configure --preset {context.preset.name}.",
        ) from error
    except (OSError, json.JSONDecodeError) as error:
        raise BuildToolError(
            f'Configured native-test registry is unreadable: "{path}": {error}',
            recovery=f"Run .\\DevTool.bat configure --fresh --preset {context.preset.name}.",
        ) from error
    if not isinstance(document, dict) or document.get("schemaVersion") != SCHEMA_VERSION:
        raise BuildToolError(
            f'Configured native-test registry "{path}" has an unsupported schema.',
            recovery=f"Run .\\DevTool.bat configure --fresh --preset {context.preset.name}.",
        )
    identity = document.get("identity")
    expected_binary = str(preset_build_directory(context.preset).resolve()).casefold()
    if not isinstance(identity, dict) or (
        str(Path(str(identity.get("sourceDir", ""))).resolve()).casefold()
        != str(REPO_ROOT.resolve()).casefold()
        or str(Path(str(identity.get("binaryDir", ""))).resolve()).casefold()
        != expected_binary
        or str(identity.get("preset", "")) != context.preset.name
    ):
        raise BuildToolError(
            f'Configured native-test registry "{path}" does not match the active checkout and preset.',
            recovery=f"Run .\\DevTool.bat configure --fresh --preset {context.preset.name}.",
        )
    records = document.get("targets")
    if not isinstance(records, list):
        raise BuildToolError(f'Configured native-test registry "{path}" has no target array.')
    targets: list[NativeTestTarget] = []
    for record in records:
        if not isinstance(record, dict) or not isinstance(record.get("name"), str):
            raise BuildToolError(f'Configured native-test registry "{path}" has an invalid target record.')
        name = str(record["name"])
        targets.append(
            NativeTestTarget(
                name=name,
                metadata_mode=str(record.get("metadataMode", "")),
                kind=str(record.get("kind", "")),
                domains=_string_list(record, "domains", target=name),
                modules=_string_list(record, "modules", target=name),
                backends=_string_list(record, "backends", target=name),
                stacks=_string_list(record, "stacks", target=name),
                direct_lifecycle=record.get("directLifecycle") is True,
                timeout_seconds=int(record.get("timeoutSeconds", 0)),
                resource_locks=_string_list(record, "resourceLocks", target=name),
                heavy_runtime=record.get("heavyRuntime") is True,
                private_source_owner=str(record.get("privateSourceOwner", "")),
                private_source_rationale=str(record.get("privateSourceRationale", "")),
            )
        )
    names = [target.name for target in targets]
    if names != sorted(names) or len(names) != len(set(names)):
        raise BuildToolError(f'Configured native-test registry "{path}" is not deterministic.')
    return NativeTestRegistry(path, context.preset.name, tuple(targets))


def _parse_selector(expression: str) -> dict[str, set[str]]:
    body = expression.removeprefix("@")
    if not body:
        raise BuildToolError("Native-test set selector '@' is empty.")
    predicates: dict[str, set[str]] = {}
    for term in body.split(","):
        dimension, separator, values_text = term.partition("=")
        if not separator:
            dimension, values_text = "domain", dimension
        if dimension not in SELECTOR_DIMENSIONS:
            expected = ", ".join(SELECTOR_DIMENSIONS)
            raise BuildToolError(
                f'Unknown native-test selector dimension "{dimension}"; expected {expected}.'
            )
        values = values_text.split("+")
        if not values or any(not VALUE_PATTERN.fullmatch(value) for value in values):
            raise BuildToolError(
                f'Invalid native-test selector term "{term}"; values must be lowercase slugs.'
            )
        if len(values) != len(set(values)):
            raise BuildToolError(f'Native-test selector term "{term}" contains duplicates.')
        predicates.setdefault(dimension, set()).update(values)
    return predicates


def resolve_selection(
    registry: NativeTestRegistry,
    expression: str,
    *,
    admit_characterization: bool = False,
) -> ResolvedSelection:
    exact = registry.target(expression)
    if exact is not None:
        if exact.characterization and not admit_characterization:
            raise BuildToolError(
                f'Native-test target "{expression}" is characterization-only.',
                recovery=f"Rerun test {expression} --mode characterization.",
            )
        return ResolvedSelection(expression, (exact,), "exact target name")
    if not expression.startswith("@"):
        raise BuildToolError(
            f'Unknown native-test target "{expression}".',
            recovery="Run .\\DevTool.bat test list to inspect configured targets and sets.",
        )
    predicates = _parse_selector(expression)
    matches: list[NativeTestTarget] = []
    for target in registry.targets:
        if target.characterization and not admit_characterization:
            continue
        if all(
            bool(set(getattr(target, SELECTOR_DIMENSIONS[dimension])) & values)
            if SELECTOR_DIMENSIONS[dimension] != "kind"
            else target.kind in values
            for dimension, values in predicates.items()
        ):
            matches.append(target)
    if not matches:
        raise BuildToolError(
            f'Native-test selector "{expression}" matched no configured targets.',
            recovery="Run .\\DevTool.bat test list to inspect configured metadata.",
        )
    explanation = ", ".join(
        f"{dimension}={' + '.join(sorted(values))}"
        for dimension, values in predicates.items()
    )
    return ResolvedSelection(expression, tuple(matches), explanation)


def filter_targets(registry: NativeTestRegistry, query: str = "") -> tuple[NativeTestTarget, ...]:
    normalized = query.casefold()
    if not normalized:
        return registry.targets
    if normalized == "migration":
        return tuple(
            target
            for target in registry.targets
            if target.metadata_mode == "legacy" or target.private_source_owner
        )
    return tuple(
        target
        for target in registry.targets
        if normalized
        in " ".join(
            (
                target.name,
                target.metadata_mode,
                target.kind,
                *target.domains,
                *target.modules,
                *target.backends,
                *target.stacks,
                target.private_source_owner,
                target.private_source_rationale,
            )
        ).casefold()
    )


def target_metadata_text(target: NativeTestTarget) -> str:
    values: Iterable[str] = (
        f"kind={target.kind}" if target.kind else "legacy",
        f"domains={'+'.join(target.domains)}" if target.domains else "",
        f"modules={'+'.join(target.modules)}" if target.modules else "",
        f"backends={'+'.join(target.backends)}" if target.backends else "",
        f"stacks={'+'.join(target.stacks)}" if target.stacks else "",
        f"locks={'+'.join(target.resource_locks)}" if target.resource_locks else "",
        "heavy" if target.heavy_runtime else "",
        (
            f"private-source-owner={target.private_source_owner}"
            if target.private_source_owner
            else ""
        ),
    )
    return ", ".join(value for value in values if value)
