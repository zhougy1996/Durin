import pytest
import os
import subprocess
import sys
from pathlib import Path
from unittest import mock
REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
PRODUCT_ROOT = REPOSITORY_ROOT / 'Tools' / 'DurinDevTool'
if str(PRODUCT_ROOT) not in sys.path:
    sys.path.insert(0, str(PRODUCT_ROOT))
from durin_dev_tool.bootstrap import agent_config, application as bootstrap_application, handler, manifests as dependency_manifests, preflight, setup, toolchain_selection
from durin_dev_tool.context import CommandIO, RepositoryContext
from durin_dev_tool.worktree import git as worktree_git
from durin_dev_tool.worktree import links as worktree_links
from durin_dev_tool.worktree import transactions as worktree_transactions
from durin_dev_tool.worktree.models import DetachedLink, Worktree, WorktreeToolError

REPOSITORY = RepositoryContext.load(REPOSITORY_ROOT)


class TestWorktreeTool:

    def test_prepare_validates_all_source_directories_before_linking(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        main = root / 'main'
        linked = root / 'feature'
        main.mkdir()
        linked.mkdir()
        worktrees = [Worktree(main, 'main'), Worktree(linked, 'feature')]
        with mock.patch.object(worktree_git, 'get_worktrees', return_value=worktrees), mock.patch.object(worktree_links, 'prepare_shared_directory_link') as prepare_link:
            with pytest.raises(WorktreeToolError, match='Prepared source directories are missing'):
                worktree_transactions.prepare_worktree(linked, source_value=str(main), link_type='auto', dry_run=True, repository=REPOSITORY, command_io=CommandIO.system())
        prepare_link.assert_not_called()

    def test_prepare_links_shared_vscode_configuration(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        main = root / 'main'
        linked = root / 'feature'
        for path in (
            main / '.agents',
            main / '.vscode',
            main / 'Engine' / 'External',
            main / '.venv',
            linked,
        ):
            path.mkdir(parents=True)
        worktrees = [Worktree(main, 'main'), Worktree(linked, 'feature')]
        with mock.patch.object(worktree_git, 'get_worktrees', return_value=worktrees), mock.patch.object(worktree_links, 'prepare_shared_directory_link') as prepare_link, mock.patch.object(worktree_transactions, 'validate_prerequisites'):
            worktree_transactions.prepare_worktree(
                linked,
                source_value=str(main),
                link_type='auto',
                dry_run=False,
                repository=REPOSITORY,
                command_io=CommandIO.system(),
            )
        specs = [call.args[2] for call in prepare_link.call_args_list]
        assert next(spec for spec in specs if spec.label == 'VS Code').relative_path == REPOSITORY.config.worktrees.vscode_directory

    def test_agent_and_vscode_links_are_data_driven_preserved_specs(self) -> None:
        specs = worktree_links.shared_directory_specs(REPOSITORY)
        preserved = {spec.label: spec.relative_path for spec in specs if spec.preserve_existing}
        assert preserved == {
            'Agent': REPOSITORY.config.worktrees.agent_directory,
            'VS Code': REPOSITORY.config.worktrees.vscode_directory,
        }

    def test_remove_refuses_unexpected_directory_links(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        worktree = Worktree(root, 'feature')
        unexpected = root / 'unexpected'
        with mock.patch.object(worktree_links, 'directory_links_under', return_value=[unexpected]):
            with pytest.raises(WorktreeToolError, match='unexpected directory links'):
                worktree_links.validate_directory_links(worktree, REPOSITORY)

    def test_remove_accepts_shared_vscode_link(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        worktree = Worktree(root, 'feature')
        vscode = root / '.vscode'
        with mock.patch.object(worktree_links, 'directory_links_under', return_value=[vscode]), mock.patch.object(worktree_links, 'is_link_like', side_effect=lambda path: path == vscode):
            assert worktree_links.validate_directory_links(worktree, REPOSITORY) == [vscode]

    @pytest.mark.skipif(os.name == 'nt', reason='POSIX directory symlink behavior')
    def test_removing_directory_symlink_preserves_its_target(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        root = Path(tmp_path_factory.mktemp('case'))
        target = root / 'target'
        link = root / 'link'
        target.mkdir()
        marker = target / 'preserved.txt'
        marker.write_text('preserved', encoding='utf-8')
        link.symlink_to(target, target_is_directory=True)

        worktree_links.remove_link_or_empty_directory(
            link,
            dry_run=False,
            command_io=CommandIO.system(),
        )

        assert not link.exists()
        assert marker.read_text(encoding='utf-8') == 'preserved'

    @pytest.mark.skipif(os.name == 'nt', reason='POSIX directory symlink behavior')
    def test_detaching_directory_symlink_preserves_its_target(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        root = Path(tmp_path_factory.mktemp('case'))
        target = root / 'target'
        link = root / 'link'
        target.mkdir()
        marker = target / 'preserved.txt'
        marker.write_text('preserved', encoding='utf-8')
        link.symlink_to(target, target_is_directory=True)

        detached = worktree_links.detach_link(link)

        assert detached == DetachedLink(link, target, 'symlink')
        assert not link.exists()
        assert marker.read_text(encoding='utf-8') == 'preserved'

    @pytest.mark.skipif(os.name != 'nt', reason='Windows junction behavior')
    def test_detaching_junction_preserves_its_target(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        target = root / 'target'
        link = root / 'link'
        target.mkdir()
        marker = target / 'preserved.txt'
        marker.write_text('preserved', encoding='utf-8')
        result = subprocess.run(['cmd.exe', '/d', '/c', 'mklink', '/J', str(link), str(target)], check=False, capture_output=True, text=True)
        assert result.returncode == 0, result.stderr or result.stdout
        detached = worktree_links.detach_link(link)
        assert detached.kind == 'junction'
        assert not link.exists()
        assert marker.read_text(encoding='utf-8') == 'preserved'
