
import hashlib
import io
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from unittest import mock

import pytest

PRODUCT_ROOT = Path(__file__).resolve().parents[1]
REPOSITORY_ROOT = PRODUCT_ROOT.parents[1]
from durin_dev_tool import cli
from durin_dev_tool.context import CommandIO, RepositoryContext
from durin_dev_tool.bootstrap import manifests as bootstrap_manifests
from durin_dev_tool.build.errors import BuildToolError
from durin_dev_tool.errors import DevToolError
from durin_dev_tool.registry import CommandRegistry, CommandSpec
from durin_dev_tool.shell import split_shell_command
from durin_dev_tool.worktree import transactions as worktree_transactions


COMMAND_CASES = (
    (("help",), "help"),
    (("shell",), "shell"),
    (("setup",), "setup"),
    (("stop",), "stop"),
    (("presets",), "presets"),
    (("preset",), "preset"),
    (("status",), "status"),
    (("path",), "path root"),
    (("open",), "open runtime"),
    (("configure",), "configure"),
    (("build",), "build"),
    (("clean",), "clean"),
    (("recover",), "recover"),
    (("purge",), "purge"),
    (("rebuild",), "rebuild"),
    (("test",), "test"),
    (("run",), "run"),
    (("cook",), "cook --output Saved/Cooked --target win64 --target-profile game"),
    (("asset", "check"), "asset check --baseline --json"),
    (("asset", "resave"), "asset resave /Game/Characters --apply"),
    (("asset", "storage"), "asset storage"),
    (
        ("scene", "graybox-build"),
        "scene graybox-build --project Sandbox/Sandbox.dproject --output /Game/Test",
    ),
    (
        ("create", "module"),
        "create module Sample --project Sandbox/Sandbox.dproject",
    ),
    (("create", "project"), "create project Sample --path Examples/Sample"),
    (("doc", "list"), "doc list"),
    (("doc", "find"), 'doc find "build tools"'),
    (("doc", "refs"), "doc refs Documentation/README.md"),
    (("doc", "validate"), "doc validate"),
    (("doc", "move"), "doc move Documentation/A.md Documentation/B.md"),
    (("doc", "task", "list"), "doc task list"),
    (("doc", "task", "validate"), "doc task validate"),
    (("doc", "task", "remove"), "doc task remove Documentation/Tasks/Test.md"),
    (("doc", "plan", "list"), "doc plan list"),
    (("doc", "plan", "context"), "doc plan context Test"),
    (("doc", "plan", "validate"), "doc plan validate"),
    (("doc", "plan", "archive"), "doc plan archive 2026-08"),
    (("doc", "roadmap", "list"), "doc roadmap list"),
    (("doc", "roadmap", "validate"), "doc roadmap validate"),
    (("doc", "roadmap", "archive"), "doc roadmap archive 2026-08"),
    (("dependency", "prepare"), "dependency prepare"),
    (("dependency", "validate"), "dependency validate"),
    (("worktree", "open"), "worktree open"),
    (("worktree", "list"), "worktree list"),
    (("worktree", "add"), "worktree add ../Durin-worker"),
    (("worktree", "prepare"), "worktree prepare"),
    (("worktree", "remove"), "worktree remove ../Durin-worker"),
)

EXPECTED_COMMAND_PATHS = {
    ("help",),
    ("shell",),
    ("setup",),
    ("stop",),
    ("presets",),
    ("preset",),
    ("status",),
    ("path",),
    ("open",),
    ("configure",),
    ("build",),
    ("clean",),
    ("recover",),
    ("purge",),
    ("rebuild",),
    ("test",),
    ("run",),
    ("cook",),
    ("asset",),
    ("asset", "check"),
    ("asset", "resave"),
    ("asset", "storage"),
    ("scene",),
    ("scene", "graybox-build"),
    ("create",),
    ("create", "module"),
    ("create", "project"),
    ("doc",),
    ("doc", "list"),
    ("doc", "find"),
    ("doc", "refs"),
    ("doc", "validate"),
    ("doc", "move"),
    ("doc", "task"),
    ("doc", "task", "list"),
    ("doc", "task", "validate"),
    ("doc", "task", "remove"),
    ("doc", "plan"),
    ("doc", "plan", "list"),
    ("doc", "plan", "context"),
    ("doc", "plan", "validate"),
    ("doc", "plan", "archive"),
    ("doc", "roadmap"),
    ("doc", "roadmap", "list"),
    ("doc", "roadmap", "validate"),
    ("doc", "roadmap", "archive"),
    ("dependency",),
    ("dependency", "prepare"),
    ("dependency", "validate"),
    ("worktree",),
    ("worktree", "open"),
    ("worktree", "list"),
    ("worktree", "add"),
    ("worktree", "prepare"),
    ("worktree", "remove"),
}


def command_paths(registry: CommandRegistry) -> set[tuple[str, ...]]:
    paths: set[tuple[str, ...]] = set()

    def visit(specification: CommandSpec, parent: tuple[str, ...] = ()) -> None:
        path = (*parent, specification.name)
        paths.add(path)
        for child in specification.subcommands:
            visit(child, path)

    for specification in registry.specifications:
        visit(specification)
    return paths


def comparable_namespace(namespace: object) -> dict[str, object]:
    values = vars(namespace).copy()
    values.pop("_command_spec")
    return values


class TestCommandGrammarContract:
    def test_feature_spec_imports_do_not_eagerly_import_handlers(self) -> None:
        result = subprocess.run(
            [
                sys.executable,
                "-c",
                (
                    "import sys; "
                    "import durin_dev_tool.commands.asset_specs; "
                    "import durin_dev_tool.commands.bootstrap_specs; "
                    "import durin_dev_tool.commands.build_specs; "
                    "import durin_dev_tool.commands.cook_specs; "
                    "import durin_dev_tool.commands.documentation_specs; "
                    "import durin_dev_tool.commands.scene_specs; "
                    "import durin_dev_tool.commands.worktree_specs; "
                    "assert 'durin_dev_tool.asset' not in sys.modules; "
                    "assert 'durin_dev_tool.storage_qualification' not in sys.modules; "
                    "assert 'durin_dev_tool.bootstrap.handler' not in sys.modules; "
                    "assert 'durin_dev_tool.build.handler' not in sys.modules; "
                    "assert 'durin_dev_tool.cook' not in sys.modules; "
                    "assert 'durin_dev_tool.documentation.handler' not in sys.modules; "
                    "assert 'durin_dev_tool.scene' not in sys.modules; "
                    "assert 'durin_dev_tool.worktree.handler' not in sys.modules"
                ),
            ],
            cwd=PRODUCT_ROOT,
            check=False,
            capture_output=True,
            text=True,
        )
        assert result.returncode == 0, result.stderr

    def test_registry_reuses_one_composed_parser(self) -> None:
        registry = CommandRegistry()
        parser = registry.parser()

        registry.parse(["help"])
        registry.parse(["asset", "check", "--project", "Examples/Sandbox.dproject"])

        assert registry.parser() is parser

    def test_all_registered_paths_and_leaf_examples_are_frozen(self) -> None:
        registry = CommandRegistry()
        assert command_paths(registry) == EXPECTED_COMMAND_PATHS
        assert {path for path, _command in COMMAND_CASES} == {
            path for path in EXPECTED_COMMAND_PATHS if not any(
                candidate[: len(path)] == path and len(candidate) > len(path)
                for candidate in EXPECTED_COMMAND_PATHS
            )
        }

        for expected_path, command in COMMAND_CASES:
            direct_spec, direct_namespace = registry.parse(split_shell_command(command))
            shell_spec, shell_namespace = registry.parse(split_shell_command(command))
            assert direct_spec.name == expected_path[-1]
            assert shell_spec is direct_spec
            assert comparable_namespace(shell_namespace) == comparable_namespace(
                direct_namespace
            )

    def test_command_help_snapshot_is_frozen(self) -> None:
        registry = CommandRegistry()
        paths = sorted(command_paths(registry))
        snapshot = "\n\0\n".join(
            f"{' '.join(path)}\n{registry.format_command_help(path)}" for path in paths
        )
        assert hashlib.sha256(snapshot.encode()).hexdigest() == (
            "5868ae9300629cb1458e6cfeeb95068340db52483d8ba222ac1da57f17cc3239"
        )
        assert hashlib.sha256(registry.format_help().encode()).hexdigest() == (
            "7d44e1043a23fa443bb05186e4aefdf451b537dcfd8fb6b4439f1aab24dd4f83"
        )




class TestCommandBoundaryContract:
    def test_repository_contexts_do_not_leak_between_services(
        self,
        tmp_path: Path,
    ) -> None:
        base = RepositoryContext.load(REPOSITORY_ROOT)
        first_root = tmp_path / "first"
        second_root = tmp_path / "second"
        first = base.at_root(first_root)
        second = base.at_root(second_root)
        for repository, name in ((first, "first"), (second, "second")):
            manifest_directory = repository.resolve(
                repository.config.paths.third_party_manifests
            )
            manifest_directory.mkdir(parents=True)
            (manifest_directory / f"{name}.json").write_text(
                f'{{"name": "{name}"}}',
                encoding="utf-8",
            )

        assert [item["name"] for item in bootstrap_manifests.load_manifests(first)] == [
            "first"
        ]
        assert [item["name"] for item in bootstrap_manifests.load_manifests(second)] == [
            "second"
        ]
        assert not hasattr(bootstrap_manifests, "REPO_ROOT")
        assert not hasattr(worktree_transactions, "REPO_ROOT")
        assert not hasattr(bootstrap_manifests, "repository_paths")
        assert not hasattr(worktree_transactions, "repository_paths")

    def test_registry_forwards_repository_root_streams_and_return_code(self) -> None:
        calls: list[tuple[Path, object, object, object, object]] = []

        def handler(namespace: object, **keywords: object) -> int:
            del namespace
            stdout = keywords["stdout"]
            stderr = keywords["stderr"]
            print("ordinary", file=stdout)
            print("diagnostic", file=stderr)
            calls.append(
                (
                    keywords["repository_root"],
                    stdout,
                    stderr,
                    keywords["repository_context"],
                    keywords["command_io"],
                )
            )
            return 7

        registry = CommandRegistry(
            specifications=(CommandSpec("probe", "probe boundary", "unused:handler"),),
        )
        roots = (Path("first-root"), Path("second-root"))
        with mock.patch.object(CommandSpec, "load_handler", return_value=handler):
            for root in roots:
                stdout = io.StringIO()
                stderr = io.StringIO()
                specification, namespace = registry.parse(["probe"])
                assert registry.execute(
                    specification,
                    namespace,
                    repository_root=root,
                    stdout=stdout,
                    stderr=stderr,
                ) == 7
                assert stdout.getvalue() == "ordinary\n"
                assert stderr.getvalue() == "diagnostic\n"
        assert [call[0] for call in calls] == list(roots)
        assert calls[0][1:] != calls[1][1:]
        assert all(isinstance(call[3], RepositoryContext) for call in calls)
        assert all(isinstance(call[4], CommandIO) for call in calls)
        assert [call[3].root for call in calls] == [
            (REPOSITORY_ROOT / root).resolve() for root in roots
        ]

    def test_registry_uses_supplied_context_and_io_without_rediscovery(self) -> None:
        repository = RepositoryContext.load(REPOSITORY_ROOT)
        command_io = CommandIO(io.StringIO(), io.StringIO(), plain=True)
        observed: dict[str, object] = {}

        def handler(namespace: object, **keywords: object) -> int:
            del namespace
            observed.update(keywords)
            return 0

        registry = CommandRegistry(
            specifications=(CommandSpec("probe", "probe boundary", "unused:handler"),),
        )
        specification, namespace = registry.parse(["probe"])
        with mock.patch.object(CommandSpec, "load_handler", return_value=handler), mock.patch.object(
            RepositoryContext,
            "load",
            side_effect=AssertionError("repository context was rediscovered"),
        ):
            assert registry.execute(
                specification,
                namespace,
                repository_context=repository,
                command_io=command_io,
            ) == 0
        assert observed["repository_context"] is repository
        assert observed["command_io"] is command_io
        assert observed["stdout"] is command_io.stdout
        assert observed["stderr"] is command_io.stderr

    @pytest.mark.parametrize(
        ("failure", "exit_code", "message"),
        (
            (DevToolError("expected"), 1, "Error: expected\n"),
            (KeyboardInterrupt(), 130, "Cancelled.\n"),
            (OSError("disk"), 1, "Error: operating system failure: disk\n"),
        ),
    )
    def test_cli_main_exit_codes_and_stderr_ownership(
        self,
        failure: BaseException,
        exit_code: int,
        message: str,
    ) -> None:
        stdout = io.StringIO()
        stderr = io.StringIO()
        with mock.patch.object(cli, "run", side_effect=failure), mock.patch.object(
            cli.sys, "stdout", stdout
        ), mock.patch.object(cli.sys, "stderr", stderr):
            assert cli.main([]) == exit_code
        assert stdout.getvalue() == ""
        assert stderr.getvalue() == message

    def test_build_error_structured_metadata_is_preserved(self) -> None:
        started = datetime(2026, 8, 13, 1, 2, 3, tzinfo=timezone.utc)
        ended = datetime(2026, 8, 13, 1, 2, 4, tzinfo=timezone.utc)
        error = BuildToolError(
            "failed",
            command=["cmake", "--build", "Build"],
            exit_code=9,
            recovery="rerun",
            output_excerpt="compiler output",
            log_path=Path("Build/command.log"),
            process_id=42,
            started_at_utc=started,
            ended_at_utc=ended,
        )
        assert str(error) == "failed"
        assert error.command == ["cmake", "--build", "Build"]
        assert error.exit_code == 9
        assert error.recovery == "rerun"
        assert error.output_excerpt == "compiler output"
        assert error.log_path == Path("Build/command.log")
        assert error.process_id == 42
        assert error.started_at_utc is started
        assert error.ended_at_utc is ended
