
import io
from pathlib import Path
from unittest import mock

import pytest

from durin_dev_tool import scene
from durin_dev_tool.context import RepositoryContext
from durin_dev_tool.errors import DevToolError
from durin_dev_tool.registry import CommandRegistry


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
REPOSITORY = RepositoryContext.load(REPOSITORY_ROOT)


def _namespace(arguments: list[str]):
    _spec, namespace = CommandRegistry().parse(
        ["scene", "graybox-build", *arguments]
    )
    return namespace


def test_graybox_build_forwards_one_bounded_startup_command(tmp_path: Path) -> None:
    project = tmp_path / "Sandbox.dproject"
    project.write_text("{}", encoding="utf-8")
    executable = tmp_path / "DurinEditor.exe"
    executable.touch()
    calls: list[tuple[list[str], dict[str, object]]] = []

    with mock.patch.object(
        RepositoryContext,
        "load",
        side_effect=AssertionError("repository context was rediscovered"),
    ):
        result = scene.run(
            _namespace(
                [
                    "--project", str(project),
                    "--output", "/Game/Levels/Arena",
                    "--width", "24",
                    "--ceiling",
                ]
            ),
            repository_root=tmp_path,
            repository_context=REPOSITORY,
            stdout=io.StringIO(),
            stderr=io.StringIO(),
            executable_resolver=lambda *_: executable,
            command_runner=lambda command, **kwargs: calls.append((list(command), kwargs)),
        )

    assert result == 0
    assert len(calls) == 1
    command, options = calls[0]
    assert command[0] == str(executable)
    assert "--hidden-window" in command
    assert command.count("--startup-command=graybox-build") == 1
    assert "--startup-command-arg=--output=/Game/Levels/Arena" in command
    assert "--startup-command-arg=--width=24" in command
    assert "--startup-command-arg=--ceiling" in command
    assert options["show_heartbeat"]
    assert options["wait_for_descendants"]
    assert options["timeout_seconds"] == 300


def test_graybox_build_rejects_invalid_dimension_before_process_start(tmp_path: Path) -> None:
    project = tmp_path / "Sandbox.dproject"
    project.write_text("{}", encoding="utf-8")
    executable = tmp_path / "DurinEditor.exe"
    executable.touch()
    with pytest.raises(DevToolError, match="--width"):
        scene.run(
            _namespace([
                "--project", str(project),
                "--output", "/Game/Levels/Arena",
                "--width", "0",
            ]),
            repository_root=tmp_path,
            repository_context=REPOSITORY,
            stdout=io.StringIO(),
            stderr=io.StringIO(),
            executable_resolver=lambda *_: executable,
            command_runner=lambda *_args, **_kwargs: None,
        )


def test_graybox_build_rejects_non_mounted_output(tmp_path: Path) -> None:
    project = tmp_path / "Sandbox.dproject"
    project.write_text("{}", encoding="utf-8")
    executable = tmp_path / "DurinEditor.exe"
    executable.touch()
    namespace = _namespace([
        "--project", str(project),
        "--output", "Sandbox/Levels/Arena",
    ])
    with pytest.raises(DevToolError, match="complete mounted Level path"):
        scene.run(
            namespace,
            repository_root=tmp_path,
            repository_context=REPOSITORY,
            stdout=io.StringIO(),
            stderr=io.StringIO(),
            executable_resolver=lambda *_: executable,
            command_runner=lambda *_args, **_kwargs: None,
        )
