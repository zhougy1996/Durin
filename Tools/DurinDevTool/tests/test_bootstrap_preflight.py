import pytest
import json
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
from durin_dev_tool.bootstrap.models import BootstrapError
from durin_dev_tool.context import CommandIO, RepositoryContext
from durin_dev_tool import toolchain

REPOSITORY = RepositoryContext.load(REPOSITORY_ROOT)

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
        with mock.patch.object(preflight, 'find_command', return_value='cmake'), mock.patch.object(preflight.subprocess, 'run', return_value=completed):
            error = preflight.check_cmake(root)
        assert 'requires 3.24 or newer' in (error or '')

    def test_cmake_lookup_passes_case_insensitive_environment_path(self) -> None:
        with mock.patch.object(toolchain.shutil, 'which', return_value='cmake') as which:
            assert toolchain.find_command('cmake', {'Path': 'custom-path'}) == 'cmake'
        which.assert_called_once_with('cmake', path='custom-path')

    def test_setup_interactively_confirms_automatic_toolchain(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        root = Path(tmp_path_factory.mktemp('case'))
        selection = toolchain_selection.ToolchainSelection('C:/cmake/bin/cmake.exe', root / 'VsDevCmd.bat', ('-arch=x64',), {'PATH': 'ready'})
        with mock.patch.object(setup, 'resolve_toolchain', return_value=selection), mock.patch('builtins.input', return_value=''):
            assert setup.select_setup_toolchain(root, interactive=True) == selection

    def test_setup_interactively_accepts_manual_toolchain(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        root = Path(tmp_path_factory.mktemp('case'))
        selection = toolchain_selection.ToolchainSelection('C:/cmake/bin/cmake.exe', root / 'VsDevCmd.bat', ('-arch=x64',), {'PATH': 'ready'})
        with mock.patch.object(setup, 'resolve_toolchain', side_effect=[preflight.PreflightError('automatic detection failed'), selection]), mock.patch('builtins.input', side_effect=['C:/cmake/bin/cmake.exe', str(root / 'VsDevCmd.bat'), '-arch=x64']):
            assert setup.select_setup_toolchain(root, interactive=True) == selection

    def test_setup_non_interactive_reports_unusable_automatic_toolchain(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        root = Path(tmp_path_factory.mktemp('case'))
        with mock.patch.object(setup, 'resolve_toolchain', side_effect=preflight.PreflightError('automatic detection failed')), pytest.raises(BootstrapError, match='--non-interactive'):
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
        script, arguments = toolchain_selection.configured_visual_studio_environment(root)
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
        with mock.patch.object(preflight, 'find_command', return_value='cl.exe'):
            error = preflight.check_msvc_version(environment)
        assert 'requires 14.44 or newer' in (error or '')
        assert 'std::format_string' in (error or '')

