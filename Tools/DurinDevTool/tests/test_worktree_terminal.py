import pytest
import json
from pathlib import Path
from unittest import mock
REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
from durin_dev_tool.bootstrap import agent_config, application as bootstrap_application, handler, manifests as dependency_manifests, preflight, setup, toolchain_selection
from durin_dev_tool.context import CommandIO, RepositoryContext
from durin_dev_tool.worktree import terminal as worktree_terminal
from durin_dev_tool.worktree.models import DetachedLink, Worktree, WorktreeToolError

REPOSITORY = RepositoryContext.load(REPOSITORY_ROOT)


class TestWorktreeTool:

    def test_terminal_environment_uses_typed_local_config(
        self,
        tmp_path_factory: pytest.TempPathFactory,
    ) -> None:
        root = Path(tmp_path_factory.mktemp('case'))
        script = root / 'toolchain' / 'setup.cmd'
        script.parent.mkdir()
        script.touch()
        config_path = root / '.agents' / 'DevTool.user.json'
        config_path.parent.mkdir()
        config_path.write_text(
            json.dumps(
                {
                    'version': 1,
                    'toolchain': {
                        'environmentScript': str(script),
                        'environmentArguments': ['x64'],
                    },
                }
            ),
            encoding='utf-8',
        )
        assert worktree_terminal.environment_arguments(root, REPOSITORY, CommandIO.system()) == [str(script), 'x64']

    @pytest.mark.parametrize('count', [1, 4, 5, 8])
    def test_terminal_layout_opens_one_tab_per_worktree(self, count: int) -> None:
        worktrees = [Worktree(Path(f'C:/repo {index}'), f'branch-{index}') for index in range(count)]
        environments = [[f'C:/toolchain {index}/setup.cmd', 'x64'] for index in range(count)]
        with mock.patch.object(worktree_terminal, 'environment_arguments', side_effect=environments):
            arguments = worktree_terminal.terminal_arguments(worktrees, REPOSITORY, CommandIO.system())

        assert arguments[:3] == ['-w', 'new', '--maximized']
        commands: list[list[str]] = []
        current: list[str] = []
        for argument in arguments[3:]:
            if argument == ';':
                commands.append(current)
                current = []
            else:
                current.append(argument)
        commands.append(current)

        assert len(commands) == count
        for command, worktree, environment in zip(commands, worktrees, environments):
            assert command == [
                'new-tab', '--startingDirectory', str(worktree.path),
                '--title', worktree.path.name, 'cmd.exe', '/k', *environment,
            ]

    def test_iterm_paths_are_shell_quoted_and_passed_as_data(self) -> None:
        path = Path('/repo space/\'quoted"\\$(touch unwanted)')
        arguments = worktree_terminal.iterm_arguments([Worktree(path, 'main')])
        import shlex
        assert arguments[:2] == ['osascript', '-']
        assert arguments[2] == path.name
        assert shlex.split(arguments[3])[:4] == ['cd', '--', str(path), '&&']
        assert shlex.split(arguments[3])[-1] == path.name
        assert str(path) not in worktree_terminal.ITERM_SCRIPT

    @pytest.mark.parametrize('line_break', ['\n', '\r'])
    def test_iterm_rejects_paths_that_inject_shell_lines(self, line_break: str) -> None:
        with pytest.raises(WorktreeToolError, match='line breaks'):
            worktree_terminal.iterm_arguments([Worktree(Path('/repo' + line_break + 'exit'), 'main')])

    @pytest.mark.parametrize('platform', ['darwin', 'win32'])
    @pytest.mark.parametrize('dry_run', [False, True])
    def test_open_dispatches_and_dry_run_does_not_launch(self, platform: str, dry_run: bool) -> None:
        worktrees = [Worktree(Path('/repo'), 'main'), Worktree(Path('/feature'), 'feature')]
        with (
            mock.patch.object(worktree_terminal.sys, 'platform', platform),
            mock.patch.object(worktree_terminal, 'get_worktrees', return_value=worktrees),
            mock.patch.object(worktree_terminal, 'environment_arguments', return_value=[]) as environment,
            mock.patch.object(worktree_terminal.subprocess, 'run') as run,
        ):
            run.return_value.returncode = 0
            worktree_terminal.open_worktree_terminals(REPOSITORY, CommandIO.system(), dry_run=dry_run)
        assert run.call_count == (0 if dry_run else 1)
        if platform == 'darwin':
            environment.assert_not_called()
            if not dry_run:
                assert run.call_args.args[0] == worktree_terminal.iterm_arguments(worktrees)
                assert run.call_args.kwargs['input'] == worktree_terminal.ITERM_SCRIPT
                assert worktree_terminal.ITERM_SCRIPT.count('create window with default profile') == 1
                assert 'if itemIndex > 1 then' in worktree_terminal.ITERM_SCRIPT
        elif not dry_run:
            assert run.call_args.args[0][0] == 'wt.exe'

    def test_iterm_reports_automation_failure(self) -> None:
        with mock.patch.object(worktree_terminal.subprocess, 'run') as run:
            run.return_value.returncode = 1
            run.return_value.stderr = 'Not authorized (-1743)'
            with pytest.raises(WorktreeToolError, match='Automation.*-1743'):
                worktree_terminal.open_iterm([Worktree(Path('/repo'), 'main')], CommandIO.system(), dry_run=False)

    def test_unsupported_platform_does_not_launch(self) -> None:
        with (
            mock.patch.object(worktree_terminal.sys, 'platform', 'linux'),
            mock.patch.object(worktree_terminal, 'get_worktrees', return_value=[Worktree(Path('/repo'), 'main')]),
            mock.patch.object(worktree_terminal.subprocess, 'run') as run,
        ):
            with pytest.raises(WorktreeToolError, match='supports'):
                worktree_terminal.open_worktree_terminals(REPOSITORY, CommandIO.system(), dry_run=False)
        run.assert_not_called()
