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

    def test_terminal_layout_focuses_the_first_pane_before_the_fourth_split(self) -> None:
        worktrees = [Worktree(Path(f'C:/repo-{index}'), f'branch-{index}') for index in range(4)]
        with mock.patch.object(worktree_terminal, 'environment_arguments', return_value=[]):
            arguments = worktree_terminal.terminal_arguments(worktrees, REPOSITORY, CommandIO.system())
        focus_original = arguments.index('move-focus')
        assert arguments[:3] == ['-w', 'new', '--maximized']
        assert arguments[focus_original:focus_original + 3] == ['move-focus', 'first', ';']
        assert arguments[focus_original + 3:focus_original + 7] == ['split-pane', '-H', '--size', '0.5']

    def test_terminal_layout_repeats_the_same_balanced_grid_on_each_tab(self) -> None:
        worktrees = [Worktree(Path(f'C:/repo-{index}'), f'branch-{index}') for index in range(8)]
        with mock.patch.object(worktree_terminal, 'environment_arguments', return_value=[]):
            arguments = worktree_terminal.terminal_arguments(worktrees, REPOSITORY, CommandIO.system())

        commands: list[list[str]] = []
        current: list[str] = []
        for argument in arguments[3:]:
            if argument == ';':
                commands.append(current)
                current = []
            else:
                current.append(argument)
        commands.append(current)

        assert [command[:2] for command in commands] == [
            ['new-tab', '--startingDirectory'],
            ['split-pane', '-V'],
            ['split-pane', '-H'],
            ['move-focus', 'first'],
            ['split-pane', '-H'],
            ['new-tab', '--startingDirectory'],
            ['split-pane', '-V'],
            ['split-pane', '-H'],
            ['move-focus', 'first'],
            ['split-pane', '-H'],
        ]
        split_commands = [command for command in commands if command[0] == 'split-pane']
        assert all(command[2:4] == ['--size', '0.5'] for command in split_commands)
