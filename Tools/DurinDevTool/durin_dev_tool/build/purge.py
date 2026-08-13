"""Safe discovery and removal of generated build artifacts."""

from __future__ import annotations

import shutil
from pathlib import Path
from typing import Callable, Sequence

from .config import (
    BuildPaths,
    BuildContext,
    BuildProfile,
    BuildToolError,
    ConfigurePreset,
    default_build_paths,
    preset_build_directory,
    preset_cache_string,
    preset_install_directory,
    preset_output_configuration,
)
from .output import BuildOutput
from .recovery import interruption_marker_path


def workspace_project_roots(root: Path | None = None) -> list[Path]:
    root = root or default_build_paths().root
    return sorted({descriptor.parent for descriptor in root.glob("*/*.dproject")})


def require_purge_child(path: Path, parent: Path) -> Path:
    resolved = path.resolve()
    try:
        relative = resolved.relative_to(parent.resolve())
    except ValueError as exc:
        raise BuildToolError(f'Purge path escapes its allowed root: "{resolved}"') from exc
    if not relative.parts:
        raise BuildToolError(f'Purge cannot remove an output root directly: "{resolved}"')
    return resolved


def collect_purge_paths(
    profile: BuildProfile,
    selected_presets: Sequence[ConfigurePreset],
    *,
    root: Path | None = None,
) -> list[Path]:
    root = root or default_build_paths().root
    paths: set[Path] = set()
    output_configs: set[str] = set()
    third_party_configs: set[str] = set()
    intermediate_profiles: set[tuple[str, str, str]] = set()
    for preset in selected_presets:
        paths.add(require_purge_child(preset_build_directory(preset, root=root), root / "Build"))
        install_directory = preset_install_directory(preset, root=root)
        if install_directory is not None:
            paths.add(require_purge_child(install_directory, root / "Install"))
        output_configs.add(preset_output_configuration(preset))
        third_party_configs.add(preset_cache_string(preset, "CMAKE_BUILD_TYPE"))
        intermediate_profiles.add(
            (
                "Build",
                profile.platform,
                preset_cache_string(preset, "DURIN_RUNTIME_VARIANT"),
            )
        )
        paths.add(interruption_marker_path(preset.name, root / "Build" / ".agent-state"))
    for project_root in workspace_project_roots(root):
        for output_config in output_configs:
            paths.add(
                require_purge_child(
                    project_root / "Binaries" / profile.platform / output_config,
                    project_root / "Binaries",
                )
            )
        for third_party_config in third_party_configs:
            paths.add(
                require_purge_child(
                    project_root / "Binaries" / profile.platform / "ThirdParty" / third_party_config,
                    project_root / "Binaries",
                )
            )
        for intermediate_root, platform_name, runtime_variant in intermediate_profiles:
            paths.add(
                require_purge_child(
                    project_root / "Intermediate" / intermediate_root / platform_name / runtime_variant,
                    project_root / "Intermediate",
                )
            )
    return sorted(paths, key=lambda path: (len(path.parts), str(path).lower()), reverse=True)


def remove_purge_paths(paths: Sequence[Path], *, root: Path | None = None) -> None:
    root = root or default_build_paths().root
    checkout_root = root.resolve()
    for path in paths:
        resolved = path.resolve()
        try:
            resolved.relative_to(checkout_root)
        except ValueError as exc:
            raise BuildToolError(f'Purge path escapes the checkout: "{resolved}"') from exc
        if resolved == checkout_root:
            raise BuildToolError("Purge cannot remove the checkout root.")
        try:
            if path.is_symlink() or path.is_file():
                path.unlink(missing_ok=True)
            elif path.is_dir():
                shutil.rmtree(path)
        except OSError as exc:
            raise BuildToolError(f'Could not purge build artifact path "{path}": {exc}') from exc


def execute_purge(
    context: BuildContext,
    output: BuildOutput,
    confirm: Callable[[Sequence[Path], bool], bool],
) -> None:
    paths_config = BuildPaths.from_repository(context.repository) if context.repository else default_build_paths()
    selected = [context.preset]
    if context.request.all_presets:
        selected = [context.presets[name] for name in context.profile.presets]
    paths = [
        path
        for path in collect_purge_paths(
            context.profile,
            selected,
            root=paths_config.root,
        )
        if path.exists() or path.is_symlink()
    ]
    if not paths:
        scope = "all registered presets" if context.request.all_presets else f'preset "{context.preset.name}"'
        output.warning(f"No build artifacts were found for {scope}.")
        return
    if not context.request.yes and not confirm(paths, context.request.all_presets):
        output.cancelled("Purge cancelled.")
        return
    with output.stage("Purge"):
        remove_purge_paths(paths, root=paths_config.root)
    output.success(f"Purged {len(paths)} build artifact path(s).")
