import pytest
import argparse
import subprocess
from pathlib import Path
from unittest import mock
REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
from durin_dev_tool.bootstrap import agent_config, application as bootstrap_application, handler, manifests as dependency_manifests, preflight, setup, toolchain_selection
from durin_dev_tool.context import CommandIO, RepositoryContext
from durin_dev_tool.registry import CommandRegistry
from durin_dev_tool.worktree import application as worktree_application
from durin_dev_tool.worktree import git as worktree_git
from durin_dev_tool.worktree import transactions as worktree_transactions
from durin_dev_tool.worktree.models import DetachedLink, Worktree, WorktreeToolError

REPOSITORY = RepositoryContext.load(REPOSITORY_ROOT)


class TestWorktreeTool:

    def test_worktree_porcelain_parser_preserves_branch_and_lock_state(self) -> None:
        worktrees = worktree_git.parse_worktrees('worktree C:/repo\nHEAD 0123456789\nbranch refs/heads/main\n\nworktree C:/repo-feature\nHEAD abcdef0123\ndetached\nlocked in use\n')
        assert worktrees == [Worktree(Path('C:/repo'), 'main', False), Worktree(Path('C:/repo-feature'), None, True)]

    def test_add_prepares_without_calling_setup(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        target = Path(directory) / 'feature'
        args = argparse.Namespace(path=str(target), branch='feature', detach=False, commit_ish=None, source=None, link_type='auto')
        git_result = subprocess.CompletedProcess([], 0, '', '')
        with mock.patch.object(worktree_git, 'git_command', return_value=git_result) as git, mock.patch.object(worktree_transactions, 'prepare_worktree') as prepare:
            worktree_application.add_worktree(target, branch='feature', detach=False, commit_ish=None, source_value=None, link_type='auto', repository=REPOSITORY, command_io=CommandIO.system())
        git.assert_called_once_with(
            ['worktree', 'add', '-b', 'feature', str(target)],
            repository=mock.ANY,
            command_io=mock.ANY,
            capture_output=False,
        )
        prepare.assert_called_once_with(
            target,
            source_value=None,
            link_type='auto',
            dry_run=False,
            repository=mock.ANY,
            command_io=mock.ANY,
        )

    def test_unified_worktree_family_uses_explicit_leaf_commands(self) -> None:
        registry = CommandRegistry()
        specification, namespace = registry.parse(['worktree', 'open', '--dry-run'])
        assert specification.name == 'open'
        assert namespace.worktree_action == 'open'
        assert namespace.dry_run

    def test_worktree_family_defaults_to_list(self) -> None:
        registry = CommandRegistry()
        specification, namespace = registry.parse(['worktree'])
        assert specification.name == 'list'
        assert namespace.worktree_action == 'list'

    def test_registry_owns_exactly_five_worktree_commands(self) -> None:
        registry = CommandRegistry()
        family = next((specification for specification in registry.specifications if specification.name == 'worktree'))
        assert tuple((child.name for child in family.subcommands)) == ('open', 'list', 'add', 'prepare', 'remove')
        assert family.default_subcommand == 'list'
        assert 'list' in family.summary
        assert 'inspect' not in family.summary
        assert not hasattr(handler, 'create_parser')
