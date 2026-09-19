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
