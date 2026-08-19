from . import build_request_fixtures as request_fixtures
import pytest
import os
from dataclasses import replace
from pathlib import Path
from unittest import mock
from durin_dev_tool.build import config as build_config
from durin_dev_tool.build import core as build_core
from durin_dev_tool.build import toolchain_context as build_toolchain_context
from durin_dev_tool.build import build_context
from durin_dev_tool.toolchain import parse_environment_output


class TestCore:
    make_profile = staticmethod(request_fixtures.make_profile)
    make_preset = staticmethod(request_fixtures.make_preset)

    def test_buildtool_rejects_missing_windows_long_paths_policy(self) -> None:
        fake_winreg = mock.MagicMock(HKEY_LOCAL_MACHINE=object(), REG_DWORD=4)
        fake_winreg.OpenKey.return_value.__enter__.return_value = object()
        fake_winreg.QueryValueEx.return_value = (0, fake_winreg.REG_DWORD)
        with mock.patch.object(build_toolchain_context.os, 'name', 'nt'), mock.patch.dict(os.sys.modules, {'winreg': fake_winreg}), pytest.raises(build_config.BuildToolError, match='LongPathsEnabled'):
            build_toolchain_context.require_windows_long_paths_enabled()
    def test_buildtool_accepts_enabled_windows_long_paths_policy(self) -> None:
        fake_winreg = mock.MagicMock(HKEY_LOCAL_MACHINE=object(), REG_DWORD=4)
        fake_winreg.OpenKey.return_value.__enter__.return_value = object()
        fake_winreg.QueryValueEx.return_value = (1, fake_winreg.REG_DWORD)
        with mock.patch.object(build_toolchain_context.os, 'name', 'nt'), mock.patch.dict(os.sys.modules, {'winreg': fake_winreg}):
            build_toolchain_context.require_windows_long_paths_enabled()
    def test_run_project_is_normalized_and_validated_without_toolchain_state(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        descriptor = root / 'Games' / '示例 Project' / 'Example.dproject'
        descriptor.parent.mkdir(parents=True)
        descriptor.write_text('{}', encoding='utf-8')
        request = request_fixtures.command_request(build_config.Action.RUN, options=request_fixtures.RunActionOptions(project_path=Path('Games') / '示例 Project' / 'Example.dproject', arguments=('--hidden-window', 'argument with spaces')))
        normalized = build_core.normalize_run_request(request, root=root)
        assert normalized.project_path == descriptor.resolve()
        assert normalized.run_arguments == request.run_arguments
    def test_run_defaults_durin_game_to_sandbox_project(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        descriptor = root / 'Sandbox' / 'Sandbox.dproject'
        descriptor.parent.mkdir(parents=True)
        descriptor.write_text('{}', encoding='utf-8')
        request = request_fixtures.command_request(build_config.Action.RUN, options=request_fixtures.RunActionOptions())
        normalized = build_core.normalize_run_request(request, preset=self.make_preset(runtime_variant='DurinGame'), root=root)
        assert normalized.project_path == descriptor.resolve()
    def test_run_does_not_default_editor_or_override_raw_project_selector(self) -> None:
        request = request_fixtures.command_request(build_config.Action.RUN, options=request_fixtures.RunActionOptions())
        editor_request = build_core.normalize_run_request(request, preset=self.make_preset(runtime_variant='DurinEditor'))
        raw_game_request = request_fixtures.command_request(build_config.Action.RUN, options=request_fixtures.RunActionOptions(arguments=('--project=Other.dproject',)))
        normalized_raw_game_request = build_core.normalize_run_request(raw_game_request, preset=self.make_preset(runtime_variant='DurinGame'))
        assert editor_request.project_path is None
        assert normalized_raw_game_request.project_path is None
        assert normalized_raw_game_request.run_arguments == raw_game_request.run_arguments
    def test_run_project_rejects_missing_and_wrong_extension_descriptors(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        wrong_extension = request_fixtures.command_request(build_config.Action.RUN, options=request_fixtures.RunActionOptions(project_path=Path('Example.json')))
        with pytest.raises(build_config.BuildToolError, match='\\.dproject extension'):
            build_core.normalize_run_request(wrong_extension, root=root)
        missing = request_fixtures.command_request(build_config.Action.RUN, options=request_fixtures.RunActionOptions(project_path=Path('Missing.dproject')))
        with pytest.raises(build_config.BuildToolError, match='was not found'):
            build_core.normalize_run_request(missing, root=root)
    def test_run_project_rejects_conflicting_runtime_project_selectors(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        descriptor = Path(directory) / 'Example.dproject'
        descriptor.write_text('{}', encoding='utf-8')
        for arguments in (('--project', 'Other.dproject'), ('--project=Other.dproject',)):
            with pytest.raises(build_config.BuildToolError, match='either through --project or through --args'):
                build_core.normalize_run_request(request_fixtures.command_request(build_config.Action.RUN, options=request_fixtures.RunActionOptions(project_path=descriptor, arguments=arguments)))
    def test_environment_output_collapses_windows_case_duplicates(self) -> None:
        environment = parse_environment_output('PATH=developer\nPath=parent\n', case_insensitive=True)
        assert environment == {'PATH': 'developer'}
    def test_inherit_provider_preserves_environment(self) -> None:
        with mock.patch.dict(os.environ, {'DURIN_TEST_ENV': 'present'}, clear=True):
            environment = build_toolchain_context.build_environment(self.make_profile(), build_config.EnvironmentSetup(), current_host='windows')
        assert environment['DURIN_TEST_ENV'] == 'present'
    def test_visual_studio_environment_is_captured_once(self) -> None:
        profile = replace(self.make_profile(), environment_provider=build_config.EnvironmentProvider.VISUAL_STUDIO)
        with mock.patch.object(build_toolchain_context, 'load_visual_studio_environment_cache', return_value=None), mock.patch.object(build_toolchain_context, 'write_visual_studio_environment_cache'), mock.patch.object(build_toolchain_context, 'find_vsdevcmd', return_value=Path('VsDevCmd.bat')), mock.patch.object(build_toolchain_context, 'capture_setup_environment', return_value={'PATH': 'ready', 'VSLANG': '2052'}) as capture, mock.patch.object(build_toolchain_context, 'detect_msvc_showincludes_prefix', return_value='Note: including file:  ') as detect_prefix:
            environment = build_toolchain_context.build_environment(profile, build_config.EnvironmentSetup(), current_host='windows')
        assert environment['PATH'] == 'ready'
        assert environment['VSLANG'] == '1033'
        capture.assert_called_once()
        detect_prefix.assert_called_once_with(environment)
    def test_visual_studio_environment_cache_reuses_delta_and_invalidates_for_compiler_change(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        profile = replace(self.make_profile(), environment_provider=build_config.EnvironmentProvider.VISUAL_STUDIO)
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        script = root / 'VsDevCmd.bat'
        compiler = root / 'cl.exe'
        cache = root / 'environment.json'
        script.touch()
        compiler.touch()
        captured = {'PATH': str(root) + os.pathsep + 'original-path', 'VSLANG': '1033', 'VSINSTALLDIR': str(root), 'DURIN_LIVE_VALUE': 'first'}
        with mock.patch.object(build_toolchain_context, 'find_vsdevcmd', return_value=script), mock.patch.object(build_toolchain_context, 'visual_studio_environment_cache_path', return_value=cache), mock.patch.object(build_toolchain_context, 'capture_setup_environment', return_value=captured) as capture, mock.patch.object(build_toolchain_context, 'detect_msvc_showincludes_prefix', return_value='Note: including file:  ') as detect_prefix, mock.patch.object(build_toolchain_context, 'find_command', return_value=str(compiler)), mock.patch.dict(os.environ, {'DURIN_LIVE_VALUE': 'first', 'PATH': 'original-path'}, clear=True):
            first = build_toolchain_context.build_environment(profile, build_config.EnvironmentSetup(), current_host='windows')
            os.environ['DURIN_LIVE_VALUE'] = 'second'
            os.environ['PATH'] = 'new-path'
            second = build_toolchain_context.build_environment(profile, build_config.EnvironmentSetup(), current_host='windows')
            compiler.write_text('updated', encoding='utf-8')
            build_toolchain_context.build_environment(profile, build_config.EnvironmentSetup(), current_host='windows')
        assert first['PATH'] == str(root) + os.pathsep + 'original-path'
        assert second['DURIN_LIVE_VALUE'] == 'second'
        assert second['PATH'] == str(root) + os.pathsep + 'new-path'
        assert capture.call_count == 2
        assert detect_prefix.call_count == 2
    def test_visual_studio_environment_rejects_localized_compiler_output(self) -> None:
        profile = replace(self.make_profile(), environment_provider=build_config.EnvironmentProvider.VISUAL_STUDIO)
        with mock.patch.object(build_toolchain_context, 'load_visual_studio_environment_cache', return_value=None), mock.patch.object(build_toolchain_context, 'find_vsdevcmd', return_value=Path('VsDevCmd.bat')), mock.patch.object(build_toolchain_context, 'capture_setup_environment', return_value={'PATH': 'ready'}), mock.patch.object(build_toolchain_context, 'detect_msvc_showincludes_prefix', return_value='注意: 包含文件:  '), pytest.raises(build_config.BuildToolError, match='English language pack'):
            build_toolchain_context.build_environment(profile, build_config.EnvironmentSetup(), current_host='windows')
    def test_windows_setup_script_is_passed_as_separate_argument(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        script = Path(directory) / 'VS Tools' / 'VsDevCmd.bat'
        script.parent.mkdir()
        script.touch()
        completed = mock.Mock(returncode=0, stdout='DURIN_ENV=ready\n', stderr='')
        with mock.patch.object(build_toolchain_context.subprocess, 'run', return_value=completed) as run:
            environment = build_toolchain_context.capture_setup_environment(script, ['-arch=x64'], current_host='windows', cwd=directory)
        command = run.call_args.args[0]
        assert command[4:7] == ['call', str(script), '-arch=x64']
        assert environment['DURIN_ENV'] == 'ready'
    def test_visual_studio_profile_adds_bundled_ninja(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        ninja = root / 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
        ninja.parent.mkdir(parents=True)
        ninja.touch()
        profile = replace(self.make_profile(), environment_provider=build_config.EnvironmentProvider.VISUAL_STUDIO, required_commands=('ninja',))
        environment = {'Path': 'original', 'VSINSTALLDIR': str(root)}
        with mock.patch.object(build_toolchain_context, 'find_command', return_value=None):
            build_toolchain_context.ensure_required_commands(profile, environment)
        assert environment['Path'].startswith(str(ninja.parent))
    def test_derive_context_reuses_toolchain_environment(self) -> None:
        profile = self.make_profile()
        presets = {'debug': self.make_preset(), 'release': self.make_preset('release')}
        request = request_fixtures.command_request(build_config.Action.SHELL, context=build_config.RequestContext(preset='debug'))
        environment = {'PATH': 'cached'}
        context = build_config.BuildContext(request, build_config.LocalConfig(), profile, presets, presets['debug'], 'windows', cmake='cmake', jobs=8, environment=environment)
        child = build_context.derive_build_context(context, request_fixtures.command_request(build_config.Action.BUILD, context=build_config.RequestContext(preset='release'), options=request_fixtures.BuildActionOptions(target='all')))
        assert child.environment is environment
        assert child.preset.name == 'release'
