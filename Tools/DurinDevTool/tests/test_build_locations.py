from . import build_request_fixtures as request_fixtures

import io
from functools import partial
from pathlib import Path
from unittest import mock

import pytest


REPO_ROOT = Path(__file__).resolve().parents[3]
from durin_dev_tool.build import config as build_config
from durin_dev_tool.build import locations as build_locations
from durin_dev_tool.build import operations as build_operations
from durin_dev_tool.build import opener as build_opener
from durin_dev_tool.build import runtime as build_runtime
from durin_dev_tool.build.output import BuildOutput


make_profile = partial(request_fixtures.make_profile, ("debug",))
make_preset = partial(request_fixtures.make_preset, testing=None)


class TestBuildLocations:
    def test_registry_resolves_canonical_locations_in_stable_order(
        self,
        tmp_path_factory: pytest.TempPathFactory,
    ) -> None:
        root = Path(tmp_path_factory.mktemp("location root"))
        profile = make_profile()
        preset = make_preset()

        locations = build_locations.resolve_all_locations(
            profile=profile,
            preset=preset,
            root=root,
        )

        assert tuple(location.spec.name for location in locations) == (
            "root",
            "build",
            "binaries",
            "output",
            "runtime",
            "saved",
            "configs",
            "runtime-logs",
            "tests",
            "logs",
        )
        assert tuple(location.path for location in locations) == (
            root,
            root / "Build/debug",
            root / "Engine/Binaries",
            root / "Engine/Binaries/Win64/Debug",
            root / "Engine/Binaries/Win64/Debug/Runtime/DurinEditor",
            root / "Engine/Binaries/Win64/Debug/Runtime/DurinEditor/Saved",
            root / "Engine/Binaries/Win64/Debug/Runtime/DurinEditor/Saved/Configs",
            root / "Engine/Binaries/Win64/Debug/Runtime/DurinEditor/Saved/Logs",
            root / "Engine/Binaries/Win64/Debug/Tests/DurinEditor/Bin",
            root / "Build/.agent-state/logs",
        )
        assert locations[0].is_directory
        assert not any(location.is_directory for location in locations[1:])

    def test_location_names_and_aliases_are_case_insensitive(self) -> None:
        canonical = build_locations.resolve_location("binaries", root=REPO_ROOT)
        alias = build_locations.resolve_location("BIN", root=REPO_ROOT)
        assert alias.spec is canonical.spec
        assert alias.path == canonical.path
        assert build_locations.location_names() == (
            "root",
            "build",
            "binaries",
            "output",
            "runtime",
            "saved",
            "configs",
            "runtime-logs",
            "tests",
            "logs",
        )
        assert build_locations.resolve_location(
            "engine-logs",
            profile=make_profile(),
            preset=make_preset(),
            root=Path("repo"),
        ).path == Path("repo/Engine/Binaries/Win64/Debug/Runtime/DurinEditor/Saved/Logs")

    def test_unknown_location_lists_bounded_choices(self) -> None:
        with pytest.raises(build_config.BuildToolError, match="Available locations: root, build"):
            build_locations.resolve_location("unknown")

    def test_context_locations_require_profile_and_preset(self) -> None:
        with pytest.raises(build_config.BuildToolError, match="selected CMake preset"):
            build_locations.resolve_location("build")
        with pytest.raises(build_config.BuildToolError, match="selected build profile"):
            build_locations.resolve_location("output", preset=make_preset())

    def test_runtime_and_test_executable_paths_use_shared_locations(self) -> None:
        profile = make_profile()
        preset = make_preset()
        root = Path("repo")
        assert build_runtime.runtime_executable_path(profile, preset, root=root) == (
            root
            / "Engine/Binaries/Win64/Debug/Runtime/DurinEditor/DurinEditor.exe"
        )
        assert build_runtime.test_executable_path(
            profile,
            preset,
            "CoreTests",
            root=root,
        ) == (
            root
            / "Engine/Binaries/Win64/Debug/Tests/DurinEditor/Bin/CoreTests.exe"
        )

    def test_windows_opener_uses_startfile_for_existing_directory(
        self,
        tmp_path_factory: pytest.TempPathFactory,
    ) -> None:
        root = Path(tmp_path_factory.mktemp("open location"))
        location = build_locations.resolve_location("root", root=root)
        with mock.patch.object(
            build_opener.os,
            "startfile",
            create=True,
        ) as startfile:
            build_opener.open_location(location, current_host="windows")
        startfile.assert_called_once_with(root)

    @pytest.mark.parametrize(
        ("current_host", "command"),
        (("macos", "open"), ("linux", "xdg-open")),
    )
    def test_posix_opener_uses_host_file_manager(
        self,
        tmp_path_factory: pytest.TempPathFactory,
        current_host: str,
        command: str,
    ) -> None:
        root = Path(tmp_path_factory.mktemp("open location"))
        location = build_locations.resolve_location("root", root=root)
        with mock.patch.object(build_opener.subprocess, "Popen") as popen:
            build_opener.open_location(location, current_host=current_host)
        popen.assert_called_once_with(
            [command, str(root)],
            cwd=build_config.default_build_paths().root,
            stdout=build_opener.subprocess.DEVNULL,
            stderr=build_opener.subprocess.DEVNULL,
        )

    def test_opener_rejects_missing_directory_before_platform_dispatch(self) -> None:
        profile = make_profile()
        preset = make_preset()
        location = build_locations.resolve_location(
            "runtime",
            profile=profile,
            preset=preset,
            root=Path("missing-root"),
        )
        with mock.patch.object(
            build_opener.os,
            "startfile",
            create=True,
        ) as startfile, pytest.raises(
            build_config.BuildToolError,
            match="Runtime directory was not found",
        ) as raised:
            build_opener.open_location(location, current_host="windows")
        startfile.assert_not_called()
        assert raised.value.recovery == (
            "Build the complete runtime first with build --target all."
        )

    def test_path_command_prints_exact_resolved_path(self) -> None:
        profile = make_profile()
        preset = make_preset()
        request = request_fixtures.command_request(
            build_config.Action.PATH,
            options=request_fixtures.LocationActionOptions(location="runtime"),
        )
        context = build_config.BuildContext(
            request,
            build_config.LocalConfig(),
            profile,
            {preset.name: preset},
            preset,
            "windows",
        )
        stdout = io.StringIO()
        output = BuildOutput(plain=True, stdout=stdout, stderr=io.StringIO())
        location = build_locations.ResolvedLocation(
            next(spec for spec in build_locations.LOCATION_SPECS if spec.name == "runtime"),
            Path("resolved/runtime"),
            False,
        )
        with mock.patch.object(
            build_operations,
            "resolve_location",
            return_value=location,
        ):
            build_operations.execute_location_request(request, context, output)
        assert stdout.getvalue() == f"{location.path}\n"

    def test_open_command_uses_resolved_location(self) -> None:
        profile = make_profile()
        preset = make_preset()
        request = request_fixtures.command_request(
            build_config.Action.OPEN,
            options=request_fixtures.LocationActionOptions(location="bin"),
        )
        context = build_config.BuildContext(
            request,
            build_config.LocalConfig(),
            profile,
            {preset.name: preset},
            preset,
            "windows",
        )
        stdout = io.StringIO()
        output = BuildOutput(plain=True, stdout=stdout, stderr=io.StringIO())
        location = build_locations.ResolvedLocation(
            next(spec for spec in build_locations.LOCATION_SPECS if spec.name == "binaries"),
            Path("resolved/binaries"),
            True,
        )
        with mock.patch.object(
            build_operations,
            "resolve_location",
            return_value=location,
        ), mock.patch.object(build_operations, "open_location") as open_location:
            build_operations.execute_location_request(request, context, output)
        open_location.assert_called_once_with(
            location,
            current_host="windows",
            root=build_config.default_build_paths().root,
        )
        assert f'Opened binaries directory: "{location.path}"' in stdout.getvalue()

    def test_path_all_plain_output_is_stable_tab_separated(self) -> None:
        profile = make_profile()
        preset = make_preset()
        request = request_fixtures.command_request(
            build_config.Action.PATH,
            output=build_config.OutputOptions(plain=True),
            options=request_fixtures.LocationActionOptions(all_locations=True),
        )
        context = build_config.BuildContext(
            request,
            build_config.LocalConfig(),
            profile,
            {preset.name: preset},
            preset,
            "windows",
        )
        stdout = io.StringIO()
        output = BuildOutput(plain=True, stdout=stdout, stderr=io.StringIO())
        build_operations.execute_location_request(request, context, output)
        lines = stdout.getvalue().splitlines()
        assert len(lines) == len(build_locations.LOCATION_SPECS)
        assert lines[0] == f"root\t{build_config.default_build_paths().root}"
        assert [line.partition("\t")[0] for line in lines] == list(
            build_locations.location_names()
        )
