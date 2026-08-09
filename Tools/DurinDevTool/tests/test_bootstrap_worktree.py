from __future__ import annotations
import pytest
import argparse
import io
import json
import os
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path
from unittest import mock
REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
PRODUCT_ROOT = REPOSITORY_ROOT / 'Tools' / 'DurinDevTool'
if str(PRODUCT_ROOT) not in sys.path:
    sys.path.insert(0, str(PRODUCT_ROOT))
from durin_dev_tool.bootstrap import agent_config, dependencies, handler, preflight, setup
from durin_dev_tool.build import config as build_config
from durin_dev_tool import toolchain
from durin_dev_tool.registry import Capability, CommandRegistry
from durin_dev_tool.worktree import services

class TestThirdPartyBootstrap:

    @staticmethod
    def make_manifests() -> list[dict[str, object]]:
        return [{'name': 'normal', 'kind': 'direct_source', 'source_dir': 'normal', 'source': {'type': 'git', 'tag': 'v1'}}, {'name': 'tests', 'kind': 'direct_source', 'test_only': True, 'source_dir': 'tests', 'source': {'type': 'git', 'tag': 'v1'}}, {'name': 'tracy', 'kind': 'direct_source', 'development_only': True, 'source_dir': 'tracy', 'source': {'type': 'git', 'tag': 'v0.13.1'}}, {'name': 'tracy-tools', 'kind': 'tool_package', 'development_only': True, 'allow_unsupported_platform': True, 'source_dir': 'tracy-tools', 'source': {'type': 'archive', 'platforms': {'Win64': {'url': 'https://example.invalid/tracy-tools.zip', 'archive_name': 'tracy-tools.zip', 'required_files': ['tracy-profiler.exe']}}}}]

    def test_all_excludes_development_dependencies_by_default(self) -> None:
        selected = dependencies.resolve_selected_manifests(self.make_manifests(), use_all=True, libs_arg=None, with_tests=False, with_development=False)
        assert [manifest['name'] for manifest in selected] == ['normal']

    def test_all_can_include_development_and_test_dependencies(self) -> None:
        selected = dependencies.resolve_selected_manifests(self.make_manifests(), use_all=True, libs_arg=None, with_tests=True, with_development=True)
        assert [manifest['name'] for manifest in selected] == ['normal', 'tests', 'tracy', 'tracy-tools']

    def test_explicit_development_dependency_preserves_requested_order(self) -> None:
        selected = dependencies.resolve_selected_manifests(self.make_manifests(), use_all=False, libs_arg='tracy,normal', with_tests=False, with_development=False)
        assert [manifest['name'] for manifest in selected] == ['tracy', 'normal']

    def test_shared_install_commands_receive_toolchain_environment(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        root = Path(tmp_path_factory.mktemp('case'))
        manifest = {'name': 'library', 'kind': 'shared_install', 'cmake_dir': 'CMake', 'install_required_file_sets': {'Debug': [['include/library.h']]}}
        (root / 'CMake').mkdir()
        with mock.patch.object(dependencies, 'REPO_ROOT', root), mock.patch.object(dependencies, 'verify_any_required_file_set', side_effect=[False, True]), mock.patch.object(dependencies, 'run_command') as run:
            dependencies.install_shared_library(manifest, 'Win64', 'Debug', 'C:/cmake/bin/cmake.exe', environment={'PATH': 'ready'})
        assert run.call_count == 2
        assert all(call.kwargs['environment'] == {'PATH': 'ready'} for call in run.call_args_list)

    def test_configured_cmake_uses_typed_local_config(
        self,
        tmp_path_factory: pytest.TempPathFactory,
    ) -> None:
        root = Path(tmp_path_factory.mktemp('case'))
        config_path = root / '.agents' / 'DevTool.user.json'
        config_path.parent.mkdir()
        config_path.write_text(
            json.dumps({'version': 1, 'cmake': {'command': 'custom-cmake'}}),
            encoding='utf-8',
        )
        with dependencies.repository_paths(root), mock.patch.dict(
            dependencies.os.environ,
            {'CMAKE_COMMAND': ''},
        ):
            assert dependencies.configured_cmake_command() == 'custom-cmake'

    def test_development_only_must_be_boolean(self) -> None:
        manifests = self.make_manifests()
        manifests[2]['development_only'] = 'yes'
        with pytest.raises(dependencies.BootstrapError, match='must be a boolean'):
            dependencies.validate_manifests(manifests)

    @staticmethod
    def make_tool_manifest(*, sha256: str='0' * 64) -> dict[str, object]:
        return {'name': 'tracy-tools', 'version': '0.13.1', 'kind': 'tool_package', 'development_only': True, 'allow_unsupported_platform': True, 'repair_command': 'DevTool.bat dependency prepare --libs tracy,tracy-tools', 'source_dir': 'packages/tracy-tools/0.13.1/Win64', 'source': {'type': 'archive', 'platforms': {'Win64': {'url': 'https://example.invalid/windows-0.13.1.zip', 'archive_name': 'windows-0.13.1.zip', 'sha256': sha256, 'required_files': ['tracy-profiler.exe', 'tracy-capture.exe']}}}}

    def test_archive_sha256_must_be_64_hexadecimal_digits(self) -> None:
        manifest = self.make_tool_manifest(sha256='not-a-digest')
        with pytest.raises(dependencies.BootstrapError, match='64 hexadecimal digits'):
            dependencies.validate_manifests([manifest])

    def test_archive_digest_mismatch_does_not_publish_package(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        manifest = self.make_tool_manifest()
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        archive_path = root / 'source.zip'
        with zipfile.ZipFile(archive_path, 'w') as archive:
            archive.writestr('tracy-profiler.exe', b'profiler')
            archive.writestr('tracy-capture.exe', b'capture')
        with mock.patch.object(dependencies, 'REPO_ROOT', root), mock.patch.object(dependencies.urllib.request, 'urlretrieve', side_effect=lambda _url, destination: shutil.copy2(archive_path, destination)), pytest.raises(dependencies.BootstrapError, match='integrity verification failed'):
            dependencies.ensure_archive_source(manifest, 'Win64')
        assert not (root / manifest['source_dir']).exists()

    def test_archive_digest_successfully_publishes_required_files(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        archive_path = root / 'source.zip'
        with zipfile.ZipFile(archive_path, 'w') as archive:
            archive.writestr('tracy-profiler.exe', b'profiler')
            archive.writestr('tracy-capture.exe', b'capture')
        manifest = self.make_tool_manifest(sha256=dependencies.compute_sha256(archive_path))
        with mock.patch.object(dependencies, 'REPO_ROOT', root), mock.patch.object(dependencies.urllib.request, 'urlretrieve', side_effect=lambda _url, destination: shutil.copy2(archive_path, destination)):
            dependencies.process_manifest(manifest, platform_name='Win64', configs=['Debug', 'Release'], cmake_command='cmake')
        package_dir = root / manifest['source_dir']
        assert (package_dir / 'tracy-profiler.exe').is_file()
        assert (package_dir / 'tracy-capture.exe').is_file()

    def test_prepared_archive_does_not_download_again(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        manifest = self.make_tool_manifest()
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        package_dir = root / manifest['source_dir']
        package_dir.mkdir(parents=True)
        for file_name in manifest['source']['platforms']['Win64']['required_files']:
            (package_dir / file_name).touch()
        with mock.patch.object(dependencies, 'REPO_ROOT', root), mock.patch.object(dependencies.urllib.request, 'urlretrieve') as urlretrieve:
            dependencies.ensure_archive_source(manifest, 'Win64')
        urlretrieve.assert_not_called()

    def test_optional_tool_package_skips_unsupported_platform(self) -> None:
        manifest = self.make_tool_manifest()
        output = io.StringIO()
        with mock.patch('sys.stdout', output):
            dependencies.process_manifest(manifest, platform_name='Linux', configs=['Debug', 'Release'], cmake_command='cmake')
        assert 'Skipping tracy-tools' in output.getvalue()

    def test_status_query_is_read_only_and_reports_missing_files(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        manifest = self.make_tool_manifest()
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        with mock.patch.object(dependencies, 'REPO_ROOT', root):
            status = dependencies.query_manifest_status(manifest, 'Win64')
        assert not status['prepared']
        assert status['version'] == '0.13.1'
        assert status['missing_files'] == ['tracy-profiler.exe', 'tracy-capture.exe']
        assert not (root / 'packages').exists()

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
        assert services.environment_arguments(root) == [str(script), 'x64']

    def test_worktree_porcelain_parser_preserves_branch_and_lock_state(self) -> None:
        worktrees = services.parse_worktrees('worktree C:/repo\nHEAD 0123456789\nbranch refs/heads/main\n\nworktree C:/repo-feature\nHEAD abcdef0123\ndetached\nlocked in use\n')
        assert worktrees == [services.Worktree(Path('C:/repo'), 'main', False), services.Worktree(Path('C:/repo-feature'), None, True)]

    def test_terminal_layout_returns_to_original_pane_before_the_fourth_split(self) -> None:
        worktrees = [services.Worktree(Path(f'C:/repo-{index}'), f'branch-{index}') for index in range(4)]
        with mock.patch.object(services, 'environment_arguments', return_value=[]):
            arguments = services.terminal_arguments(worktrees)
        focus_original = arguments.index('move-focus')
        assert arguments[focus_original:focus_original + 6] == ['move-focus', 'previousInOrder', ';', 'move-focus', 'previousInOrder', ';']
        assert arguments[focus_original + 6:focus_original + 8] == ['split-pane', '-H']
        assert 'first' not in arguments

    def test_add_prepares_without_calling_setup(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        target = Path(directory) / 'feature'
        args = argparse.Namespace(path=str(target), branch='feature', detach=False, commit_ish=None, source=None, link_type='auto')
        git_result = subprocess.CompletedProcess([], 0, '', '')
        with mock.patch.object(services, 'git_command', return_value=git_result) as git, mock.patch.object(services, 'prepare_registered_worktree') as prepare:
            services.add_worktree(args)
        git.assert_called_once_with(['worktree', 'add', '-b', 'feature', str(target)], capture_output=False)
        prepare.assert_called_once_with(target, source_value=None, link_type='auto', dry_run=False)

    def test_remove_refuses_main_worktree(self) -> None:
        main = Path('C:/repo')
        with pytest.raises(services.WorktreeToolError, match='main worktree'):
            services.require_registered_linked_worktree(main, [services.Worktree(main, 'main')])

    def test_prepare_allows_a_locked_linked_worktree(self) -> None:
        main = services.Worktree(Path('C:/repo'), 'main')
        locked = services.Worktree(Path('C:/repo-feature'), 'feature', True)
        assert services.require_registered_linked_worktree(locked.path, [main, locked], require_unlocked=False) == locked

    def test_prepare_validates_all_source_directories_before_linking(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        main = root / 'main'
        linked = root / 'feature'
        main.mkdir()
        linked.mkdir()
        worktrees = [services.Worktree(main, 'main'), services.Worktree(linked, 'feature')]
        with mock.patch.object(services, 'get_worktrees', return_value=worktrees), mock.patch.object(services, 'prepare_agent_link') as prepare_agent:
            with pytest.raises(services.WorktreeToolError, match='Prepared source directories are missing'):
                services.prepare_registered_worktree(linked, source_value=str(main), link_type='auto', dry_run=True)
        prepare_agent.assert_not_called()

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
        worktrees = [services.Worktree(main, 'main'), services.Worktree(linked, 'feature')]
        with mock.patch.object(services, 'get_worktrees', return_value=worktrees), mock.patch.object(services, 'prepare_agent_link'), mock.patch.object(services, 'prepare_vscode_link') as prepare_vscode, mock.patch.object(services, 'prepare_directory_link'), mock.patch.object(services, 'run_preflight'):
            services.prepare_registered_worktree(
                linked,
                source_value=str(main),
                link_type='auto',
                dry_run=False,
            )
        prepare_vscode.assert_called_once_with(
            main,
            linked,
            link_type=services.choose_link_type('auto'),
            dry_run=False,
        )

    def test_agent_and_vscode_links_share_preservation_helper(self) -> None:
        source = Path('C:/repo')
        target = Path('C:/repo-feature')
        with mock.patch.object(services, 'prepare_preserved_directory_link') as prepare:
            services.prepare_agent_link(source, target, link_type='junction', dry_run=True)
            services.prepare_vscode_link(source, target, link_type='junction', dry_run=True)
        assert prepare.call_args_list == [
            mock.call(
                source,
                target,
                relative_path=services.AGENT_DIRECTORY,
                preservation_label='Agent',
                link_type='junction',
                dry_run=True,
            ),
            mock.call(
                source,
                target,
                relative_path=services.VSCODE_DIRECTORY,
                preservation_label='VS Code',
                link_type='junction',
                dry_run=True,
            ),
        ]

    def test_remove_refuses_unexpected_directory_links(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        worktree = services.Worktree(root, 'feature')
        unexpected = root / 'unexpected'
        with mock.patch.object(services, 'directory_links_under', return_value=[unexpected]):
            with pytest.raises(services.WorktreeToolError, match='unexpected directory links'):
                services.validate_directory_links(worktree)

    def test_remove_accepts_shared_vscode_link(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        worktree = services.Worktree(root, 'feature')
        vscode = root / '.vscode'
        with mock.patch.object(services, 'directory_links_under', return_value=[vscode]), mock.patch.object(services, 'is_link_like', side_effect=lambda path: path == vscode):
            assert services.validate_directory_links(worktree) == [vscode]

    def test_remove_detaches_shared_links_before_git_removal(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        main = root / 'main'
        linked = root / 'feature'
        main.mkdir()
        linked.mkdir()
        shared_link = linked / '.venv'
        args = argparse.Namespace(path=str(linked), force=False, dry_run=False)
        worktrees = [services.Worktree(main, 'main'), services.Worktree(linked, 'feature')]
        detached = services.DetachedLink(shared_link, main / '.venv', 'junction')
        git_result = subprocess.CompletedProcess([], 0, '', '')
        events: list[str] = []

        def detach(path: Path) -> services.DetachedLink:
            events.append(f'detach:{path.name}')
            return detached

        def run_git(arguments: list[str], **_: object) -> subprocess.CompletedProcess[str]:
            events.append(f"git:{' '.join(arguments)}")
            return git_result
        with mock.patch.object(services, 'get_worktrees', return_value=worktrees), mock.patch.object(services, 'require_clean_worktree'), mock.patch.object(services, 'validate_directory_links', return_value=[shared_link]), mock.patch.object(services, 'detach_link', side_effect=detach), mock.patch.object(services, 'git_command', side_effect=run_git):
            services.remove_worktree(args)
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
        worktrees = [services.Worktree(main, 'main'), services.Worktree(linked, 'feature')]
        detached = services.DetachedLink(shared_link, main / '.venv', 'junction')
        git_result = subprocess.CompletedProcess([], 1, '', 'locked')
        with mock.patch.object(services, 'get_worktrees', return_value=worktrees), mock.patch.object(services, 'require_clean_worktree'), mock.patch.object(services, 'validate_directory_links', return_value=[shared_link]), mock.patch.object(services, 'detach_link', return_value=detached), mock.patch.object(services, 'git_command', return_value=git_result), mock.patch.object(services, 'restore_link') as restore:
            with pytest.raises(services.WorktreeToolError, match='Removing Git worktree'):
                services.remove_worktree(args)
        restore.assert_called_once_with(detached)

    @pytest.mark.skipif(os.name != 'nt', reason='requires Windows directory junctions')
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
        detached = services.detach_link(link)
        assert detached.kind == 'junction'
        assert not link.exists()
        assert marker.read_text(encoding='utf-8') == 'preserved'

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
        assert not hasattr(services, 'create_parser')

class TestSetupPreflight:

    def test_windows_long_paths_policy_error_is_actionable(self) -> None:
        with mock.patch.object(preflight, 'read_windows_long_paths_enabled', return_value=False):
            error = preflight.check_windows_long_paths()
        assert 'LongPathsEnabled' in (error or '')
        assert 'Enable Win32 long paths' in (error or '')
        assert 'never changes machine policy' in (error or '')

    def test_windows_long_paths_policy_accepts_enabled_host(self) -> None:
        with mock.patch.object(preflight, 'read_windows_long_paths_enabled', return_value=True):
            assert preflight.check_windows_long_paths() is None

    def test_cmake_minimum_version_is_checked(
        self,
        tmp_path_factory: pytest.TempPathFactory,
    ) -> None:
        root = Path(tmp_path_factory.mktemp('case'))
        completed = subprocess.CompletedProcess(['cmake', '--version'], 0, 'cmake version 3.23.5\n', '')
        with mock.patch.object(preflight, 'command_path', return_value='cmake'), mock.patch.object(preflight.subprocess, 'run', return_value=completed):
            error = preflight.check_cmake(root)
        assert 'requires 3.24 or newer' in (error or '')

    def test_cmake_lookup_passes_case_insensitive_environment_path(self) -> None:
        with mock.patch.object(preflight.shutil, 'which', return_value='cmake') as which:
            assert preflight.command_path('cmake', {'Path': 'custom-path'}) == 'cmake'
        which.assert_called_once_with('cmake', path='custom-path')

    def test_setup_interactively_confirms_automatic_toolchain(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        root = Path(tmp_path_factory.mktemp('case'))
        selection = preflight.ToolchainSelection('C:/cmake/bin/cmake.exe', root / 'VsDevCmd.bat', ('-arch=x64',), {'PATH': 'ready'})
        with mock.patch.object(setup, 'resolve_toolchain', return_value=selection), mock.patch('builtins.input', return_value=''):
            assert setup.select_setup_toolchain(root, interactive=True) == selection

    def test_setup_interactively_accepts_manual_toolchain(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        root = Path(tmp_path_factory.mktemp('case'))
        selection = preflight.ToolchainSelection('C:/cmake/bin/cmake.exe', root / 'VsDevCmd.bat', ('-arch=x64',), {'PATH': 'ready'})
        with mock.patch.object(setup, 'resolve_toolchain', side_effect=[preflight.PreflightError('automatic detection failed'), selection]), mock.patch('builtins.input', side_effect=['C:/cmake/bin/cmake.exe', str(root / 'VsDevCmd.bat'), '-arch=x64']):
            assert setup.select_setup_toolchain(root, interactive=True) == selection

    def test_setup_non_interactive_reports_unusable_automatic_toolchain(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        root = Path(tmp_path_factory.mktemp('case'))
        with mock.patch.object(setup, 'resolve_toolchain', side_effect=preflight.PreflightError('automatic detection failed')), pytest.raises(dependencies.BootstrapError, match='--non-interactive'):
            setup.select_setup_toolchain(root, interactive=False)

    def test_toolchain_config_is_saved_without_overwriting_other_settings(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        root = Path(tmp_path_factory.mktemp('case'))
        config = root / '.agents' / 'DevTool.user.json'
        config.parent.mkdir()
        config.write_text(json.dumps({'version': 1, 'build': {'parallelJobs': 8}}), encoding='utf-8')
        script = root / 'VsDevCmd.bat'
        agent_config.save_toolchain_config(root, cmake_command='C:/cmake/bin/cmake.exe', environment_script=script, environment_arguments=['-arch=x64'])
        saved = json.loads(config.read_text(encoding='utf-8'))
        assert saved['build']['parallelJobs'] == 8
        assert saved['cmake']['command'] == 'C:/cmake/bin/cmake.exe'
        assert saved['toolchain']['environmentScript'] == script.as_posix()
        assert saved['toolchain']['environmentArguments'] == ['-arch=x64']

    def test_vswhere_lookup_uses_registry_when_program_files_environment_is_missing(
        self,
        tmp_path_factory: pytest.TempPathFactory,
    ) -> None:
        if os.name != 'nt':
            pytest.skip('Visual Studio discovery is Windows-only')
        directory = Path(tmp_path_factory.mktemp('case'))
        vswhere = directory / 'Microsoft Visual Studio' / 'Installer' / 'vswhere.exe'
        vswhere.parent.mkdir(parents=True)
        vswhere.touch()
        installation = directory / 'Visual Studio' / 'Enterprise'
        script = installation / 'Common7' / 'Tools' / 'VsDevCmd.bat'
        script.parent.mkdir(parents=True)
        script.touch()

        fake_winreg = mock.MagicMock(
            HKEY_LOCAL_MACHINE=object(),
            KEY_READ=1,
            KEY_WOW64_64KEY=2,
            KEY_WOW64_32KEY=4,
        )
        fake_winreg.OpenKey.return_value.__enter__.return_value = object()

        def query_value(_key: object, value_name: str) -> tuple[str, int]:
            if value_name == 'ProgramFilesDir (x86)':
                return str(directory), 1
            raise OSError

        fake_winreg.QueryValueEx.side_effect = query_value
        completed = subprocess.CompletedProcess([], 0, f'{installation}\n', '')
        with mock.patch.dict(os.sys.modules, {'winreg': fake_winreg}), mock.patch.object(toolchain.subprocess, 'run', return_value=completed):
            assert toolchain.find_vsdevcmd({}) == script

    def test_visual_studio_environment_override_is_loaded_from_agent_config(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        config = root / '.agents' / 'DevTool.user.json'
        config.parent.mkdir(parents=True)
        config.write_text(json.dumps({'version': 1, 'toolchain': {'environmentScript': str(root / 'toolchain/VsDevCmd.bat'), 'environmentArguments': ['-arch=x64', '-host_arch=x64']}}), encoding='utf-8')
        with mock.patch.object(preflight, 'REPO_ROOT', root):
            script, arguments = preflight.configured_visual_studio_environment()
        assert script == (root / 'toolchain/VsDevCmd.bat').resolve()
        assert arguments == ['-arch=x64', '-host_arch=x64']

    def test_vulkan_sdk_check_reports_every_required_file(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        error = preflight.check_vulkan_sdk({'VULKAN_SDK': directory})
        assert 'vulkan.h' in (error or '')
        assert 'vk_mem_alloc.h' in (error or '')
        assert 'vulkan-1.lib' in (error or '')

    def test_old_msvc_toolset_has_actionable_version_error(self) -> None:
        environment = {'PATH': 'tools', 'VCTOOLSVERSION': '14.43.34808'}
        with mock.patch.object(preflight, 'command_path', return_value='cl.exe'):
            error = preflight.check_msvc_version(environment)
        assert 'requires 14.44 or newer' in (error or '')
        assert 'std::format_string' in (error or '')

class TestAgentConfigLifecycle:

    def test_template_path_matches_repository_layout(self) -> None:
        repo_root = Path(__file__).resolve().parents[3]
        assert agent_config.template_path(repo_root).is_file()

    @staticmethod
    def create_repo(root: Path) -> None:
        template = root / agent_config.TEMPLATE_RELATIVE_PATH
        template.parent.mkdir(parents=True)
        template.write_text('{"version": 1}\n', encoding='utf-8')

    def test_initialize_is_idempotent(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        self.create_repo(root)
        target = agent_config.ensure_agent_config(root)
        target.write_text('local edit\n', encoding='utf-8')
        agent_config.ensure_agent_config(root)
        assert target.read_text(encoding='utf-8') == 'local edit\n'

    def test_dry_run_does_not_create_config(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        self.create_repo(root)
        target = agent_config.ensure_agent_config(root, dry_run=True)
        assert not target.exists()


class TestVSCodeConfigLifecycle:

    @staticmethod
    def create_repo(root: Path) -> None:
        template_directory = root / setup.VSCODE_TEMPLATE_DIRECTORY
        template_directory.mkdir(parents=True)
        for file_name in setup.VSCODE_TEMPLATE_FILES:
            (template_directory / file_name).write_text(
                f'template {file_name}\n',
                encoding='utf-8',
            )

    def test_initialize_copies_only_missing_files(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        self.create_repo(root)
        vscode = root / '.vscode'
        vscode.mkdir()
        settings = vscode / 'settings.json'
        settings.write_text('local settings\n', encoding='utf-8')

        launch = {'version': '0.2.0', 'configurations': [{'name': 'generated'}]}
        with mock.patch.object(setup, 'generate_vscode_launch_configuration', return_value=launch):
            assert setup.ensure_vscode_configuration(root) == vscode

        assert settings.read_text(encoding='utf-8') == 'local settings\n'
        assert json.loads((vscode / 'launch.json').read_text(encoding='utf-8')) == launch
        assert (vscode / 'extensions.json').read_text(encoding='utf-8') == 'template extensions.json\n'

    def test_existing_launch_configuration_is_not_generated_or_overwritten(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        self.create_repo(root)
        vscode = root / '.vscode'
        vscode.mkdir()
        launch = vscode / 'launch.json'
        launch.write_text('local launch\n', encoding='utf-8')
        with mock.patch.object(setup, 'generate_vscode_launch_configuration') as generate:
            setup.ensure_vscode_configuration(root)
        generate.assert_not_called()
        assert launch.read_text(encoding='utf-8') == 'local launch\n'

    def test_missing_template_does_not_create_target_directory(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        with pytest.raises(dependencies.BootstrapError, match='templates are missing'):
            setup.ensure_vscode_configuration(root)
        assert not (root / '.vscode').exists()

    def test_launch_configuration_covers_registered_windows_presets(self) -> None:
        with mock.patch.object(setup, 'load_local_config', return_value=build_config.LocalConfig()):
            launch = setup.generate_vscode_launch_configuration(
                REPOSITORY_ROOT,
                current_host='windows',
                environment={},
            )
        configurations = {
            item['name']: item
            for item in launch['configurations']
        }
        assert tuple(configurations) == (
            'Win64-Debug-DurinEditor',
            'Win64-Release-DurinEditor',
            'Win64-Release-DurinEditor-Profiling',
            'Win64-Debug-DurinGame',
            'Win64-Release-DurinGame',
            'Win64-Release-DurinGame-Profiling',
            'Win64-Shipping-DurinGame',
        )
        assert 'Win64-Debug-DurinEditor-FastConfigure' not in configurations
        assert configurations['Win64-Release-DurinEditor-Profiling']['program'] == (
            '${workspaceFolder}/Engine/Binaries/Win64/Release-Profiling/'
            'Runtime/DurinEditor/DurinEditor.exe'
        )
        assert configurations['Win64-Shipping-DurinGame']['program'] == (
            '${workspaceFolder}/Engine/Binaries/Win64/Shipping/'
            'Runtime/DurinGame/DurinGame.exe'
        )


class TestBootstrapRegistry:

    @pytest.fixture(autouse=True)
    def _setup_registry(self) -> None:
        self.registry = CommandRegistry()

    def test_bootstrap_commands_are_available_without_prepared_environment(self) -> None:
        commands = (['setup'], ['dependency', 'prepare', '--libs', 'tracy'], ['dependency', 'validate'], ['worktree', 'open', '--dry-run'], ['worktree', 'list'], ['worktree', 'add', 'feature'], ['worktree', 'prepare'], ['worktree', 'remove', 'feature'])
        for arguments in commands:
            specification, _ = self.registry.parse(arguments)
            assert specification.capability is Capability.BOOTSTRAP

    def test_setup_accepts_non_interactive_mode(self) -> None:
        specification, namespace = self.registry.parse(['setup', '--non-interactive'])
        assert specification.name == 'setup'
        assert namespace.non_interactive

    def test_dependency_prepare_requires_exactly_one_selection_mode(self) -> None:
        for arguments in (['dependency', 'prepare'], ['dependency', 'prepare', '--all', '--libs', 'tracy']):
            specification, namespace = self.registry.parse(arguments)
            with pytest.raises(Exception, match='exactly one'):
                self.registry.execute(specification, namespace, repository_root=REPOSITORY_ROOT, stdout=io.StringIO(), stderr=io.StringIO())

    def test_worktree_add_rejects_conflicting_modes(self) -> None:
        specification, namespace = self.registry.parse(['worktree', 'add', 'feature', '--branch', 'topic', '--detach'])
        with pytest.raises(Exception, match='cannot be used together'):
            self.registry.execute(specification, namespace, repository_root=REPOSITORY_ROOT, stdout=io.StringIO(), stderr=io.StringIO())

class TestSetupOrchestration:

    def test_setup_fallback_styles_tty_output_without_third_party_packages(self) -> None:
        class TtyBuffer(io.StringIO):
            def isatty(self) -> bool:
                return True

        stream = TtyBuffer()
        with mock.patch.dict(os.environ, {}, clear=True), mock.patch.object(
            handler, '_enable_virtual_terminal', return_value=True
        ):
            output = handler._BootstrapOutput(stream, plain=False)
            print('Durin setup completed successfully.', file=output)
        assert '\x1b[1;32m' in stream.getvalue()

    def test_setup_fallback_respects_plain_output(self) -> None:
        stream = io.StringIO()
        output = handler._BootstrapOutput(stream, plain=True)
        print('Durin setup completed successfully.', file=output)
        assert '\x1b[' not in stream.getvalue()

    def test_preflight_runs_before_every_repository_mutation(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        events: list[str] = []
        python = root / '.venv' / 'Scripts' / 'python.exe'
        selection = preflight.ToolchainSelection('cmake.exe', root / 'VsDevCmd.bat', ('x64',), {'PATH': 'ready'})
        with mock.patch.object(setup, 'select_setup_toolchain', return_value=selection), mock.patch.object(setup, 'validate_prerequisites', side_effect=lambda _, **__: events.append('preflight')), mock.patch.object(setup, 'ensure_agent_config', side_effect=lambda _: events.append('config')), mock.patch.object(setup, 'save_toolchain_config', side_effect=lambda *_args, **_kwargs: events.append('toolchain')), mock.patch.object(setup, 'ensure_vscode_configuration', side_effect=lambda _: events.append('vscode')), mock.patch.object(setup, 'ensure_python_environment', side_effect=lambda _: events.append('python') or python), mock.patch.object(setup, 'prepare_dependencies', side_effect=lambda *_args, **_kwargs: events.append('dependencies')):
            assert setup.setup_repository(root) == python
        assert events == ['preflight', 'config', 'toolchain', 'vscode', 'python', 'dependencies']

    def test_linked_worktree_setup_points_only_to_unified_prepare(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        (root / '.git').write_text('gitdir: elsewhere', encoding='utf-8')
        with pytest.raises(dependencies.BootstrapError, match='DevTool worktree prepare') as raised:
            setup.setup_repository(root)
        assert 'WorktreeTool' not in str(raised.value)

    def test_setup_uses_complete_dependency_selection(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        python = root / '.venv' / 'Scripts' / 'python.exe'
        selection = preflight.ToolchainSelection('cmake.exe', root / 'VsDevCmd.bat', ('x64',), {'PATH': 'ready'})
        with mock.patch.object(setup, 'select_setup_toolchain', return_value=selection), mock.patch.object(setup, 'validate_prerequisites'), mock.patch.object(setup, 'ensure_agent_config'), mock.patch.object(setup, 'save_toolchain_config'), mock.patch.object(setup, 'ensure_vscode_configuration'), mock.patch.object(setup, 'ensure_python_environment', return_value=python), mock.patch.object(setup, 'prepare_dependencies') as prepare:
            setup.setup_repository(root)
        request = prepare.call_args.args[1]
        assert request.use_all
        assert request.with_tests
        assert request.with_development
        assert request.cmake_command == 'cmake.exe'
        assert prepare.call_args.kwargs['environment'] == {'PATH': 'ready'}

    def test_worktree_preparation_uses_shared_python_preflight(self) -> None:
        target = Path('C:/repo-feature')
        with mock.patch.object(services, 'validate_prerequisites') as validate:
            services.run_preflight(target)
        validate.assert_called_once_with(target)

    def test_worktree_preflight_preserves_unexpected_runtime_error(self) -> None:
        failure = RuntimeError('unexpected defect')
        with mock.patch.object(services, 'validate_prerequisites', side_effect=failure), pytest.raises(RuntimeError) as raised:
            services.run_preflight(Path('C:/repo-feature'))
        assert raised.value is failure

    def test_expected_preflight_failure_is_reported_as_devtool_error(self) -> None:
        namespace = argparse.Namespace(bootstrap_action='setup', plain=True)
        failure = preflight.PreflightError('missing prerequisite')
        with mock.patch.object(handler, 'setup_repository', side_effect=failure), pytest.raises(handler.DevToolError, match='missing prerequisite') as raised:
            handler.run(
                namespace,
                repository_root=REPOSITORY_ROOT,
                stdout=io.StringIO(),
                stderr=io.StringIO(),
            )
        assert raised.value.__cause__ is failure

    def test_unexpected_bootstrap_runtime_error_retains_identity_and_traceback(self) -> None:
        namespace = argparse.Namespace(bootstrap_action='setup', plain=True)
        failure = RuntimeError('unexpected defect')

        def fail_setup(_repository_root: Path, *, interactive: bool) -> Path:
            raise failure

        with mock.patch.object(handler, 'setup_repository', side_effect=fail_setup), pytest.raises(RuntimeError) as raised:
            handler.run(
                namespace,
                repository_root=REPOSITORY_ROOT,
                stdout=io.StringIO(),
                stderr=io.StringIO(),
            )
        assert raised.value is failure
        assert 'fail_setup' in {entry.name for entry in raised.traceback}

    def test_successful_system_python_setup_restarts_interactive_shell(self) -> None:
        namespace = argparse.Namespace(bootstrap_action='setup')
        session: dict[str, object] = {}
        prepared_python = Path(sys.executable).with_name('prepared-python.exe')
        completed = mock.Mock(returncode=0)
        with mock.patch.object(handler, 'setup_repository', return_value=prepared_python), mock.patch.object(handler.subprocess, 'run', return_value=completed) as run:
            result = handler.run(namespace, repository_root=REPOSITORY_ROOT, stdout=io.StringIO(), stderr=io.StringIO(), session_state=session)
        assert result == 0
        assert session['exit_requested']
        assert run.call_args.args[0][0] == str(prepared_python)
        assert run.call_args.args[0][-1] == 'shell'

class TestRelocatedManifest:

    def test_every_relocated_manifest_validates(self) -> None:
        manifests = dependencies.load_manifests()
        dependencies.validate_manifests(manifests)
        assert len(manifests) == 10

    def test_tracy_repair_command_is_focused_and_runnable(self) -> None:
        manifest = next((item for item in dependencies.load_manifests() if item['name'] == 'tracy-tools'))
        assert manifest['repair_command'] == 'DevTool.bat dependency prepare --libs tracy,tracy-tools'
        assert (REPOSITORY_ROOT / 'DevTool.bat').is_file()
