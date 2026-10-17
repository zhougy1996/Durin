import pytest
import json
import os
import subprocess
import sys
from pathlib import Path
from unittest import mock
REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
from durin_dev_tool.bootstrap import agent_config, application as bootstrap_application, handler, manifests as dependency_manifests, preflight, setup, toolchain_selection
from durin_dev_tool.bootstrap.models import BootstrapError
from durin_dev_tool.context import CommandIO, RepositoryContext
from durin_dev_tool import toolchain

REPOSITORY = RepositoryContext.load(REPOSITORY_ROOT)

class TestSetupPreflight:

    def test_prepare_reads_config_and_checks_each_tool_once(self, tmp_path) -> None:
        from durin_dev_tool.bootstrap import toolchain_setup
        config = tmp_path / '.agents/DevTool.user.json'
        config.parent.mkdir()
        script = tmp_path / 'VsDevCmd.bat'
        cmake = str(tmp_path / 'cmake.exe')
        config.write_text(json.dumps({'version': 1, 'cmake': {'command': cmake}, 'toolchain': {'environmentScript': str(script)}}), encoding='utf-8')
        with mock.patch.object(setup.sys, 'platform', 'win32'), mock.patch.object(setup, 'load_local_config', wraps=setup.load_local_config) as load, mock.patch.object(toolchain_selection, 'load_local_config', side_effect=AssertionError('configuration was read again')), mock.patch.object(toolchain_setup, 'capture_windows_environment', return_value={}) as capture, mock.patch.object(toolchain_setup, 'resolve_cmake_executable', return_value=cmake), mock.patch.object(toolchain_setup, 'check_cmake', return_value=None) as check_cmake, mock.patch.object(preflight, 'check_cmake', side_effect=AssertionError('CMake was checked again')), mock.patch.object(preflight, 'check_msvc_version', return_value=None) as check_msvc, mock.patch.object(preflight, 'find_ninja', return_value='ninja') as ninja, mock.patch.object(preflight, 'find_command', return_value='git'), mock.patch.object(preflight, 'check_windows_long_paths', return_value=None), mock.patch.object(preflight, 'check_vulkan_sdk', return_value=None):
            selected = setup.prepare_setup(REPOSITORY.at_root(tmp_path), CommandIO.system(), interactive=False)
        assert selected.cmake_command == cmake
        for probe in (load, capture, check_cmake, check_msvc, ninja):
            probe.assert_called_once()

    def test_independent_preflight_still_checks_the_complete_toolchain(self) -> None:
        selection = toolchain_selection.ToolchainSelection('cmake', None, (), {})
        with mock.patch.object(preflight, 'find_command', return_value='git'), mock.patch.object(preflight, 'check_windows_long_paths', return_value=None), mock.patch.object(preflight, 'check_cmake', return_value='bad CMake') as cmake, mock.patch.object(preflight, 'find_ninja', return_value=None), mock.patch.object(preflight, 'check_msvc_version', return_value='bad MSVC'), mock.patch.object(preflight, 'check_vulkan_sdk', return_value=None):
            errors = preflight.collect_errors(REPOSITORY_ROOT, selection=selection, current_platform='win32')
        cmake.assert_called_once()
        assert 'bad CMake' in errors and 'bad MSVC' in errors
        assert any('Ninja' in error for error in errors)

    def test_standalone_resolution_reuses_the_parsed_config(self) -> None:
        from durin_dev_tool.build.models import LocalConfig
        with mock.patch.object(toolchain_selection.sys, 'platform', 'darwin'), mock.patch.dict(os.environ, {}, clear=True), mock.patch.object(toolchain_selection, 'load_local_config', return_value=LocalConfig()) as load, mock.patch.object(toolchain_selection, 'resolve_cmake_executable', return_value='/usr/local/bin/cmake'), mock.patch.object(toolchain_selection, 'toolchain_errors', return_value=[]):
            selected = toolchain_selection.resolve_toolchain(repository_context=REPOSITORY)
        load.assert_called_once()
        assert selected.cmake_command == '/usr/local/bin/cmake'
        assert selected.environment_script is None

    def test_toolchain_checks_cannot_be_skipped_without_a_selection(self) -> None:
        with pytest.raises(ValueError, match='checked toolchain selection'):
            preflight.collect_errors(REPOSITORY_ROOT, toolchain_checked=True)

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
        with mock.patch.object(preflight, 'find_command', return_value='cmake'), mock.patch.object(preflight.subprocess, 'run', return_value=completed):
            error = preflight.check_cmake(root)
        assert 'requires 3.24 or newer' in (error or '')

    def test_cmake_lookup_passes_case_insensitive_environment_path(self) -> None:
        with mock.patch.object(toolchain.shutil, 'which', return_value='cmake') as which:
            assert toolchain.find_command('cmake', {'Path': 'custom-path'}) == 'cmake'
        which.assert_called_once_with('cmake', path='custom-path')

    def test_macos_setup_automatically_uses_valid_toolchain(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        root = Path(tmp_path_factory.mktemp('case'))
        selection = toolchain_selection.ToolchainSelection('C:/cmake/bin/cmake.exe', root / 'VsDevCmd.bat', ('-arch=x64',), {'PATH': 'ready'})
        with mock.patch.object(setup.sys, 'platform', 'darwin'), mock.patch.object(setup, 'resolve_toolchain', return_value=selection), mock.patch('builtins.input', side_effect=AssertionError('must not prompt')):
            assert setup.select_setup_toolchain(root, interactive=True) == selection

    def test_macos_setup_interactively_accepts_manual_toolchain(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        root = Path(tmp_path_factory.mktemp('case'))
        selection = toolchain_selection.ToolchainSelection('C:/cmake/bin/cmake.exe', root / 'VsDevCmd.bat', ('-arch=x64',), {'PATH': 'ready'})
        with mock.patch.object(setup.sys, 'platform', 'darwin'), mock.patch.object(setup, 'resolve_toolchain', side_effect=[preflight.PreflightError('automatic detection failed'), selection]), mock.patch('builtins.input', side_effect=['C:/cmake/bin/cmake.exe', str(root / 'VsDevCmd.bat'), '-arch=x64']):
            assert setup.select_setup_toolchain(root, interactive=True) == selection

    def test_setup_non_interactive_reports_unusable_automatic_toolchain(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        root = Path(tmp_path_factory.mktemp('case'))
        with mock.patch.object(setup.sys, 'platform', 'darwin'), mock.patch.object(setup, 'resolve_toolchain', side_effect=preflight.PreflightError('automatic detection failed')), pytest.raises(BootstrapError, match='--non-interactive'):
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

    def test_inherited_toolchain_config_saves_null_environment_script(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        root = Path(tmp_path_factory.mktemp('case'))
        config = root / '.agents' / 'DevTool.user.json'
        config.parent.mkdir()
        config.write_text(json.dumps({'version': 1}), encoding='utf-8')
        agent_config.save_toolchain_config(
            root,
            cmake_command='/opt/homebrew/bin/cmake',
            environment_script=None,
            environment_arguments=[],
        )
        saved = json.loads(config.read_text(encoding='utf-8'))
        assert saved['toolchain']['environmentScript'] is None
        assert saved['toolchain']['environmentArguments'] == []

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
        script, arguments = toolchain_selection.configured_visual_studio_environment(root)
        assert script == (root / 'toolchain/VsDevCmd.bat').resolve()
        assert arguments == ['-arch=x64', '-host_arch=x64']

    def test_vulkan_sdk_check_reports_every_required_file(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        error = preflight.check_vulkan_sdk(
            {'VULKAN_SDK': directory}, current_platform='win32'
        )
        assert 'vulkan.h' in (error or '')
        assert 'vk_mem_alloc.h' in (error or '')
        assert 'vulkan-1.lib' in (error or '')

    def test_macos_vulkan_sdk_requires_headers_loader_and_moltenvk(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        root = Path(tmp_path_factory.mktemp('case'))
        error = preflight.check_vulkan_sdk(
            {'VULKAN_SDK': str(root)}, current_platform='darwin'
        )
        assert 'include/vulkan/vulkan.h' in (error or '')
        assert 'include/vma/vk_mem_alloc.h' in (error or '')
        assert 'lib/libvulkan.dylib' in (error or '')
        assert 'lib/libMoltenVK.dylib' in (error or '')
        assert 'share/vulkan/icd.d/MoltenVK_icd.json' in (error or '')

        for relative in (
            'include/vulkan/vulkan.h',
            'include/vma/vk_mem_alloc.h',
            'lib/libvulkan.dylib',
            'lib/libMoltenVK.dylib',
            'share/vulkan/icd.d/MoltenVK_icd.json',
        ):
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.touch()
        assert preflight.check_vulkan_sdk(
            {'VULKAN_SDK': str(root)}, current_platform='darwin'
        ) is None

    def test_macos_toolchain_inherits_environment_without_setup_script(self) -> None:
        environment = {'PATH': '/toolchain/bin', 'VULKAN_SDK': '/sdk'}
        with mock.patch.object(toolchain_selection.sys, 'platform', 'darwin'), mock.patch.dict(
            toolchain_selection.os.environ, environment, clear=True
        ), mock.patch.object(
            toolchain_selection,
            'configured_visual_studio_environment',
            return_value=(None, []),
        ), mock.patch.object(
            toolchain_selection, 'resolve_cmake_executable', return_value='/toolchain/bin/cmake'
        ):
            selection = toolchain_selection.select_toolchain(REPOSITORY_ROOT)
        assert selection.environment_script is None
        assert selection.environment_arguments == ()
        assert selection.environment == environment

    def test_macos_toolchain_detects_vulkan_setup_script(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        root = Path(tmp_path_factory.mktemp('case'))
        sdk = root / 'macOS'
        sdk.mkdir()
        script = root / 'setup-env.sh'
        script.touch()
        assert toolchain_selection.find_macos_vulkan_environment_script(
            {'VULKAN_SDK': str(sdk)}
        ) == script

    def test_macos_architecture_rejects_non_arm64(self) -> None:
        assert preflight.check_macos_architecture('arm64') is None
        assert 'Apple Silicon arm64' in (preflight.check_macos_architecture('x86_64') or '')

    def test_old_msvc_toolset_has_actionable_version_error(self) -> None:
        environment = {'PATH': 'tools', 'VCTOOLSVERSION': '14.43.34808'}
        with mock.patch.object(preflight, 'find_command', return_value='cl.exe'):
            error = preflight.check_msvc_version(environment)
        assert 'requires 14.44 or newer' in (error or '')
        assert 'std::format_string' in (error or '')
