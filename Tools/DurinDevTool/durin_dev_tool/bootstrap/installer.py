"""Third-party source verification and CMake installation execution."""

from __future__ import annotations

from pathlib import Path
from typing import Any, Mapping, Sequence

from ..context import CommandIO, RepositoryContext
from . import sources
from .models import BootstrapError


def build_directory(manifest: dict[str, Any], platform: str, config: str, repository: RepositoryContext) -> Path:
    return repository.root / "Build" / "ThirdParty" / f"{platform}-{config}-{manifest['name']}"


def install_directory(manifest: dict[str, Any], platform: str, config: str, repository: RepositoryContext) -> Path:
    return (
        repository.root
        / repository.config.worktrees.external_directory
        / "Install"
        / platform
        / config
        / manifest["name"]
    )


def install_shared_library(
    manifest: dict[str, Any],
    platform: str,
    config: str,
    cmake_command: str,
    *,
    repository: RepositoryContext,
    command_io: CommandIO,
    environment: Mapping[str, str] | None,
) -> None:
    install_dir = install_directory(manifest, platform, config, repository)
    required_sets = manifest["install_required_file_sets"][config]
    if sources.verify_any_required_file_set(install_dir, required_sets):
        command_io.out(f"{manifest['name']} {config} is already installed at \"{install_dir}\".")
        return
    cmake_dir = sources.resolve_repo_path(manifest["cmake_dir"], repository)
    build_dir = build_directory(manifest, platform, config, repository)
    build_dir.parent.mkdir(parents=True, exist_ok=True)
    install_dir.parent.mkdir(parents=True, exist_ok=True)
    command_io.out(f"Installing {manifest['name']} {config}...")
    sources.run_command([
        cmake_command, "-S", str(cmake_dir), "-B", str(build_dir),
        "-D", f"CMAKE_BUILD_TYPE={config}",
        "-D", f"CMAKE_INSTALL_PREFIX={install_dir}",
    ], command_io=command_io, environment=environment)
    sources.run_command(
        [cmake_command, "--build", str(build_dir), "--config", config, "--target", "install"],
        command_io=command_io,
        environment=environment,
    )
    if not sources.verify_any_required_file_set(install_dir, required_sets):
        raise BootstrapError(
            f"{manifest['name']} {config} install completed, but expected files were not found in \"{install_dir}\"."
        )


def prepare_manifest(
    manifest: dict[str, Any],
    *,
    platform_name: str,
    configurations: Sequence[str],
    cmake_command: str,
    repository: RepositoryContext,
    command_io: CommandIO,
    environment: Mapping[str, str] | None,
) -> None:
    command_io.out(f"==> Preparing {manifest['name']} ({manifest['kind']})")
    if (
        manifest["source"]["type"] == "archive"
        and platform_name not in manifest["source"]["platforms"]
        and manifest.get("allow_unsupported_platform", False)
    ):
        command_io.out(f"Skipping {manifest['name']}: no package is available for platform {platform_name}.")
        return
    sources.ensure_source_prepared(manifest, platform_name, repository, command_io)
    source_dir = sources.resolve_repo_path(manifest["source_dir"], repository)
    if manifest["kind"] == "direct_source":
        required = manifest.get("required_files", [])
        if required and not sources.verify_required_files(source_dir, required):
            raise BootstrapError(f"{manifest['name']} source is missing required files after preparation.")
        return
    if manifest["kind"] == "prebuilt_sdk":
        required = manifest["required_files_by_platform"].get(platform_name)
        if not required:
            raise BootstrapError(f"{manifest['name']} does not define required files for platform {platform_name}.")
        if not sources.verify_required_files(source_dir, required):
            raise BootstrapError(f"{manifest['name']} package is missing required files after preparation.")
        return
    if manifest["kind"] == "tool_package":
        required = manifest["source"]["platforms"][platform_name]["required_files"]
        if not sources.verify_required_files(source_dir, required):
            raise BootstrapError(f"{manifest['name']} package is missing required files after preparation.")
        return
    if manifest["kind"] == "shared_install":
        for config in configurations:
            install_shared_library(
                manifest,
                platform_name,
                config,
                cmake_command,
                repository=repository,
                command_io=command_io,
                environment=environment,
            )
        return
    raise BootstrapError(f"Unsupported manifest kind for {manifest['name']}: {manifest['kind']}")
