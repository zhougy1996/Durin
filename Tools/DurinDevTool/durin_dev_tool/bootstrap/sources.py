"""Third-party Git/archive source acquisition."""

from __future__ import annotations

import hashlib
import os
import shutil
import subprocess
import tarfile
import tempfile
import urllib.request
import zipfile
from pathlib import Path
from typing import Any, Mapping, Sequence

from ..context import CommandIO, RepositoryContext
from ..toolchain import find_command
from .models import BootstrapError


def normalize_rel_path(value: str) -> Path:
    return Path(value.replace("/", os.sep))


def resolve_repo_path(value: str, repository: RepositoryContext) -> Path:
    return repository.root / normalize_rel_path(value)


def run_command(
    command: Sequence[str],
    *,
    command_io: CommandIO,
    cwd: Path | None = None,
    environment: Mapping[str, str] | None = None,
) -> None:
    command_io.out(f"[run] {' '.join(command)}")
    result = subprocess.run(
        list(command),
        cwd=str(cwd) if cwd else None,
        env=dict(environment) if environment is not None else None,
        stdout=command_io.stdout,
        stderr=command_io.stderr,
    )
    if result.returncode != 0:
        raise BootstrapError(f"Command failed with exit code {result.returncode}: {' '.join(command)}")


def verify_required_files(base: Path, required: Sequence[str]) -> bool:
    return all((base / normalize_rel_path(name)).exists() for name in required)


def verify_any_required_file_set(base: Path, required_sets: Sequence[Sequence[str]]) -> bool:
    return any(verify_required_files(base, required) for required in required_sets)


def compute_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git_required_files(source: dict[str, Any]) -> list[str]:
    return [source.get("marker", "CMakeLists.txt"), *source.get("required_files", [])]


def _update_submodules(source_dir: Path, command_io: CommandIO) -> None:
    run_command(["git", "submodule", "update", "--init", "--recursive"], cwd=source_dir, command_io=command_io)


def ensure_git_source(manifest: dict[str, Any], repository: RepositoryContext, command_io: CommandIO) -> None:
    source = manifest["source"]
    source_dir = resolve_repo_path(manifest["source_dir"], repository)
    required = git_required_files(source)
    if verify_required_files(source_dir, required):
        command_io.out(f"{manifest['name']} source is already available at \"{source_dir}\".")
        return
    if not find_command("git"):
        raise BootstrapError("Required command was not found in PATH: git")
    if source_dir.exists() and (source_dir / ".git").exists() and source.get("recursive_submodules", False):
        command_io.out(f"Repairing {manifest['name']} submodules in \"{source_dir}\"...")
        _update_submodules(source_dir, command_io)
        if verify_required_files(source_dir, required):
            command_io.out(f"{manifest['name']} source is now complete at \"{source_dir}\".")
            return
    if source_dir.exists():
        raise BootstrapError(
            f"{manifest['name']} source directory exists but is incomplete: \"{source_dir}\".\n"
            "Please remove it and run bootstrap again, or repair it manually."
        )
    if commit := source.get("commit"):
        command_io.out(f"Cloning {manifest['name']} source commit {commit} into \"{source_dir}\"...")
        source_dir.parent.mkdir(parents=True, exist_ok=True)
        run_command(["git", "init", str(source_dir)], command_io=command_io)
        run_command(["git", "-C", str(source_dir), "remote", "add", "origin", source["url"]], command_io=command_io)
        run_command(["git", "-C", str(source_dir), "fetch", "--depth", "1", "origin", commit], command_io=command_io)
        run_command(["git", "-C", str(source_dir), "checkout", "--detach", "FETCH_HEAD"], command_io=command_io)
    else:
        command_io.out(f"Cloning {manifest['name']} source {source['tag']} into \"{source_dir}\"...")
        run_command([
            "git", "clone", "--branch", source["tag"], "--depth", "1",
            *(["--recurse-submodules"] if source.get("recursive_submodules", False) else []),
            source["url"], str(source_dir),
        ], command_io=command_io)
    if source.get("recursive_submodules", False):
        _update_submodules(source_dir, command_io)
    if not verify_required_files(source_dir, required):
        raise BootstrapError(
            f"{manifest['name']} source clone completed, but expected files were not found in \"{source_dir}\"."
        )


def _copy_extracted(source_root: Path, destination: Path) -> None:
    entries = list(source_root.iterdir())
    content_root = entries[0] if len(entries) == 1 and entries[0].is_dir() else source_root
    destination.mkdir(parents=True, exist_ok=True)
    for entry in content_root.iterdir():
        target = destination / entry.name
        if entry.is_dir():
            shutil.copytree(entry, target, dirs_exist_ok=True)
        else:
            shutil.copy2(entry, target)


def ensure_archive_source(
    manifest: dict[str, Any],
    platform_name: str,
    repository: RepositoryContext,
    command_io: CommandIO,
) -> None:
    source_dir = resolve_repo_path(manifest["source_dir"], repository)
    platform_source = manifest["source"]["platforms"].get(platform_name)
    if not platform_source:
        raise BootstrapError(f"{manifest['name']} does not define a bootstrap archive for platform {platform_name}.")
    required = platform_source["required_files"]
    if verify_required_files(source_dir, required):
        command_io.out(f"{manifest['name']} package is already available at \"{source_dir}\".")
        return
    if source_dir.exists():
        raise BootstrapError(
            f"{manifest['name']} directory exists but is incomplete: \"{source_dir}\".\n"
            "Please remove it and run bootstrap again, or repair it manually."
        )
    url = platform_source["url"]
    archive_name = platform_source["archive_name"]
    command_io.out(f"Downloading {manifest['name']} package from {url}...")
    with tempfile.TemporaryDirectory(prefix=f"durin-{manifest['name']}-") as temporary:
        archive_path = Path(temporary) / archive_name
        extract_dir = Path(temporary) / "extract"
        extract_dir.mkdir()
        urllib.request.urlretrieve(url, archive_path)
        if checksum := platform_source.get("sha256"):
            actual = compute_sha256(archive_path)
            if actual.lower() != checksum.lower():
                raise BootstrapError(
                    f"{manifest['name']} archive integrity verification failed for \"{archive_path}\".\n"
                    f"Expected SHA-256: {checksum.lower()}\nActual SHA-256:   {actual}"
                )
        if archive_name.endswith(".zip"):
            with zipfile.ZipFile(archive_path) as archive:
                archive.extractall(extract_dir)
        elif archive_name.endswith(".tar.gz") or archive_name.endswith(".tgz"):
            with tarfile.open(archive_path, "r:gz") as archive:
                archive.extractall(extract_dir)
        else:
            raise BootstrapError(f"Unsupported archive format for {archive_name}")
        _copy_extracted(extract_dir, source_dir)
    if not verify_required_files(source_dir, required):
        raise BootstrapError(
            f"{manifest['name']} package was extracted, but expected files were not found in \"{source_dir}\"."
        )


def ensure_source_prepared(
    manifest: dict[str, Any],
    platform_name: str,
    repository: RepositoryContext,
    command_io: CommandIO,
) -> None:
    kind = manifest["source"]["type"]
    if kind == "git":
        ensure_git_source(manifest, repository, command_io)
    elif kind == "archive":
        ensure_archive_source(manifest, platform_name, repository, command_io)
    else:
        raise BootstrapError(f"Unsupported source type for {manifest['name']}: {kind}")
