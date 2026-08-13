"""Third-party manifest loading, selection, validation, and planning."""

from __future__ import annotations

import json
import re
from pathlib import Path
from typing import Any

from ..context import RepositoryContext
from .models import BootstrapError, DependencyRequest, PlannedDependency
from . import sources

SHA256_PATTERN = re.compile(r"^[0-9a-fA-F]{64}$")
VALID_CONFIGS = ("Debug", "Release")


def load_manifests(repository: RepositoryContext) -> list[dict[str, Any]]:
    manifests: list[dict[str, Any]] = []
    directory = repository.resolve(repository.config.paths.third_party_manifests)
    for path in sorted(directory.glob("*.json")):
        manifests.append(json.loads(path.read_text(encoding="utf-8")))
    manifests.sort(key=lambda manifest: (manifest.get("order", 9999), manifest["name"]))
    return manifests


def validate_manifests(manifests: list[dict[str, Any]]) -> None:
    required_common = {"name", "kind", "source", "source_dir"}
    valid_kinds = {"prebuilt_sdk", "direct_source", "shared_install", "tool_package"}
    for manifest in manifests:
        missing = required_common.difference(manifest)
        if missing:
            raise BootstrapError(
                f"Manifest {manifest.get('name', '<unknown>')} is missing required keys: {sorted(missing)}"
            )
        if manifest["kind"] not in valid_kinds:
            raise BootstrapError(f"Manifest {manifest['name']} has unsupported kind: {manifest['kind']}")
        for field_name in ("test_only", "development_only", "allow_unsupported_platform"):
            if field_name in manifest and not isinstance(manifest[field_name], bool):
                raise BootstrapError(f"Manifest {manifest['name']} field {field_name} must be a boolean.")
        source_kind = manifest["source"].get("type")
        if source_kind not in {"git", "archive"}:
            raise BootstrapError(f"Manifest {manifest['name']} has unsupported source type: {source_kind}")
        if source_kind == "git":
            revisions = [key for key in ("tag", "commit") if manifest["source"].get(key)]
            if len(revisions) != 1:
                raise BootstrapError(
                    f"Git manifest {manifest['name']} must define exactly one source revision: tag or commit."
                )
        if source_kind == "archive":
            platforms = manifest["source"].get("platforms")
            if not isinstance(platforms, dict) or not platforms:
                raise BootstrapError(f"Archive manifest {manifest['name']} must define source platforms.")
            for platform_name, platform_source in platforms.items():
                if not isinstance(platform_source, dict):
                    raise BootstrapError(
                        f"Archive manifest {manifest['name']} platform {platform_name} must be an object."
                    )
                missing_fields = {"url", "archive_name", "required_files"}.difference(platform_source)
                if missing_fields:
                    raise BootstrapError(
                        f"Archive manifest {manifest['name']} platform {platform_name} is missing required keys: {sorted(missing_fields)}"
                    )
                checksum = platform_source.get("sha256")
                if checksum is not None and (
                    not isinstance(checksum, str) or not SHA256_PATTERN.fullmatch(checksum)
                ):
                    raise BootstrapError(
                        f"Archive manifest {manifest['name']} platform {platform_name} sha256 must contain exactly 64 hexadecimal digits."
                    )
        if manifest["kind"] == "shared_install" and (
            "cmake_dir" not in manifest or "install_required_file_sets" not in manifest
        ):
            raise BootstrapError(
                f"Shared-install manifest {manifest['name']} must define cmake_dir and install_required_file_sets."
            )
        if manifest["kind"] == "prebuilt_sdk" and "required_files_by_platform" not in manifest:
            raise BootstrapError(f"Prebuilt SDK manifest {manifest['name']} must define required_files_by_platform.")
        if manifest["kind"] == "tool_package" and source_kind != "archive":
            raise BootstrapError(f"Tool package manifest {manifest['name']} must use an archive source.")


def select_manifests(
    manifests: list[dict[str, Any]],
    request: DependencyRequest,
) -> list[dict[str, Any]]:
    if request.use_all == bool(request.libraries):
        raise BootstrapError("Specify exactly one of --all or --libs.")
    if request.use_all:
        return [
            manifest
            for manifest in manifests
            if (request.with_tests or not manifest.get("test_only", False))
            and (request.with_development or not manifest.get("development_only", False))
        ]
    names = [name.strip() for name in (request.libraries or "").split(",") if name.strip()]
    by_name = {manifest["name"]: manifest for manifest in manifests}
    unknown = [name for name in names if name not in by_name]
    if unknown:
        raise BootstrapError(f"Unknown third-party libraries requested: {', '.join(unknown)}")
    return [by_name[name] for name in names]


def configurations(value: str) -> tuple[str, ...]:
    return VALID_CONFIGS if value == "All" else (value,)


def plan_dependencies(
    manifests: list[dict[str, Any]],
    request: DependencyRequest,
) -> tuple[PlannedDependency, ...]:
    selected = select_manifests(manifests, request)
    configs = configurations(request.config)
    return tuple(
        PlannedDependency(
            manifest["name"],
            manifest["kind"],
            configs if manifest["kind"] == "shared_install" else (),
        )
        for manifest in selected
    )


def query_manifest_status(
    manifest: dict[str, Any],
    platform_name: str,
    repository: RepositoryContext,
) -> dict[str, Any]:
    source_dir = sources.resolve_repo_path(manifest["source_dir"], repository)
    source = manifest["source"]
    platform_source = source.get("platforms", {}).get(platform_name) if source["type"] == "archive" else None
    platform_supported = source["type"] != "archive" or platform_source is not None
    required = (
        sources.git_required_files(source)
        if source["type"] == "git"
        else platform_source["required_files"] if platform_source else []
    )
    missing = [name for name in required if not (source_dir / sources.normalize_rel_path(name)).exists()]
    return {
        "name": manifest["name"],
        "version": manifest.get("version"),
        "platform": platform_name,
        "platform_supported": platform_supported,
        "source_dir": str(source_dir),
        "required_files": required,
        "missing_files": missing,
        "prepared": platform_supported and not missing,
        "repair_command": manifest.get("repair_command"),
    }
