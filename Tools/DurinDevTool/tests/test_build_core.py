from __future__ import annotations
import pytest
import argparse
import io
import json
import os
import shutil
import subprocess
import zipfile
from dataclasses import replace
from pathlib import Path
from unittest import mock
REPO_ROOT = Path(__file__).resolve().parents[3]
DEV_TOOL_DIR = REPO_ROOT / 'Tools' / 'DurinDevTool'
if str(DEV_TOOL_DIR) not in os.sys.path:
    os.sys.path.insert(0, str(DEV_TOOL_DIR))
from durin_dev_tool.build import operations as build_cli
from durin_dev_tool.build import config as build_config
from durin_dev_tool.build import core as build_core
from durin_dev_tool.build import descriptors as build_descriptors
from durin_dev_tool.build import locking as build_locking
from durin_dev_tool.build import process as build_process
from durin_dev_tool.build import runtime as build_runtime
from durin_dev_tool.build import scaffolding as build_scaffolding
from durin_dev_tool.build.handler import request_from_namespace
from durin_dev_tool.build.output import BuildOutput
from durin_dev_tool.registry import CommandRegistry

def parse_build_request(arguments: list[str]) -> build_config.CommandRequest:
    _spec, namespace = CommandRegistry().parse(arguments)
    if getattr(namespace, 'selected_preset', ''):
        namespace.preset = namespace.selected_preset
    return request_from_namespace(namespace)

class TestCore:

    def test_buildtool_rejects_missing_windows_long_paths_policy(self) -> None:
        fake_winreg = mock.MagicMock(HKEY_LOCAL_MACHINE=object(), REG_DWORD=4)
        fake_winreg.OpenKey.return_value.__enter__.return_value = object()
        fake_winreg.QueryValueEx.return_value = (0, fake_winreg.REG_DWORD)
        with mock.patch.object(build_core.os, 'name', 'nt'), mock.patch.dict(os.sys.modules, {'winreg': fake_winreg}), pytest.raises(build_config.BuildToolError, match='LongPathsEnabled'):
            build_core.require_windows_long_paths_enabled()

    def test_buildtool_accepts_enabled_windows_long_paths_policy(self) -> None:
        fake_winreg = mock.MagicMock(HKEY_LOCAL_MACHINE=object(), REG_DWORD=4)
        fake_winreg.OpenKey.return_value.__enter__.return_value = object()
        fake_winreg.QueryValueEx.return_value = (1, fake_winreg.REG_DWORD)
        with mock.patch.object(build_core.os, 'name', 'nt'), mock.patch.dict(os.sys.modules, {'winreg': fake_winreg}):
            build_core.require_windows_long_paths_enabled()

    def make_profile(self) -> build_config.BuildProfile:
        return build_config.BuildProfile('test-profile', 'windows', 'debug', ('debug', 'release'), build_config.EnvironmentProvider.INHERIT, 'Win64', '.exe', True, ())

    def make_preset(self, name: str='debug', testing: str='ON', runtime_variant: str='DurinEditor') -> build_config.ConfigurePreset:
        return build_config.ConfigurePreset(name, {'name': name, 'binaryDir': '${sourceDir}/Build/${presetName}', 'cacheVariables': {'CMAKE_BUILD_TYPE': 'Debug', 'DURIN_RUNTIME_VARIANT': runtime_variant, 'BUILD_TESTING': testing}})

    def test_run_project_is_normalized_and_validated_without_toolchain_state(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        descriptor = root / 'Games' / '示例 Project' / 'Example.dproject'
        descriptor.parent.mkdir(parents=True)
        descriptor.write_text('{}', encoding='utf-8')
        request = build_config.CommandRequest(build_config.Action.RUN, options=build_config.RunActionOptions(project_path=Path('Games') / '示例 Project' / 'Example.dproject', arguments=('--hidden-window', 'argument with spaces')))
        normalized = build_core.normalize_run_request(request, root=root)
        assert normalized.project_path == descriptor.resolve()
        assert normalized.run_arguments == request.run_arguments

    def test_run_defaults_durin_game_to_sandbox_project(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        descriptor = root / 'Sandbox' / 'Sandbox.dproject'
        descriptor.parent.mkdir(parents=True)
        descriptor.write_text('{}', encoding='utf-8')
        request = build_config.CommandRequest(build_config.Action.RUN, options=build_config.RunActionOptions())
        normalized = build_core.normalize_run_request(request, preset=self.make_preset(runtime_variant='DurinGame'), root=root)
        assert normalized.project_path == descriptor.resolve()

    def test_run_does_not_default_editor_or_override_raw_project_selector(self) -> None:
        request = build_config.CommandRequest(build_config.Action.RUN, options=build_config.RunActionOptions())
        editor_request = build_core.normalize_run_request(request, preset=self.make_preset(runtime_variant='DurinEditor'))
        raw_game_request = build_config.CommandRequest(build_config.Action.RUN, options=build_config.RunActionOptions(arguments=('--project=Other.dproject',)))
        normalized_raw_game_request = build_core.normalize_run_request(raw_game_request, preset=self.make_preset(runtime_variant='DurinGame'))
        assert editor_request.project_path is None
        assert normalized_raw_game_request.project_path is None
        assert normalized_raw_game_request.run_arguments == raw_game_request.run_arguments

    def test_run_project_rejects_missing_and_wrong_extension_descriptors(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        wrong_extension = build_config.CommandRequest(build_config.Action.RUN, options=build_config.RunActionOptions(project_path=Path('Example.json')))
        with pytest.raises(build_config.BuildToolError, match='\\.dproject extension'):
            build_core.normalize_run_request(wrong_extension, root=root)
        missing = build_config.CommandRequest(build_config.Action.RUN, options=build_config.RunActionOptions(project_path=Path('Missing.dproject')))
        with pytest.raises(build_config.BuildToolError, match='was not found'):
            build_core.normalize_run_request(missing, root=root)

    def test_run_project_rejects_conflicting_runtime_project_selectors(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        descriptor = Path(directory) / 'Example.dproject'
        descriptor.write_text('{}', encoding='utf-8')
        for arguments in (('--project', 'Other.dproject'), ('--project=Other.dproject',)):
            with pytest.raises(build_config.BuildToolError, match='either through --project or through --args'):
                build_core.normalize_run_request(build_config.CommandRequest(build_config.Action.RUN, options=build_config.RunActionOptions(project_path=descriptor, arguments=arguments)))

    def test_environment_output_collapses_windows_case_duplicates(self) -> None:
        environment = build_core.parse_environment_output('PATH=developer\nPath=parent\n', case_insensitive=True)
        assert environment == {'PATH': 'developer'}

    def test_inherit_provider_preserves_environment(self) -> None:
        with mock.patch.dict(os.environ, {'DURIN_TEST_ENV': 'present'}, clear=True):
            environment = build_core.build_environment(self.make_profile(), build_config.EnvironmentSetup(), current_host='windows')
        assert environment['DURIN_TEST_ENV'] == 'present'

    def test_visual_studio_environment_is_captured_once(self) -> None:
        profile = replace(self.make_profile(), environment_provider=build_config.EnvironmentProvider.VISUAL_STUDIO)
        with mock.patch.object(build_core, 'load_visual_studio_environment_cache', return_value=None), mock.patch.object(build_core, 'write_visual_studio_environment_cache'), mock.patch.object(build_core, 'find_vsdevcmd', return_value=Path('VsDevCmd.bat')), mock.patch.object(build_core, 'capture_setup_environment', return_value={'PATH': 'ready', 'VSLANG': '2052'}) as capture, mock.patch.object(build_core, 'detect_msvc_showincludes_prefix', return_value='Note: including file:  ') as detect_prefix:
            environment = build_core.build_environment(profile, build_config.EnvironmentSetup(), current_host='windows')
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
        with mock.patch.object(build_core, 'find_vsdevcmd', return_value=script), mock.patch.object(build_core, 'visual_studio_environment_cache_path', return_value=cache), mock.patch.object(build_core, 'capture_setup_environment', return_value=captured) as capture, mock.patch.object(build_core, 'detect_msvc_showincludes_prefix', return_value='Note: including file:  ') as detect_prefix, mock.patch.object(build_core.shutil, 'which', return_value=str(compiler)), mock.patch.dict(os.environ, {'DURIN_LIVE_VALUE': 'first', 'PATH': 'original-path'}, clear=True):
            first = build_core.build_environment(profile, build_config.EnvironmentSetup(), current_host='windows')
            os.environ['DURIN_LIVE_VALUE'] = 'second'
            os.environ['PATH'] = 'new-path'
            second = build_core.build_environment(profile, build_config.EnvironmentSetup(), current_host='windows')
            compiler.write_text('updated', encoding='utf-8')
            build_core.build_environment(profile, build_config.EnvironmentSetup(), current_host='windows')
        assert first['PATH'] == str(root) + os.pathsep + 'original-path'
        assert second['DURIN_LIVE_VALUE'] == 'second'
        assert second['PATH'] == str(root) + os.pathsep + 'new-path'
        assert capture.call_count == 2
        assert detect_prefix.call_count == 2

    def test_visual_studio_environment_rejects_localized_compiler_output(self) -> None:
        profile = replace(self.make_profile(), environment_provider=build_config.EnvironmentProvider.VISUAL_STUDIO)
        with mock.patch.object(build_core, 'load_visual_studio_environment_cache', return_value=None), mock.patch.object(build_core, 'find_vsdevcmd', return_value=Path('VsDevCmd.bat')), mock.patch.object(build_core, 'capture_setup_environment', return_value={'PATH': 'ready'}), mock.patch.object(build_core, 'detect_msvc_showincludes_prefix', return_value='注意: 包含文件:  '), pytest.raises(build_config.BuildToolError, match='English language pack'):
            build_core.build_environment(profile, build_config.EnvironmentSetup(), current_host='windows')

    def test_windows_setup_script_is_passed_as_separate_argument(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        script = Path(directory) / 'VS Tools' / 'VsDevCmd.bat'
        script.parent.mkdir()
        script.touch()
        completed = mock.Mock(returncode=0, stdout='DURIN_ENV=ready\n', stderr='')
        with mock.patch.object(build_core.subprocess, 'run', return_value=completed) as run:
            environment = build_core.capture_setup_environment(script, ['-arch=x64'], current_host='windows')
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
        with mock.patch.object(build_core.shutil, 'which', return_value=None):
            build_core.ensure_required_commands(profile, environment)
        assert environment['Path'].startswith(str(ninja.parent))

    def test_derive_context_reuses_toolchain_environment(self) -> None:
        profile = self.make_profile()
        presets = {'debug': self.make_preset(), 'release': self.make_preset('release')}
        request = build_config.CommandRequest(build_config.Action.SHELL, context=build_config.RequestContext(preset='debug'))
        environment = {'PATH': 'cached'}
        context = build_config.BuildContext(request, build_config.LocalConfig(), profile, presets, presets['debug'], 'windows', cmake='cmake', jobs=8, environment=environment)
        child = build_core.derive_context(context, build_config.CommandRequest(build_config.Action.BUILD, context=build_config.RequestContext(preset='release'), options=build_config.BuildActionOptions(target='all')))
        assert child.environment is environment
        assert child.preset.name == 'release'

    def test_runtime_path_uses_runtime_variant_and_preset_role(self) -> None:
        preset = self.make_preset()
        values = dict(preset.values)
        cache = dict(values['cacheVariables'])
        cache['CMAKE_BUILD_TYPE'] = 'Release'
        cache['DURIN_PRESET_ROLE'] = 'Profiling'
        preset = build_config.ConfigurePreset('profiling', {**values, 'cacheVariables': cache})
        path = build_core.runtime_executable_path(self.make_profile(), preset, root=Path('repo'))
        assert path == Path('repo/Engine/Binaries/Win64/Release-Profiling/Runtime/DurinEditor/DurinEditor.exe')

    def test_run_application_reports_how_to_build_missing_runtime(self) -> None:
        preset = self.make_preset()
        request = build_config.CommandRequest(build_config.Action.RUN, options=build_config.RunActionOptions())
        context = build_config.BuildContext(request, build_config.LocalConfig(), self.make_profile(), {'debug': preset}, preset, 'windows')
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        with mock.patch.object(build_runtime, 'runtime_executable_path', return_value=Path('missing/DurinEditor.exe')), pytest.raises(build_config.BuildToolError, match='was not found'):
            build_core.run_application(context, output)

    def test_run_application_waits_for_relaunched_descendants(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        preset = self.make_preset()
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        project = root / '示例 Project' / 'Example.dproject'
        project.parent.mkdir()
        project.touch()
        context = build_config.BuildContext(build_config.CommandRequest(build_config.Action.RUN, options=build_config.RunActionOptions(project_path=project.resolve(), arguments=('--hidden-window', 'argument with spaces'))), build_config.LocalConfig(), self.make_profile(), {'debug': preset}, preset, 'windows')
        executable = root / 'DurinEditor.exe'
        executable.touch()
        with mock.patch.object(build_runtime, 'runtime_executable_path', return_value=executable), mock.patch.object(build_runtime, 'run_command') as run:
            build_core.run_application(context, output)
        assert run.call_args.args[0] == [str(executable), f'--project={project.resolve()}', '--hidden-window', 'argument with spaces']
        assert run.call_args.kwargs['wait_for_descendants']
        assert not run.call_args.kwargs['show_heartbeat']
        assert run.call_args.kwargs['colorize_log_levels']

    def test_run_command_waits_for_windows_process_job(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        process = mock.Mock(pid=42, returncode=0)
        process.stdout = io.StringIO()
        process.wait.return_value = 0
        process_job = mock.Mock()
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        directory = tmp_path_factory.mktemp('case')
        with mock.patch.object(build_process, 'command_log_path', return_value=Path(directory) / 'command.log'), mock.patch.object(build_process.subprocess, 'Popen', return_value=process), mock.patch.object(build_process, 'WindowsProcessJob', return_value=process_job):
            build_core.run_command(['DurinEditor.exe'], environment={}, output=output, wait_for_descendants=True)
        process_job.assign.assert_called_once_with(process)
        process_job.wait.assert_called_once_with()
        process_job.close.assert_called_once_with()

    def test_interrupt_terminates_relaunched_windows_process_job(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        process = mock.Mock(pid=42, returncode=0)
        process.stdout = io.StringIO()
        process.wait.return_value = 0
        process.poll.return_value = 0
        process_job = mock.Mock()
        process_job.wait.side_effect = KeyboardInterrupt
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        directory = tmp_path_factory.mktemp('case')
        with mock.patch.object(build_process, 'command_log_path', return_value=Path(directory) / 'command.log'), mock.patch.object(build_process.subprocess, 'Popen', return_value=process), mock.patch.object(build_process, 'WindowsProcessJob', return_value=process_job), pytest.raises(build_config.BuildToolError, match='Application run was interrupted'):
            build_core.run_command(['DurinEditor.exe'], environment={}, output=output, recovery_required_on_interrupt=False, wait_for_descendants=True)
        process_job.terminate.assert_called_once_with()
        process_job.close.assert_called_once_with()

    def test_test_action_rejects_non_test_preset(self) -> None:
        request = build_config.CommandRequest(build_config.Action.TEST, options=build_config.TestActionOptions(target='CoreTests'))
        with pytest.raises(build_config.BuildToolError, match='does not enable BUILD_TESTING'):
            build_core.validate_request(request, self.make_preset(testing='OFF'))

    def test_compact_native_test_enables_gtest_brief_output(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        preset = self.make_preset()
        context = build_config.BuildContext(build_config.CommandRequest(build_config.Action.TEST, options=build_config.TestActionOptions(target='CoreTests', filter='Core.*')), build_config.LocalConfig(), self.make_profile(), {'debug': preset}, preset, 'windows', environment={})
        output = BuildOutput(plain=True, output_mode=build_config.OutputMode.COMPACT, stdout=io.StringIO(), stderr=io.StringIO())
        directory = tmp_path_factory.mktemp('case')
        with mock.patch.object(build_runtime, 'test_executable_path', return_value=Path(directory) / 'CoreTests.exe') as executable_path, mock.patch.object(build_runtime, 'run_command') as run:
            executable_path.return_value.touch()
            build_core.run_native_test(context, output)
        assert run.call_args.args[0] == [str(executable_path.return_value), '--gtest_filter=Core.*', '--gtest_brief=1']
        assert run.call_args.kwargs['colorize_test_output']

    def test_all_native_tests_use_ctest_registration(self) -> None:
        preset = self.make_preset()
        context = build_config.BuildContext(build_config.CommandRequest(build_config.Action.TEST, options=build_config.TestActionOptions(target='ALL', timeout_seconds=60, schedule_random=True, output_junit=Path('Build/results.xml'), ctest_regex='^Core\\.', granularity=build_config.TestGranularity.CASE)), build_config.LocalConfig(), self.make_profile(), {'debug': preset}, preset, 'windows', cmake=r'C:\Tools\CMake\bin\cmake.exe', jobs=4, environment={'PATH': 'cached'})
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        build_directory = Path('Build/debug')
        with mock.patch.object(build_runtime, 'preset_build_directory', return_value=build_directory), mock.patch.object(build_runtime, 'run_command') as run:
            build_core.run_all_native_tests(context, output)
        run.assert_called_once_with(
            [r'C:\Tools\CMake\bin\ctest.exe', '--test-dir', str(build_directory), '--output-on-failure', '--no-tests=error', '-j', '4', '-L', 'native-test-case', '-LE', 'native-test-characterization', '--timeout', '60', '--schedule-random', '-R', '^Core\\.', '--output-junit', str(build_core.REPO_ROOT / 'Build/results.xml')],
            environment={'PATH': 'cached'},
            output=output,
            recovery_required_on_interrupt=False,
            interruption_message='Native test run was interrupted.',
            colorize_test_output=True,
            show_heartbeat=False,
        )

    def test_default_target_mode_treats_include_direct_as_noop(self) -> None:
        preset = self.make_preset()
        request = build_config.CommandRequest(
            build_config.Action.TEST,
            options=build_config.TestActionOptions(
                target='all',
                include_direct=True,
            ),
        )
        context = build_config.BuildContext(request, build_config.LocalConfig(), self.make_profile(), {'debug': preset}, preset, 'windows', cmake='cmake', jobs=4, environment={})
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        with mock.patch.object(build_runtime, 'run_command') as run:
            build_core.run_all_native_tests(context, output)
        assert run.call_count == 1
        assert run.call_args.args[0][-6:] == [
            '-L',
            'native-test-target',
            '-LE',
            'native-test-characterization',
            '--timeout',
            '300',
        ]

    def test_direct_lifecycle_phase_uses_companion_junit_output(self) -> None:
        preset = self.make_preset()
        request = build_config.CommandRequest(
            build_config.Action.TEST,
            options=build_config.TestActionOptions(
                target='all',
                include_direct=True,
                output_junit=Path('Build/results.xml'),
                granularity=build_config.TestGranularity.HYBRID,
            ),
        )
        context = build_config.BuildContext(request, build_config.LocalConfig(), self.make_profile(), {'debug': preset}, preset, 'windows', cmake='cmake', jobs=4, environment={})
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        with mock.patch.object(build_runtime, 'run_command') as run:
            build_core.run_all_native_tests(context, output)
        assert run.call_args_list[0].args[0][-2:] == [
            '--output-junit',
            str(build_core.REPO_ROOT / 'Build/results.xml'),
        ]
        assert run.call_args_list[1].args[0][-2:] == [
            '--output-junit',
            str(build_core.REPO_ROOT / 'Build/results.direct.xml'),
        ]

    def test_direct_lifecycle_phase_allows_regex_without_a_direct_match(self) -> None:
        preset = self.make_preset()
        request = build_config.CommandRequest(
            build_config.Action.TEST,
            options=build_config.TestActionOptions(
                target='all',
                include_direct=True,
                ctest_regex='^FCoreTests\\.',
                granularity=build_config.TestGranularity.CASE,
            ),
        )
        context = build_config.BuildContext(request, build_config.LocalConfig(), self.make_profile(), {'debug': preset}, preset, 'windows', cmake='cmake', jobs=4, environment={})
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        with mock.patch.object(build_runtime, 'run_command') as run:
            build_core.run_all_native_tests(context, output)
        assert '--no-tests=error' in run.call_args_list[0].args[0]
        assert '--no-tests=ignore' in run.call_args_list[1].args[0]

    def test_all_native_tests_reject_gtest_filter(self) -> None:
        request = build_config.CommandRequest(build_config.Action.TEST, options=build_config.TestActionOptions(target='all', filter='Core.*'))
        with pytest.raises(build_config.BuildToolError, match='cannot be used with --target all'):
            build_core.validate_request(request, self.make_preset())

    def test_single_native_test_rejects_ctest_only_options(self) -> None:
        request = build_config.CommandRequest(build_config.Action.TEST, options=build_config.TestActionOptions(target='CoreTests', include_direct=True))
        with pytest.raises(build_config.BuildToolError, match='require --target all'):
            build_core.validate_request(request, self.make_preset())

    def test_single_native_test_rejects_explicit_granularity(self) -> None:
        request = build_config.CommandRequest(build_config.Action.TEST, options=build_config.TestActionOptions(target='CoreTests', granularity=build_config.TestGranularity.TARGET))
        with pytest.raises(build_config.BuildToolError, match='--granularity require --target all'):
            build_core.validate_request(request, self.make_preset())

    def test_batched_granularity_rejects_ctest_regex(self) -> None:
        request = build_config.CommandRequest(build_config.Action.TEST, options=build_config.TestActionOptions(target='all', ctest_regex='Core', granularity=build_config.TestGranularity.HYBRID))
        with pytest.raises(build_config.BuildToolError, match='requires --granularity case'):
            build_core.validate_request(request, self.make_preset())

    @pytest.mark.parametrize(
        ('granularity', 'label'),
        (
            (build_config.TestGranularity.CASE, 'native-test-case'),
            (build_config.TestGranularity.TARGET, 'native-test-target'),
            (build_config.TestGranularity.HYBRID, 'native-test-default'),
        ),
    )
    def test_all_native_test_granularities_select_exact_labels(self, granularity: build_config.TestGranularity, label: str) -> None:
        preset = self.make_preset()
        request = build_config.CommandRequest(build_config.Action.TEST, options=build_config.TestActionOptions(target='all', granularity=granularity))
        context = build_config.BuildContext(request, build_config.LocalConfig(), self.make_profile(), {'debug': preset}, preset, 'windows', cmake='cmake', jobs=4, environment={})
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        with mock.patch.object(build_runtime, 'run_command') as run:
            build_core.run_all_native_tests(context, output)
        command = run.call_args.args[0]
        assert command[command.index('-L') + 1] == label
        assert command[command.index('-LE') + 1] == 'native-test-characterization'

    def test_random_batched_mode_injects_and_reports_gtest_seed(self) -> None:
        preset = self.make_preset()
        request = build_config.CommandRequest(build_config.Action.TEST, options=build_config.TestActionOptions(target='all', granularity=build_config.TestGranularity.TARGET, schedule_random=True))
        context = build_config.BuildContext(request, build_config.LocalConfig(), self.make_profile(), {'debug': preset}, preset, 'windows', cmake='cmake', jobs=4, environment={'PATH': 'cached'})
        stdout = io.StringIO()
        output = BuildOutput(plain=True, stdout=stdout, stderr=io.StringIO())
        with mock.patch.object(build_runtime.secrets, 'randbelow', return_value=40), mock.patch.object(build_runtime, 'run_command') as run:
            build_core.run_all_native_tests(context, output)
        assert run.call_args.kwargs['environment'] == {'PATH': 'cached', 'GTEST_SHUFFLE': '1', 'GTEST_RANDOM_SEED': '41'}
        assert '--schedule-random' in run.call_args.args[0]
        assert 'GoogleTest shuffle seed: 41' in stdout.getvalue()

    def test_target_include_direct_is_a_noop(self) -> None:
        preset = self.make_preset()
        request = build_config.CommandRequest(build_config.Action.TEST, options=build_config.TestActionOptions(target='all', granularity=build_config.TestGranularity.TARGET, include_direct=True))
        context = build_config.BuildContext(request, build_config.LocalConfig(), self.make_profile(), {'debug': preset}, preset, 'windows', cmake='cmake', jobs=4, environment={})
        stdout = io.StringIO()
        output = BuildOutput(plain=True, stdout=stdout, stderr=io.StringIO())
        with mock.patch.object(build_runtime, 'run_command') as run:
            build_core.run_all_native_tests(context, output)
        assert run.call_count == 1
        assert 'selected no additional registrations' in stdout.getvalue()

    def test_batched_failure_prints_focused_target_diagnostic(self) -> None:
        preset = self.make_preset()
        request = build_config.CommandRequest(build_config.Action.TEST, options=build_config.TestActionOptions(target='all', granularity=build_config.TestGranularity.TARGET))
        context = build_config.BuildContext(request, build_config.LocalConfig(), self.make_profile(), {'debug': preset}, preset, 'windows', cmake='cmake', jobs=4, environment={})
        stdout = io.StringIO()
        output = BuildOutput(plain=True, stdout=stdout, stderr=io.StringIO())
        with mock.patch.object(build_runtime, 'run_command', side_effect=build_config.BuildToolError('failed')), pytest.raises(build_config.BuildToolError, match='failed'):
            build_core.run_all_native_tests(context, output)
        diagnostic = stdout.getvalue()
        assert '--target <failed-target> --filter <suite.case>' in diagnostic
        assert '--granularity case' not in diagnostic

    def test_empty_case_regex_reports_actionable_rerun(self) -> None:
        preset = self.make_preset()
        request = build_config.CommandRequest(build_config.Action.TEST, options=build_config.TestActionOptions(target='all', ctest_regex='MissingCase', granularity=build_config.TestGranularity.CASE))
        context = build_config.BuildContext(request, build_config.LocalConfig(), self.make_profile(), {'debug': preset}, preset, 'windows', cmake='cmake', jobs=4, environment={})
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        error = build_config.BuildToolError('ctest failed', output_excerpt='No tests were found!!!')
        with mock.patch.object(build_runtime, 'run_command', side_effect=error), pytest.raises(build_config.BuildToolError, match='No case registrations matched') as raised:
            build_core.run_all_native_tests(context, output)
        assert '--granularity case --ctest-regex' in raised.value.recovery

    def test_random_batched_mode_rejects_invalid_environment_seed(self) -> None:
        preset = self.make_preset()
        request = build_config.CommandRequest(build_config.Action.TEST, options=build_config.TestActionOptions(target='all', granularity=build_config.TestGranularity.HYBRID, schedule_random=True))
        context = build_config.BuildContext(request, build_config.LocalConfig(), self.make_profile(), {'debug': preset}, preset, 'windows', cmake='cmake', jobs=4, environment={'GTEST_RANDOM_SEED': 'invalid'})
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        with pytest.raises(build_config.BuildToolError, match='must be an integer'):
            build_core.run_all_native_tests(context, output)

    def test_configure_preserves_cache_unless_fresh_is_requested(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        preset = self.make_preset()
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        directory = tmp_path_factory.mktemp('case')
        with mock.patch.object(build_core, 'preset_build_directory', return_value=Path(directory)), mock.patch.object(build_core, 'require_english_msvc_ninja_prefix'), mock.patch.object(build_core, 'run_command') as run:
            context = build_config.BuildContext(build_config.CommandRequest(build_config.Action.CONFIGURE, options=build_config.BuildActionOptions()), build_config.LocalConfig(), self.make_profile(), {'debug': preset}, preset, 'windows', cmake='cmake', environment={})
            build_core.perform_action(context, output)
            assert run.call_args.args[0] == ['cmake', '--preset', 'debug']
            context.request = replace(context.request, options=build_config.BuildActionOptions(fresh=True))
            build_core.perform_action(context, output)
            assert run.call_args.args[0] == ['cmake', '--fresh', '--preset', 'debug']

    def test_configure_recovers_an_unusable_existing_cache_with_fresh(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        preset = self.make_preset()
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        directory = tmp_path_factory.mktemp('case')
        cache = Path(directory) / 'CMakeCache.txt'
        cache.write_text('CMAKE_MAKE_PROGRAM:FILEPATH=CMAKE_MAKE_PROGRAM-NOTFOUND\n', encoding='utf-8')
        with mock.patch.object(build_core, 'preset_build_directory', return_value=Path(directory)), mock.patch.object(build_core, 'require_english_msvc_ninja_prefix'), mock.patch.object(build_core, 'run_command') as run:
            context = build_config.BuildContext(build_config.CommandRequest(build_config.Action.CONFIGURE, options=build_config.BuildActionOptions()), build_config.LocalConfig(), self.make_profile(), {'debug': preset}, preset, 'windows', cmake='cmake', environment={})
            build_core.perform_action(context, output)
        assert run.call_args.args[0] == ['cmake', '--fresh', '--preset', 'debug']

    def test_all_native_tests_build_the_explicit_aggregate_target(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        preset = self.make_preset()
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        directory = Path(tmp_path_factory.mktemp('case'))
        (directory / 'CMakeCache.txt').write_text('CMAKE_MAKE_PROGRAM:FILEPATH=ninja\n', encoding='utf-8')
        context = build_config.BuildContext(build_config.CommandRequest(build_config.Action.TEST, options=build_config.TestActionOptions(target='ALL')), build_config.LocalConfig(), self.make_profile(), {'debug': preset}, preset, 'windows', cmake='cmake', jobs=4, environment={})
        with mock.patch.object(build_core, 'preset_build_directory', return_value=directory), mock.patch.object(build_core, 'ninja_uses_english_msvc_prefix', return_value=True), mock.patch.object(build_core, 'run_command') as run:
            build_core.perform_action(context, output)
        assert run.call_args.args[0] == ['cmake', '--build', str(directory), '--target', 'DurinNativeTests', '-j', '4']

    def test_failed_generator_cache_is_not_reused(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        cache = Path(directory) / 'CMakeCache.txt'
        cache.write_text('CMAKE_MAKE_PROGRAM:FILEPATH=CMAKE_MAKE_PROGRAM-NOTFOUND\n', encoding='utf-8')
        assert not build_core.cache_is_usable(cache)
        cache.write_text('CMAKE_MAKE_PROGRAM:FILEPATH=ninja\n', encoding='utf-8')
        assert build_core.cache_is_usable(cache)

    def test_ninja_msvc_prefix_requires_english(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        build_directory = Path(directory)
        rules = build_directory / 'CMakeFiles' / 'rules.ninja'
        rules.parent.mkdir()
        rules.write_text('msvc_deps_prefix = 注意: 包含文件:  \n', encoding='utf-8')
        assert not build_core.ninja_uses_english_msvc_prefix(build_directory)
        rules.write_text('msvc_deps_prefix = Note: including file:  \n', encoding='utf-8')
        assert build_core.ninja_uses_english_msvc_prefix(build_directory)

    def test_checkout_lock_is_exclusive_across_presets(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        path = build_core.lock_file_path(Path(directory))
        with build_core.BuildToolLock(path, {'pid': 1}):
            with pytest.raises(build_config.BuildToolError, match='already owns'):
                with build_core.BuildToolLock(path, {'pid': 2}):
                    pass

    def test_inaccessible_lock_reports_acl_recovery(self) -> None:
        path = Path('checkout.lock')
        denied = PermissionError(13, 'Permission denied', str(path))
        with mock.patch.object(Path, 'open', side_effect=denied), mock.patch.object(build_locking, 'recover_inaccessible_windows_lock', return_value=False), pytest.raises(build_config.BuildToolError) as raised:
            build_core.open_checkout_lock(path)
        assert 'file-permission problem' in str(raised.value)
        assert 'icacls' in raised.value.recovery
        assert 'Remove-Item' in raised.value.recovery

    def test_inaccessible_windows_lock_is_reopened_after_stale_recovery(self) -> None:
        path = Path('checkout.lock')
        handle = mock.Mock()
        denied = PermissionError(13, 'Permission denied', str(path))
        with mock.patch.object(Path, 'open', side_effect=[denied, handle]), mock.patch.object(build_locking, 'recover_inaccessible_windows_lock', return_value=True):
            assert build_core.open_checkout_lock(path) is handle

    def test_windows_lock_acl_is_reset_to_directory_inheritance(self) -> None:
        result = mock.Mock(returncode=0)
        with mock.patch.object(build_locking.os, 'name', 'nt'), mock.patch.object(build_locking.subprocess, 'run', return_value=result) as run:
            assert build_core.normalize_windows_lock_acl(Path('checkout.lock'))
        assert run.call_args.args[0] == ['icacls', 'checkout.lock', '/reset', '/q']

    def test_windows_lock_acl_reset_is_best_effort(self) -> None:
        with mock.patch.object(build_locking.os, 'name', 'nt'), mock.patch.object(build_locking.subprocess, 'run', return_value=mock.Mock(returncode=5)):
            assert not build_core.normalize_windows_lock_acl(Path('checkout.lock'))

    def test_stop_ignores_stale_unowned_lock(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        path = Path(directory) / 'checkout.lock'
        path.write_text(json.dumps({'pid': 424242}), encoding='utf-8')
        with mock.patch.object(build_locking, 'lock_file_path', return_value=path), mock.patch.object(build_locking.subprocess, 'run') as run, mock.patch.object(build_locking.os, 'killpg', create=True) as killpg:
            assert not build_core.stop_active_operation()
        run.assert_not_called()
        killpg.assert_not_called()

    def test_stop_terminates_process_recorded_by_owned_lock(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        path = Path(directory) / 'checkout.lock'
        with build_core.BuildToolLock(path, {'pid': 424242}), mock.patch.object(build_locking, 'lock_file_path', return_value=path):
            if os.name == 'nt':
                result = mock.Mock(returncode=0)
                with mock.patch.object(build_locking.subprocess, 'run', return_value=result) as run:
                    assert build_core.stop_active_operation()
                assert run.call_args.args[0][:3] == ['taskkill', '/PID', '424242']
            else:
                with mock.patch.object(build_locking.os, 'killpg') as killpg:
                    assert build_core.stop_active_operation()
                killpg.assert_called_once_with(424242, build_locking.signal.SIGTERM)

    def test_interruption_marker_requires_rebuild(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        marker = Path(directory) / 'interrupted.json'

        def interrupt(_target_override: str | None) -> None:
            raise build_config.BuildToolInterruptedError('interrupted')
        with pytest.raises(build_config.BuildToolInterruptedError):
            build_core.execute_with_recovery_marker(action=build_config.Action.BUILD, marker_file=marker, metadata={'pid': 1, 'action': 'build', 'target': 'Core'}, operation=interrupt)
        with pytest.raises(build_config.BuildToolError, match='did not return normally') as blocked:
            build_core.execute_with_recovery_marker(action=build_config.Action.BUILD, marker_file=marker, metadata={'pid': 1, 'action': 'build', 'target': 'Core'}, operation=lambda _target: None)
        assert 'run recover' in blocked.value.recovery
        build_core.execute_with_recovery_marker(action=build_config.Action.REBUILD, marker_file=marker, metadata={'pid': 1, 'action': 'rebuild', 'target': 'Core'}, operation=lambda _target: None)
        assert not marker.exists()

    def test_rebuild_rejects_an_unrelated_recovery_target(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        marker = Path(directory) / 'interrupted.json'
        marker.write_text(json.dumps({'pid': 1, 'action': 'build', 'target': 'Core'}), encoding='utf-8')
        operation = mock.Mock()
        with pytest.raises(build_config.BuildToolError, match='Interrupted target "Core"'):
            build_core.execute_with_recovery_marker(action=build_config.Action.REBUILD, marker_file=marker, metadata={'pid': 2, 'action': 'rebuild', 'target': 'Editor'}, operation=operation)
        operation.assert_not_called()
        assert build_core.recovery_target(marker) == 'Core'

    def test_rebuild_all_does_not_claim_to_recover_an_excluded_test_target(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        marker = Path(directory) / 'interrupted.json'
        marker.write_text(json.dumps({'pid': 1, 'action': 'test', 'target': 'DurinNativeTests'}), encoding='utf-8')
        operation = mock.Mock()
        with pytest.raises(build_config.BuildToolError, match='DurinNativeTests') as raised:
            build_core.execute_with_recovery_marker(action=build_config.Action.REBUILD, marker_file=marker, metadata={'pid': 2, 'action': 'rebuild', 'target': 'all'}, operation=operation)
        operation.assert_not_called()
        assert raised.value.recovery == 'Run rebuild --target DurinNativeTests.'

    def test_invalid_or_non_target_recovery_state_falls_back_to_all(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        marker = Path(directory) / 'interrupted.json'
        marker.write_text(json.dumps({'action': 'unknown', 'target': 'Core'}), encoding='utf-8')
        assert build_core.recoverable_target(marker) is None
        assert build_core.recovery_target(marker) == 'all'
        marker.write_text('{invalid', encoding='utf-8')
        assert build_core.recoverable_target(marker) is None
        assert build_core.recovery_target(marker) == 'all'

    def test_recover_clears_a_valid_interruption_marker(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        marker = Path(directory) / 'interrupted.json'
        marker.write_text(json.dumps({'pid': 1, 'action': 'build', 'target': 'Core'}), encoding='utf-8')
        operation = mock.Mock()
        build_core.execute_with_recovery_marker(action=build_config.Action.RECOVER, marker_file=marker, metadata={'pid': 2, 'action': 'recover', 'target': 'recorded-target'}, operation=operation)
        operation.assert_called_once_with('Core')
        assert not marker.exists()

    def test_recover_validation_failure_does_not_execute_or_replace_marker(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        marker = Path(directory) / 'interrupted.json'
        original = b'{"action": "unknown", "target": "Core"}'
        marker.write_bytes(original)
        operation = mock.Mock()
        with pytest.raises(build_config.BuildToolError, match='cannot be resumed safely'):
            build_core.execute_with_recovery_marker(action=build_config.Action.RECOVER, marker_file=marker, metadata={'pid': 2, 'action': 'recover', 'target': 'recorded-target'}, operation=operation)
        operation.assert_not_called()
        assert marker.read_bytes() == original

    def test_recover_builds_incrementally_without_cleaning(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        preset = self.make_preset()
        context = build_config.BuildContext(build_config.CommandRequest(build_config.Action.RECOVER), build_config.LocalConfig(), self.make_profile(), {'debug': preset}, preset, 'windows', cmake='cmake', jobs=4, environment={'PATH': 'cached'})
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        directory = tmp_path_factory.mktemp('case')
        build_directory = Path(directory)
        with mock.patch.object(build_core, 'preset_build_directory', return_value=build_directory), mock.patch.object(build_core, 'cache_is_usable', return_value=True), mock.patch.object(build_core, 'ninja_uses_english_msvc_prefix', return_value=True), mock.patch.object(build_core, 'run_command') as run:
            build_core.perform_action(context, output, target_override='Core')
        run.assert_called_once_with(['cmake', '--build', str(build_directory), '--target', 'Core', '-j', '4'], environment={'PATH': 'cached'}, output=output, show_heartbeat=False)

    def test_agent_build_enables_child_process_heartbeat(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        preset = self.make_preset()
        request = build_config.CommandRequest(
            build_config.Action.BUILD,
            output=build_config.OutputOptions(agent=True),
            options=build_config.BuildActionOptions(target='Core'),
        )
        context = build_config.BuildContext(request, build_config.LocalConfig(), self.make_profile(), {'debug': preset}, preset, 'windows', cmake='cmake', jobs=4, environment={'PATH': 'cached'})
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        build_directory = Path(tmp_path_factory.mktemp('case'))
        with mock.patch.object(build_core, 'preset_build_directory', return_value=build_directory), mock.patch.object(build_core, 'cache_is_usable', return_value=True), mock.patch.object(build_core, 'ninja_uses_english_msvc_prefix', return_value=True), mock.patch.object(build_core, 'run_command') as run:
            build_core.perform_action(context, output)
        assert run.call_args.kwargs['show_heartbeat']

    def test_normal_command_failure_restores_marker(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        marker = Path(directory) / 'interrupted.json'
        with pytest.raises(build_config.BuildToolError, match='failed'):
            build_core.execute_with_recovery_marker(action=build_config.Action.BUILD, marker_file=marker, metadata={'pid': 1}, operation=lambda _target: (_ for _ in ()).throw(build_config.BuildToolError('failed')))
        assert not marker.exists()

    def test_recover_command_failure_restores_interruption_marker(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        marker = Path(directory) / 'interrupted.json'
        original = b'{"pid": 1, "action": "build", "target": "Core"}'
        marker.write_bytes(original)
        with pytest.raises(build_config.BuildToolError, match='failed'):
            build_core.execute_with_recovery_marker(action=build_config.Action.RECOVER, marker_file=marker, metadata={'pid': 2, 'action': 'recover', 'target': 'recorded-target'}, operation=lambda _target: (_ for _ in ()).throw(build_config.BuildToolError('failed')))
        assert marker.read_bytes() == original

    def test_keyboard_interrupt_terminates_child_process_tree(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        process = mock.Mock()
        process.stdout = io.StringIO()
        process.wait.side_effect = KeyboardInterrupt
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        directory = tmp_path_factory.mktemp('case')
        with mock.patch.object(build_process, 'command_log_path', return_value=Path(directory) / 'command.log'), mock.patch.object(build_process.subprocess, 'Popen', return_value=process), mock.patch.object(build_process, 'terminate_process_tree') as terminate:
            with pytest.raises(build_config.BuildToolInterruptedError):
                build_core.run_command(['cmake', '--version'], environment={}, output=output)
        terminate.assert_called_once_with(process)

    def test_run_command_does_not_inherit_buildtool_handles(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        process = mock.Mock()
        process.stdout = io.StringIO()
        process.wait.return_value = 0
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        directory = tmp_path_factory.mktemp('case')
        with mock.patch.object(build_process, 'command_log_path', return_value=Path(directory) / 'command.log'), mock.patch.object(build_process.subprocess, 'Popen', return_value=process) as popen:
            build_core.run_command(['cmake', '--version'], environment={}, output=output)
        assert popen.call_args.kwargs['close_fds']
        assert popen.call_args.kwargs['stdout'] is build_process.subprocess.PIPE

    def test_unavailable_command_log_prevents_child_launch(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        unavailable = Path(directory) / 'unavailable'
        unavailable.write_text('not a directory', encoding='utf-8')
        log_path = unavailable / 'command.log'
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        child = [os.sys.executable, '-c', "print('x' * 1_000_000)"]
        with mock.patch.object(build_process, 'command_log_path', return_value=log_path), mock.patch.object(build_process.subprocess, 'Popen') as popen, pytest.raises(build_config.BuildToolError, match='Could not capture command output') as raised:
            build_core.run_command(child, environment=os.environ, output=output)
        popen.assert_not_called()
        assert raised.value.log_path == log_path
        assert str(log_path) in str(raised.value)
        assert isinstance(raised.value.__cause__, OSError)

    def test_command_timeout_terminates_child_process_tree(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        process = mock.Mock()
        process.stdout = io.StringIO('compiler.cpp(7): error C1234: broken\n')
        process.wait.side_effect = build_process.subprocess.TimeoutExpired(['CoreTests'], 0)
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        directory = tmp_path_factory.mktemp('case')
        with mock.patch.object(build_process, 'command_log_path', return_value=Path(directory) / 'command.log'), mock.patch.object(build_process.subprocess, 'Popen', return_value=process), mock.patch.object(build_process, 'terminate_process_tree') as terminate, pytest.raises(build_config.BuildToolError, match='timed out'):
            build_core.run_command(['CoreTests'], environment={}, output=output, timeout_seconds=0.001)
        terminate.assert_called_once_with(process)

    def test_compact_command_output_is_logged_and_failure_is_summarized(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        stdout = io.StringIO()
        output = BuildOutput(plain=True, output_mode=build_config.OutputMode.COMPACT, stdout=stdout, stderr=io.StringIO())
        directory = tmp_path_factory.mktemp('case')
        log_path = Path(directory) / 'command.log'
        with mock.patch.object(build_process, 'command_log_path', return_value=log_path):
            with pytest.raises(build_config.BuildToolError) as raised:
                build_core.run_command([os.sys.executable, '-c', "print('noise'); print('source.cpp(9): error C1000: failed'); raise SystemExit(1)"], environment=os.environ, output=output)
        assert 'noise' in log_path.read_text(encoding='utf-8')
        assert '\nnoise\n' not in stdout.getvalue()
        assert 'error C1000' in raised.value.output_excerpt
        assert raised.value.log_path == log_path

    def test_full_command_output_streams_and_is_logged(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        stdout = io.StringIO()
        output = BuildOutput(plain=True, output_mode=build_config.OutputMode.FULL, stdout=stdout, stderr=io.StringIO())
        directory = tmp_path_factory.mktemp('case')
        log_path = Path(directory) / 'command.log'
        with mock.patch.object(build_process, 'command_log_path', return_value=log_path):
            build_core.run_command([os.sys.executable, '-c', "print('visible child output')"], environment=os.environ, output=output)
        assert 'visible child output' in log_path.read_text(encoding='utf-8')
        assert 'visible child output' in stdout.getvalue()

    def test_compact_command_output_preserves_gtest_summary(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        stdout = io.StringIO()
        output = BuildOutput(plain=True, output_mode=build_config.OutputMode.COMPACT, stdout=stdout, stderr=io.StringIO())
        child_script = "print('[==========] 122 tests from 25 test suites ran. (100 ms total)'); print('[  PASSED  ] 122 tests.')"
        directory = tmp_path_factory.mktemp('case')
        with mock.patch.object(build_process, 'command_log_path', return_value=Path(directory) / 'command.log'):
            build_core.run_command([os.sys.executable, '-c', child_script], environment=os.environ, output=output)
        assert '122 tests from 25 test suites ran' in stdout.getvalue()
        assert '[  PASSED  ] 122 tests.' in stdout.getvalue()

    def test_native_test_failure_does_not_leave_recovery_marker(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        preset = self.make_preset()
        context = build_config.BuildContext(build_config.CommandRequest(build_config.Action.TEST, options=build_config.TestActionOptions(target='CoreTests')), build_config.LocalConfig(), self.make_profile(), {'debug': preset}, preset, 'windows', cmake='cmake', jobs=1, environment={})
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        marker = root / 'interrupted.json'
        lock = root / 'checkout.lock'
        with mock.patch.object(build_core, 'interruption_marker_path', return_value=marker), mock.patch.object(build_core, 'lock_file_path', return_value=lock), mock.patch.object(build_core, 'perform_action'), mock.patch.object(build_core, 'run_native_test', side_effect=build_config.BuildToolError('test failed')), pytest.raises(build_config.BuildToolError, match='test failed'):
            build_core.execute_context(context, output, confirm_purge=lambda _paths, _all: False)
        assert not marker.exists()

    def test_all_native_test_failure_does_not_leave_recovery_marker(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        preset = self.make_preset()
        context = build_config.BuildContext(build_config.CommandRequest(build_config.Action.TEST, options=build_config.TestActionOptions(target='all')), build_config.LocalConfig(), self.make_profile(), {'debug': preset}, preset, 'windows', cmake='cmake', jobs=1, environment={})
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        root = Path(tmp_path_factory.mktemp('case'))
        marker = root / 'interrupted.json'
        lock = root / 'checkout.lock'
        with mock.patch.object(build_core, 'interruption_marker_path', return_value=marker), mock.patch.object(build_core, 'lock_file_path', return_value=lock), mock.patch.object(build_core, 'perform_action'), mock.patch.object(build_core, 'run_all_native_tests', side_effect=build_config.BuildToolError('test failed')), pytest.raises(build_config.BuildToolError, match='test failed'):
            build_core.execute_context(context, output, confirm_purge=lambda _paths, _all: False)
        assert not marker.exists()

    def test_purge_paths_cover_build_outputs_and_metadata(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        project = root / 'Engine'
        project.mkdir()
        (project / 'Engine.dproject').touch()
        paths = set(build_core.collect_purge_paths(self.make_profile(), [self.make_preset()], root=root))
        assert root / 'Build/debug' in paths
        assert root / 'Engine/Binaries/Win64/Debug' in paths
        assert root / 'Engine/Binaries/Win64/ThirdParty/Debug' in paths
        assert root / 'Engine/Intermediate/Build/Win64/DurinEditor' in paths

    def test_project_purge_removes_persistent_dht_cache(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        root = Path(tmp_path_factory.mktemp('case'))
        project = root / 'Engine'
        project.mkdir()
        (project / 'Engine.dproject').touch()
        cache_entry = project / 'Intermediate/Build/Win64/DurinEditor/DHTCache/Engine/export/entry.json'
        cache_entry.parent.mkdir(parents=True)
        cache_entry.write_text('{}', encoding='utf-8')

        paths = build_core.collect_purge_paths(self.make_profile(), [self.make_preset()], root=root)
        build_core.remove_purge_paths(paths, root=root)

        assert not cache_entry.exists()

    def test_compatible_presets_share_one_runtime_intermediate_purge_root(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        root = Path(tmp_path_factory.mktemp('case'))
        project = root / 'Engine'
        project.mkdir()
        (project / 'Engine.dproject').touch()
        presets = [
            self.make_preset(name='debug-tests', testing='ON'),
            self.make_preset(name='debug-editor', testing='OFF'),
        ]

        paths = build_core.collect_purge_paths(self.make_profile(), presets, root=root)

        intermediate = project / 'Intermediate/Build/Win64/DurinEditor'
        assert paths.count(intermediate) == 1

    def test_purge_rejects_checkout_root(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        with pytest.raises(build_config.BuildToolError, match='checkout root'):
            build_core.remove_purge_paths([root], root=root)

    def test_purge_removes_only_selected_artifact(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        artifact = root / 'Build' / 'debug'
        artifact.mkdir(parents=True)
        preserved = root / 'Build' / 'ThirdParty' / 'library.lib'
        preserved.parent.mkdir(parents=True)
        preserved.touch()
        build_core.remove_purge_paths([artifact], root=root)
        assert not artifact.exists()
        assert preserved.exists()
