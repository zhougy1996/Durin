import pytest
import argparse
import subprocess
from pathlib import Path
from unittest import mock
REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
from durin_dev_tool.bootstrap import agent_config, application as bootstrap_application, handler, manifests as dependency_manifests, preflight, setup, toolchain_selection
from durin_dev_tool.context import CommandIO, RepositoryContext
from durin_dev_tool.python_environment import prepared_python_path
from durin_dev_tool.worktree import git as worktree_git
from durin_dev_tool.worktree import handler as worktree_handler
from durin_dev_tool.worktree import links as worktree_links
from durin_dev_tool.worktree import transactions as worktree_transactions
from durin_dev_tool.worktree.models import DetachedLink, Worktree, WorktreeToolError

REPOSITORY = RepositoryContext.load(REPOSITORY_ROOT)


class TestWorktreeTool:

    def test_prepare_current_worktree_restarts_an_interactive_shell(self) -> None:
        namespace = argparse.Namespace(
            worktree_action='prepare',
            path=None,
            source=None,
            link_type='auto',
            dry_run=False,
            plain=True,
        )
        session: dict[str, object] = {}
        with mock.patch.object(worktree_transactions, 'prepare_worktree'), mock.patch.object(
            worktree_handler, 'restart_prepared_shell'
        ) as restart:
            assert worktree_handler.run(
                namespace,
                repository_root=REPOSITORY_ROOT,
                stdout=mock.Mock(),
                stderr=mock.Mock(),
                repository_context=REPOSITORY,
                session_state=session,
            ) == 0
        restart.assert_called_once_with(
            REPOSITORY.root,
            prepared_python_path(
                REPOSITORY.root,
                REPOSITORY.config.worktrees.python_environment,
            ),
            session,
            mock.ANY,
        )

    def test_remove_refuses_main_worktree(self) -> None:
        main = Path('C:/repo')
        with pytest.raises(WorktreeToolError, match='main worktree'):
            worktree_transactions.require_registered_linked_worktree(main, [Worktree(main, 'main')])

    def test_prepare_allows_a_locked_linked_worktree(self) -> None:
        main = Worktree(Path('C:/repo'), 'main')
        locked = Worktree(Path('C:/repo-feature'), 'feature', True)
        assert worktree_transactions.require_registered_linked_worktree(locked.path, [main, locked], require_unlocked=False) == locked

    def test_remove_detaches_shared_links_before_git_removal(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        main = root / 'main'
        linked = root / 'feature'
        main.mkdir()
        linked.mkdir()
        shared_link = linked / '.venv'
        args = argparse.Namespace(path=str(linked), force=False, dry_run=False)
        worktrees = [Worktree(main, 'main'), Worktree(linked, 'feature')]
        detached = DetachedLink(shared_link, main / '.venv', 'junction')
        git_result = subprocess.CompletedProcess([], 0, '', '')
        events: list[str] = []

        def detach(path: Path) -> DetachedLink:
            events.append(f'detach:{path.name}')
            return detached

        def run_git(arguments: list[str], **_: object) -> subprocess.CompletedProcess[str]:
            events.append(f"git:{' '.join(arguments)}")
            return git_result
        with mock.patch.object(worktree_git, 'get_worktrees', return_value=worktrees), mock.patch.object(worktree_git, 'require_clean_worktree'), mock.patch.object(worktree_links, 'validate_directory_links', return_value=[shared_link]), mock.patch.object(worktree_links, 'detach_link', side_effect=detach), mock.patch.object(worktree_git, 'git_command', side_effect=run_git):
            worktree_transactions.remove_worktree(linked, force=False, dry_run=False, repository=REPOSITORY, command_io=CommandIO.system())
        assert events[0] == 'detach:.venv'
        assert events[1] == f'git:worktree remove {linked}'

    def test_remove_restores_detached_links_when_git_fails(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        main = root / 'main'
        linked = root / 'feature'
        main.mkdir()
        linked.mkdir()
        shared_link = linked / '.venv'
        args = argparse.Namespace(path=str(linked), force=False, dry_run=False)
        worktrees = [Worktree(main, 'main'), Worktree(linked, 'feature')]
        detached = DetachedLink(shared_link, main / '.venv', 'junction')
        git_result = subprocess.CompletedProcess([], 1, '', 'locked')
        with mock.patch.object(worktree_git, 'get_worktrees', return_value=worktrees), mock.patch.object(worktree_git, 'require_clean_worktree'), mock.patch.object(worktree_links, 'validate_directory_links', return_value=[shared_link]), mock.patch.object(worktree_links, 'detach_link', return_value=detached), mock.patch.object(worktree_git, 'git_command', return_value=git_result), mock.patch.object(worktree_links, 'restore_link') as restore:
            with pytest.raises(WorktreeToolError, match='Removing Git worktree'):
                worktree_transactions.remove_worktree(linked, force=False, dry_run=False, repository=REPOSITORY, command_io=CommandIO.system())
        restore.assert_called_once_with(detached)
