#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterator

from ..repository import discover_repository_root


REPO_ROOT = discover_repository_root()
MANIFEST_DIR = Path(__file__).resolve().parent / "thirdparty"

PLATFORM_NAMES = {
    "win32": "Win64",
    "cygwin": "Win64",
    "darwin": "MacOS",
    "linux": "Linux",
}

VALID_CONFIGS = ("Debug", "Release")
SHA256_PATTERN = re.compile(r"^[0-9a-fA-F]{64}$")


class BootstrapError(RuntimeError):
    pass


@dataclass(frozen=True)
class DependencyRequest:
    use_all: bool = False
    libraries: str | None = None
    config: str = "All"
    with_tests: bool = False
    with_development: bool = False
    cmake_command: str | None = None


@contextmanager
def repository_paths(repository_root: Path) -> Iterator[None]:
    """Scope legacy path-based services to an explicit repository root."""
    global REPO_ROOT
    previous_root = REPO_ROOT
    REPO_ROOT = repository_root.resolve()
    try:
        yield
    finally:
        REPO_ROOT = previous_root


def configured_cmake_command() -> str:
    if environment_command := os.environ.get("CMAKE_COMMAND"):
        return environment_command
    config_path = REPO_ROOT / ".agents" / "build-config.json"
    try:
        value = json.loads(config_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return "cmake"
    configured = value.get("cmakeCommand") if isinstance(value, dict) else None
    return configured if isinstance(configured, str) and configured else "cmake"


def detect_platform_name() -> str:
    for prefix, platform_name in PLATFORM_NAMES.items():
        if sys.platform.startswith(prefix):
            return platform_name
    raise BootstrapError(f"Unsupported host platform: {sys.platform}")


def load_manifests() -> list[dict[str, Any]]:
    manifests: list[dict[str, Any]] = []
    for manifest_path in sorted(MANIFEST_DIR.glob("*.json")):
        manifests.append(json.loads(manifest_path.read_text(encoding="utf-8")))
    manifests.sort(key=lambda manifest: (manifest.get("order", 9999), manifest["name"]))
    return manifests


def normalize_rel_path(path_str: str) -> Path:
    return Path(path_str.replace("/", os.sep))


def resolve_repo_path(path_str: str) -> Path:
    return REPO_ROOT / normalize_rel_path(path_str)


def ensure_command_available(command: str) -> None:
    if shutil.which(command):
        return
    raise BootstrapError(f"Required command was not found in PATH: {command}")


def run_command(command: list[str], *, cwd: Path | None = None) -> None:
    print(f"[run] {' '.join(command)}")
    result = subprocess.run(command, cwd=str(cwd) if cwd else None)
    if result.returncode != 0:
        raise BootstrapError(f"Command failed with exit code {result.returncode}: {' '.join(command)}")


def verify_required_files(base_dir: Path, required_files: list[str]) -> bool:
    return all((base_dir / normalize_rel_path(file_name)).exists() for file_name in required_files)


def verify_any_required_file_set(base_dir: Path, required_sets: list[list[str]]) -> bool:
    return any(verify_required_files(base_dir, required_files) for required_files in required_sets)


def compute_sha256(file_path: Path) -> str:
    digest = hashlib.sha256()
    with file_path.open("rb") as file:
        for chunk in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def get_git_source_required_files(source: dict[str, Any]) -> list[str]:
    required_files = [source.get("marker", "CMakeLists.txt")]
    required_files.extend(source.get("required_files", []))
    return required_files


def update_git_submodules(source_dir: Path) -> None:
    run_command(["git", "submodule", "update", "--init", "--recursive"], cwd=source_dir)


def ensure_git_source(manifest: dict[str, Any]) -> None:
    source = manifest["source"]
    source_dir = resolve_repo_path(manifest["source_dir"])
    required_files = get_git_source_required_files(source)
    if verify_required_files(source_dir, required_files):
        print(f"{manifest['name']} source is already available at \"{source_dir}\".")
        return

    ensure_command_available("git")

    if source_dir.exists() and (source_dir / ".git").exists() and source.get("recursive_submodules", False):
        print(f"Repairing {manifest['name']} submodules in \"{source_dir}\"...")
        update_git_submodules(source_dir)
        if verify_required_files(source_dir, required_files):
            print(f"{manifest['name']} source is now complete at \"{source_dir}\".")
            return

    if source_dir.exists():
        raise BootstrapError(
            f"{manifest['name']} source directory exists but is incomplete: \"{source_dir}\".\n"
            "Please remove it and run bootstrap again, or repair it manually."
        )

    if commit := source.get("commit"):
        print(f"Cloning {manifest['name']} source commit {commit} into \"{source_dir}\"...")
        source_dir.parent.mkdir(parents=True, exist_ok=True)
        run_command(["git", "init", str(source_dir)])
        run_command(["git", "-C", str(source_dir), "remote", "add", "origin", source["url"]])
        run_command(["git", "-C", str(source_dir), "fetch", "--depth", "1", "origin", commit])
        run_command(["git", "-C", str(source_dir), "checkout", "--detach", "FETCH_HEAD"])
    else:
        print(f"Cloning {manifest['name']} source {source['tag']} into \"{source_dir}\"...")
        run_command(
            [
                "git",
                "clone",
                "--branch",
                source["tag"],
                "--depth",
                "1",
                *(["--recurse-submodules"] if source.get("recursive_submodules", False) else []),
                source["url"],
                str(source_dir),
            ]
        )

    if source.get("recursive_submodules", False):
        update_git_submodules(source_dir)

    if not verify_required_files(source_dir, required_files):
        raise BootstrapError(
            f"{manifest['name']} source clone completed, but expected files were not found in \"{source_dir}\"."
        )


def copy_extracted_contents(source_root: Path, destination_dir: Path) -> None:
    entries = list(source_root.iterdir())
    content_root = source_root
    if len(entries) == 1 and entries[0].is_dir():
        content_root = entries[0]

    destination_dir.mkdir(parents=True, exist_ok=True)
    for entry in content_root.iterdir():
        target = destination_dir / entry.name
        if entry.is_dir():
            shutil.copytree(entry, target, dirs_exist_ok=True)
        else:
            shutil.copy2(entry, target)


def ensure_archive_source(manifest: dict[str, Any], platform_name: str) -> None:
    source_dir = resolve_repo_path(manifest["source_dir"])
    platform_source = manifest["source"]["platforms"].get(platform_name)
    if not platform_source:
        raise BootstrapError(
            f"{manifest['name']} does not define a bootstrap archive for platform {platform_name}."
        )

    required_files = platform_source["required_files"]
    if verify_required_files(source_dir, required_files):
        print(f"{manifest['name']} package is already available at \"{source_dir}\".")
        return

    if source_dir.exists():
        raise BootstrapError(
            f"{manifest['name']} directory exists but is incomplete: \"{source_dir}\".\n"
            "Please remove it and run bootstrap again, or repair it manually."
        )

    archive_url = platform_source["url"]
    archive_name = platform_source["archive_name"]
    print(f"Downloading {manifest['name']} package from {archive_url}...")

    with tempfile.TemporaryDirectory(prefix=f"durin-{manifest['name']}-") as temp_dir_str:
        temp_dir = Path(temp_dir_str)
        archive_path = temp_dir / archive_name
        extract_dir = temp_dir / "extract"
        extract_dir.mkdir(parents=True, exist_ok=True)

        urllib.request.urlretrieve(archive_url, archive_path)

        if expected_sha256 := platform_source.get("sha256"):
            actual_sha256 = compute_sha256(archive_path)
            if actual_sha256.lower() != expected_sha256.lower():
                raise BootstrapError(
                    f"{manifest['name']} archive integrity verification failed for \"{archive_path}\".\n"
                    f"Expected SHA-256: {expected_sha256.lower()}\n"
                    f"Actual SHA-256:   {actual_sha256}"
                )

        if archive_name.endswith(".zip"):
            with zipfile.ZipFile(archive_path) as archive:
                archive.extractall(extract_dir)
        elif archive_name.endswith(".tar.gz") or archive_name.endswith(".tgz"):
            with tarfile.open(archive_path, "r:gz") as archive:
                archive.extractall(extract_dir)
        else:
            raise BootstrapError(f"Unsupported archive format for {archive_name}")

        copy_extracted_contents(extract_dir, source_dir)

    if not verify_required_files(source_dir, required_files):
        raise BootstrapError(
            f"{manifest['name']} package was extracted, but expected files were not found in \"{source_dir}\"."
        )


def ensure_source_prepared(manifest: dict[str, Any], platform_name: str) -> None:
    source_kind = manifest["source"]["type"]
    if source_kind == "git":
        ensure_git_source(manifest)
    elif source_kind == "archive":
        ensure_archive_source(manifest, platform_name)
    else:
        raise BootstrapError(f"Unsupported source type for {manifest['name']}: {source_kind}")


def get_build_dir(manifest: dict[str, Any], platform_name: str, config: str) -> Path:
    return REPO_ROOT / "Build" / "ThirdParty" / f"{platform_name}-{config}-{manifest['name']}"


def get_install_dir(manifest: dict[str, Any], platform_name: str, config: str) -> Path:
    return REPO_ROOT / "Engine" / "External" / "Install" / platform_name / config / manifest["name"]


def install_shared_library(manifest: dict[str, Any], platform_name: str, config: str, cmake_command: str) -> None:
    install_dir = get_install_dir(manifest, platform_name, config)
    required_sets = manifest["install_required_file_sets"][config]
    if verify_any_required_file_set(install_dir, required_sets):
        print(f"{manifest['name']} {config} is already installed at \"{install_dir}\".")
        return

    cmake_dir = resolve_repo_path(manifest["cmake_dir"])
    build_dir = get_build_dir(manifest, platform_name, config)
    build_dir.parent.mkdir(parents=True, exist_ok=True)
    install_dir.parent.mkdir(parents=True, exist_ok=True)

    print(f"Installing {manifest['name']} {config}...")
    run_command(
        [
            cmake_command,
            "-S",
            str(cmake_dir),
            "-B",
            str(build_dir),
            "-D",
            f"CMAKE_BUILD_TYPE={config}",
            "-D",
            f"CMAKE_INSTALL_PREFIX={install_dir}",
        ]
    )
    run_command([cmake_command, "--build", str(build_dir), "--config", config, "--target", "install"])

    if not verify_any_required_file_set(install_dir, required_sets):
        raise BootstrapError(
            f"{manifest['name']} {config} install completed, but expected files were not found in \"{install_dir}\"."
        )


def process_manifest(
    manifest: dict[str, Any],
    *,
    platform_name: str,
    configs: list[str],
    cmake_command: str,
) -> None:
    print(f"==> Preparing {manifest['name']} ({manifest['kind']})")
    if (
        manifest["source"]["type"] == "archive"
        and platform_name not in manifest["source"]["platforms"]
        and manifest.get("allow_unsupported_platform", False)
    ):
        print(f"Skipping {manifest['name']}: no package is available for platform {platform_name}.")
        return

    ensure_source_prepared(manifest, platform_name)

    if manifest["kind"] == "direct_source":
        required_files = manifest.get("required_files", [])
        if required_files and not verify_required_files(resolve_repo_path(manifest["source_dir"]), required_files):
            raise BootstrapError(f"{manifest['name']} source is missing required files after preparation.")
        return

    if manifest["kind"] == "prebuilt_sdk":
        platform_required = manifest["required_files_by_platform"].get(platform_name)
        if not platform_required:
            raise BootstrapError(
                f"{manifest['name']} does not define required files for platform {platform_name}."
            )
        if not verify_required_files(resolve_repo_path(manifest["source_dir"]), platform_required):
            raise BootstrapError(f"{manifest['name']} package is missing required files after preparation.")
        return

    if manifest["kind"] == "tool_package":
        platform_required = manifest["source"]["platforms"][platform_name]["required_files"]
        if not verify_required_files(resolve_repo_path(manifest["source_dir"]), platform_required):
            raise BootstrapError(f"{manifest['name']} package is missing required files after preparation.")
        return

    if manifest["kind"] == "shared_install":
        for config in configs:
            install_shared_library(manifest, platform_name, config, cmake_command)
        return

    raise BootstrapError(f"Unsupported manifest kind for {manifest['name']}: {manifest['kind']}")


def resolve_selected_manifests(
    manifests: list[dict[str, Any]],
    *,
    use_all: bool,
    libs_arg: str | None,
    with_tests: bool,
    with_development: bool,
) -> list[dict[str, Any]]:
    manifests_by_name = {manifest["name"]: manifest for manifest in manifests}
    if use_all:
        return [
            manifest
            for manifest in manifests
            if (with_tests or not manifest.get("test_only", False))
            and (with_development or not manifest.get("development_only", False))
        ]

    if libs_arg:
        names = [name.strip() for name in libs_arg.split(",") if name.strip()]
        unknown = [name for name in names if name not in manifests_by_name]
        if unknown:
            raise BootstrapError(f"Unknown third-party libraries requested: {', '.join(unknown)}")
        return [manifests_by_name[name] for name in names]

    raise BootstrapError("Specify either --all or --libs.")


def normalize_configs(config_value: str) -> list[str]:
    if config_value == "All":
        return list(VALID_CONFIGS)
    return [config_value]


def query_manifest_status(manifest: dict[str, Any], platform_name: str) -> dict[str, Any]:
    source_dir = resolve_repo_path(manifest["source_dir"])
    source = manifest["source"]
    platform_source = source.get("platforms", {}).get(platform_name) if source["type"] == "archive" else None
    platform_supported = source["type"] != "archive" or platform_source is not None
    if source["type"] == "git":
        required_files = get_git_source_required_files(source)
    elif platform_source:
        required_files = platform_source["required_files"]
    else:
        required_files = []
    missing_files = [
        file_name
        for file_name in required_files
        if not (source_dir / normalize_rel_path(file_name)).exists()
    ]
    return {
        "name": manifest["name"],
        "version": manifest.get("version"),
        "platform": platform_name,
        "platform_supported": platform_supported,
        "source_dir": str(source_dir),
        "required_files": required_files,
        "missing_files": missing_files,
        "prepared": platform_supported and not missing_files,
        "repair_command": manifest.get("repair_command"),
    }


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
                raise BootstrapError(
                    f"Manifest {manifest['name']} field {field_name} must be a boolean."
                )
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
                required_fields = {"url", "archive_name", "required_files"}
                missing_platform_fields = required_fields.difference(platform_source)
                if missing_platform_fields:
                    raise BootstrapError(
                        f"Archive manifest {manifest['name']} platform {platform_name} is missing required keys: "
                        f"{sorted(missing_platform_fields)}"
                    )
                if "sha256" in platform_source and (
                    not isinstance(platform_source["sha256"], str)
                    or not SHA256_PATTERN.fullmatch(platform_source["sha256"])
                ):
                    raise BootstrapError(
                        f"Archive manifest {manifest['name']} platform {platform_name} sha256 "
                        "must contain exactly 64 hexadecimal digits."
                    )
        if manifest["kind"] == "shared_install":
            if "cmake_dir" not in manifest or "install_required_file_sets" not in manifest:
                raise BootstrapError(
                    f"Shared-install manifest {manifest['name']} must define cmake_dir and install_required_file_sets."
                )
        if manifest["kind"] == "prebuilt_sdk" and "required_files_by_platform" not in manifest:
            raise BootstrapError(f"Prebuilt SDK manifest {manifest['name']} must define required_files_by_platform.")
        if manifest["kind"] == "tool_package" and source_kind != "archive":
            raise BootstrapError(f"Tool package manifest {manifest['name']} must use an archive source.")


def validate_repository_manifests(repository_root: Path) -> int:
    with repository_paths(repository_root):
        manifests = load_manifests()
        validate_manifests(manifests)
    print(f"Validated {len(manifests)} third-party manifests successfully.")
    return len(manifests)


def prepare_dependencies(
    repository_root: Path,
    request: DependencyRequest,
) -> tuple[str, ...]:
    if request.use_all == bool(request.libraries):
        raise BootstrapError("Specify exactly one of --all or --libs.")
    with repository_paths(repository_root):
        manifests = load_manifests()
        validate_manifests(manifests)
        platform_name = detect_platform_name()
        selected_manifests = resolve_selected_manifests(
            manifests,
            use_all=request.use_all,
            libs_arg=request.libraries,
            with_tests=request.with_tests,
            with_development=request.with_development,
        )
        for manifest in selected_manifests:
            process_manifest(
                manifest,
                platform_name=platform_name,
                configs=normalize_configs(request.config),
                cmake_command=request.cmake_command or configured_cmake_command(),
            )
    return tuple(manifest["name"] for manifest in selected_manifests)
