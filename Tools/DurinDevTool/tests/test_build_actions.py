from . import build_request_fixtures as request_fixtures
import pytest
import io
import os
from dataclasses import replace
from pathlib import Path
from unittest import mock
REPO_ROOT = Path(__file__).resolve().parents[3]
DEV_TOOL_DIR = REPO_ROOT / 'Tools' / 'DurinDevTool'
if str(DEV_TOOL_DIR) not in os.sys.path:
    os.sys.path.insert(0, str(DEV_TOOL_DIR))
from durin_dev_tool.build import config as build_config
from durin_dev_tool.build import core as build_core
from durin_dev_tool.build import runtime as build_runtime
from durin_dev_tool.build.handler import request_from_namespace
from durin_dev_tool.build.output import BuildOutput
from durin_dev_tool.registry import CommandRegistry

def parse_build_request(arguments: list[str]) -> build_config.ConcreteRequest:
    _spec, namespace = CommandRegistry().parse(arguments)
    if getattr(namespace, 'selected_preset', ''):
        namespace.preset = namespace.selected_preset
    return request_from_namespace(namespace)


class TestCore:
    def make_profile(self) -> build_config.BuildProfile:
        return build_config.BuildProfile('test-profile', 'windows', 'debug', ('debug', 'release'), build_config.EnvironmentProvider.INHERIT, 'Win64', '.exe', True, ())
    def make_preset(self, name: str='debug', testing: str='ON', runtime_variant: str='DurinEditor') -> build_config.ConfigurePreset:
        return build_config.ConfigurePreset(name, {'name': name, 'binaryDir': '${sourceDir}/Build/${presetName}', 'cacheVariables': {'CMAKE_BUILD_TYPE': 'Debug', 'DURIN_RUNTIME_VARIANT': runtime_variant, 'BUILD_TESTING': testing}})
    def test_test_action_rejects_non_test_preset(self) -> None:
        request = request_fixtures.command_request(build_config.Action.TEST, options=request_fixtures.TestActionOptions(target='CoreTests'))
        with pytest.raises(build_config.BuildToolError, match='does not enable BUILD_TESTING'):
            build_core.validate_request(request, self.make_preset(testing='OFF'))
    def test_compact_native_test_enables_gtest_brief_output(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        preset = self.make_preset()
        context = build_config.BuildContext(request_fixtures.command_request(build_config.Action.TEST, options=request_fixtures.TestActionOptions(target='CoreTests', filter='Core.*')), build_config.LocalConfig(), self.make_profile(), {'debug': preset}, preset, 'windows', environment={})
        output = BuildOutput(plain=True, output_mode=build_config.OutputMode.COMPACT, stdout=io.StringIO(), stderr=io.StringIO())
        directory = tmp_path_factory.mktemp('case')
        with mock.patch.object(build_runtime, 'test_executable_path', return_value=Path(directory) / 'CoreTests.exe') as executable_path, mock.patch.object(build_runtime, 'run_command') as run:
            executable_path.return_value.touch()
            build_runtime.run_native_test(context, output)
        assert run.call_args.args[0] == [str(executable_path.return_value), '--gtest_filter=Core.*', '--gtest_brief=1']
        assert run.call_args.kwargs['colorize_test_output']
    @pytest.mark.parametrize(
        ('resolved_host', 'expected_runner'),
        (('direct', 'run_native_test'), ('application', 'run_selected_native_tests')),
    )
    def test_exact_native_test_uses_registry_driven_strategy(
        self, resolved_host: str, expected_runner: str
    ) -> None:
        preset = self.make_preset()
        context = build_config.BuildContext(
            request_fixtures.command_request(
                build_config.Action.TEST,
                options=request_fixtures.TestActionOptions(target='LaunchTests'),
            ),
            build_config.LocalConfig(),
            self.make_profile(),
            {'debug': preset},
            preset,
            'windows',
            cmake='cmake',
            jobs=1,
            environment={},
        )
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        resolved = mock.Mock(
            names=('LaunchTests',),
            targets=(mock.Mock(resolved_execution_host=resolved_host),),
        )
        with mock.patch.object(build_runtime, 'load_native_test_registry', return_value=mock.Mock()), mock.patch.object(build_runtime, 'resolve_selection', return_value=resolved), mock.patch.object(build_runtime, 'run_native_test') as direct, mock.patch.object(build_runtime, 'run_selected_native_tests') as selected:
            build_runtime.run_exact_native_test(context, output)
        runners = {'run_native_test': direct, 'run_selected_native_tests': selected}
        runners[expected_runner].assert_called_once_with(context, output)
        runners[({'run_native_test', 'run_selected_native_tests'} - {expected_runner}).pop()].assert_not_called()
        assert context.resolved_test_targets == (
            ('LaunchTests',) if resolved_host == 'application' else ()
        )
    def test_exact_native_test_reports_stale_registry_failure(self) -> None:
        preset = self.make_preset()
        context = build_config.BuildContext(
            request_fixtures.command_request(
                build_config.Action.TEST,
                options=request_fixtures.TestActionOptions(target='CoreTests'),
            ),
            build_config.LocalConfig(),
            self.make_profile(),
            {'debug': preset},
            preset,
            'windows',
        )
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        with mock.patch.object(
            build_runtime,
            'load_native_test_registry',
            side_effect=build_config.BuildToolError('registry identity does not match'),
        ), pytest.raises(build_config.BuildToolError, match='identity does not match'):
            build_runtime.run_exact_native_test(context, output)
    def test_all_native_tests_use_target_ctest_registration_and_report_mode(self) -> None:
        preset = self.make_preset()
        cmake = 'C:/Tools/CMake/bin/cmake.exe'
        context = build_config.BuildContext(request_fixtures.command_request(build_config.Action.TEST, options=request_fixtures.TestActionOptions(target='ALL', mode=build_config.TestMode.REPORT, report_path=Path('Build/results.xml'), timeout_seconds=60)), build_config.LocalConfig(), self.make_profile(), {'debug': preset}, preset, 'windows', cmake=cmake, jobs=4, environment={'PATH': 'cached'})
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        build_directory = Path('Build/debug')
        with mock.patch.object(build_runtime, 'preset_build_directory', return_value=build_directory), mock.patch.object(build_runtime, 'run_command') as run:
            build_runtime.run_all_native_tests(context, output)
        run.assert_called_once_with(
            [str(Path(cmake).with_name('ctest.exe')), '--test-dir', str(build_directory), '--output-on-failure', '--no-tests=error', '-j', '4', '-L', 'native-test-target', '-LE', 'native-test-characterization|native-test-qualification', '--timeout', '60', '--output-junit', str(build_config.default_build_paths().root / 'Build/results.xml')],
            environment={'PATH': 'cached', 'GTEST_BRIEF': '1'},
            output=output,
            recovery_required_on_interrupt=False,
            interruption_message='Native test run was interrupted.',
            colorize_test_output=True,
            show_heartbeat=False,
            cwd=build_config.default_build_paths().root,
            state_directory=build_config.default_build_paths().state_directory,
        )
    def test_selected_set_uses_direct_ctest_registrations_and_report_path(self) -> None:
        preset = self.make_preset()
        request = request_fixtures.command_request(
            build_config.Action.TEST,
            options=request_fixtures.TestActionOptions(
                target='@viewport',
                mode=build_config.TestMode.REPORT,
            ),
        )
        context = build_config.BuildContext(
            request,
            build_config.LocalConfig(),
            self.make_profile(),
            {'debug': preset},
            preset,
            'windows',
            cmake=r'C:\Tools\CMake\bin\cmake.exe',
            jobs=4,
            environment={'PATH': 'cached'},
            resolved_test_targets=('MonaViewportTests', 'ViewportTests'),
        )
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        build_directory = Path('Build/debug')
        with mock.patch.object(build_runtime, 'preset_build_directory', return_value=build_directory), mock.patch.object(build_runtime, 'run_command') as run:
            build_runtime.run_selected_native_tests(context, output)
        command = run.call_args.args[0]
        assert command[7:13] == [
            '-L',
            'native-test-target',
            '-LE',
            'native-test-characterization|native-test-qualification',
            '-R',
            r'^Durin\.NativeTestDirect\.(MonaViewportTests|ViewportTests)$',
        ]
        assert command[-2:] == [
            '--output-junit',
            str(build_config.default_build_paths().root / 'Build/NativeTestResults/debug/viewport.xml'),
        ]
    def test_selected_set_builds_only_resolved_cmake_targets(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        preset = self.make_preset()
        request = request_fixtures.command_request(
            build_config.Action.TEST,
            options=request_fixtures.TestActionOptions(target='@viewport'),
        )
        context = build_config.BuildContext(
            request,
            build_config.LocalConfig(),
            self.make_profile(),
            {'debug': preset},
            preset,
            'windows',
            cmake='cmake',
            jobs=4,
            environment={},
            resolved_test_targets=('MonaViewportTests', 'ViewportTests'),
        )
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        build_directory = Path(tmp_path_factory.mktemp('case'))
        with mock.patch.object(build_core, 'preset_build_directory', return_value=build_directory), mock.patch.object(build_core, 'cache_is_usable', return_value=True), mock.patch.object(build_core, 'run_command') as run:
            build_core.perform_action(context, output)
        assert run.call_args.args[0] == [
            'cmake',
            '--build',
            str(build_directory),
            '--target',
            'MonaViewportTests',
            'ViewportTests',
            '-j',
            '4',
        ]
    def test_all_native_tests_reject_gtest_filter(self) -> None:
        request = request_fixtures.command_request(build_config.Action.TEST, options=request_fixtures.TestActionOptions(target='all', filter='Core.*'))
        with pytest.raises(build_config.BuildToolError, match='cannot be used with test all'):
            build_core.validate_request(request, self.make_preset())
    def test_qualification_mode_selects_only_qualification_registrations(self) -> None:
        preset = self.make_preset()
        request = request_fixtures.command_request(
            build_config.Action.TEST,
            options=request_fixtures.TestActionOptions(
                target='@kind=qualification',
                mode=build_config.TestMode.QUALIFICATION,
            ),
        )
        context = build_config.BuildContext(
            request,
            build_config.LocalConfig(),
            self.make_profile(),
            {'debug': preset},
            preset,
            'windows',
            cmake='cmake',
            jobs=4,
            environment={},
            resolved_test_targets=('TerrainRenderQualificationTests',),
        )
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        with mock.patch.object(build_runtime, 'run_command') as run:
            build_runtime.run_selected_native_tests(context, output)
        command = run.call_args.args[0]
        assert command[7:13] == [
            '-L',
            'native-test-target',
            '-L',
            'native-test-qualification',
            '-R',
            r'^Durin\.NativeTestDirect\.(TerrainRenderQualificationTests)$',
        ]
    def test_random_batched_mode_injects_and_reports_gtest_seed(self) -> None:
        preset = self.make_preset()
        request = request_fixtures.command_request(build_config.Action.TEST, options=request_fixtures.TestActionOptions(target='all', mode=build_config.TestMode.STRESS))
        context = build_config.BuildContext(request, build_config.LocalConfig(), self.make_profile(), {'debug': preset}, preset, 'windows', cmake='cmake', jobs=4, environment={'PATH': 'cached'})
        stdout = io.StringIO()
        output = BuildOutput(plain=True, stdout=stdout, stderr=io.StringIO())
        with mock.patch.object(build_runtime.secrets, 'randbelow', return_value=40), mock.patch.object(build_runtime, 'run_command') as run:
            build_runtime.run_all_native_tests(context, output)
        assert run.call_args.kwargs['environment'] == {
            'PATH': 'cached',
            'GTEST_BRIEF': '1',
            'GTEST_SHUFFLE': '1',
            'GTEST_RANDOM_SEED': '41',
        }
        assert '--schedule-random' in run.call_args.args[0]
        assert 'GoogleTest shuffle seed: 41' in stdout.getvalue()
    def test_batched_failure_prints_focused_target_diagnostic(self) -> None:
        preset = self.make_preset()
        request = request_fixtures.command_request(build_config.Action.TEST, options=request_fixtures.TestActionOptions(target='all'))
        context = build_config.BuildContext(request, build_config.LocalConfig(), self.make_profile(), {'debug': preset}, preset, 'windows', cmake='cmake', jobs=4, environment={})
        stdout = io.StringIO()
        output = BuildOutput(plain=True, stdout=stdout, stderr=io.StringIO())
        with mock.patch.object(build_runtime, 'run_command', side_effect=build_config.BuildToolError('failed')), pytest.raises(build_config.BuildToolError, match='failed'):
            build_runtime.run_all_native_tests(context, output)
        diagnostic = stdout.getvalue()
        assert 'test <failed-target> <suite.case>' in diagnostic
    def test_random_batched_mode_rejects_invalid_environment_seed(self) -> None:
        preset = self.make_preset()
        request = request_fixtures.command_request(build_config.Action.TEST, options=request_fixtures.TestActionOptions(target='all', mode=build_config.TestMode.STRESS))
        context = build_config.BuildContext(request, build_config.LocalConfig(), self.make_profile(), {'debug': preset}, preset, 'windows', cmake='cmake', jobs=4, environment={'GTEST_RANDOM_SEED': 'invalid'})
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        with pytest.raises(build_config.BuildToolError, match='must be an integer'):
            build_runtime.run_all_native_tests(context, output)
    def test_stress_selection_rejects_invalid_environment_seed(self) -> None:
        preset = self.make_preset()
        request = request_fixtures.command_request(
            build_config.Action.TEST,
            options=request_fixtures.TestActionOptions(
                target='@viewport',
                mode=build_config.TestMode.STRESS,
            ),
        )
        context = build_config.BuildContext(
            request,
            build_config.LocalConfig(),
            self.make_profile(),
            {'debug': preset},
            preset,
            'windows',
            cmake='cmake',
            jobs=4,
            environment={'GTEST_RANDOM_SEED': 'invalid'},
            resolved_test_targets=('ViewportTests',),
        )
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        with pytest.raises(build_config.BuildToolError, match='must be an integer'):
            build_runtime.run_selected_native_tests(context, output)
    def test_stress_selection_uses_shared_random_invocation(self) -> None:
        preset = self.make_preset()
        request = request_fixtures.command_request(
            build_config.Action.TEST,
            options=request_fixtures.TestActionOptions(
                target='@viewport',
                mode=build_config.TestMode.STRESS,
                timeout_seconds=60,
            ),
        )
        context = build_config.BuildContext(
            request,
            build_config.LocalConfig(),
            self.make_profile(),
            {'debug': preset},
            preset,
            'windows',
            cmake='cmake',
            jobs=4,
            environment={'GTEST_RANDOM_SEED': '41'},
            resolved_test_targets=('ViewportTests',),
        )
        stdout = io.StringIO()
        output = BuildOutput(plain=True, stdout=stdout, stderr=io.StringIO())
        with mock.patch.object(build_runtime, 'run_command') as run:
            build_runtime.run_selected_native_tests(context, output)
        assert run.call_args.args[0][-3:] == ['--timeout', '60', '--schedule-random']
        assert run.call_args.kwargs['environment'] == {
            'GTEST_RANDOM_SEED': '41',
            'GTEST_BRIEF': '1',
            'GTEST_SHUFFLE': '1',
        }
        assert 'GoogleTest shuffle seed: 41' in stdout.getvalue()
    def test_configure_preserves_cache_unless_fresh_is_requested(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        preset = self.make_preset()
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        directory = tmp_path_factory.mktemp('case')
        with mock.patch.object(build_core, 'preset_build_directory', return_value=Path(directory)), mock.patch.object(build_core, 'require_english_msvc_ninja_prefix'), mock.patch.object(build_core, 'run_command') as run:
            context = build_config.BuildContext(request_fixtures.command_request(build_config.Action.CONFIGURE, options=request_fixtures.BuildActionOptions()), build_config.LocalConfig(), self.make_profile(), {'debug': preset}, preset, 'windows', cmake='cmake', environment={})
            build_core.perform_action(context, output)
            assert run.call_args.args[0] == ['cmake', '--preset', 'debug']
            context.request = replace(context.request, fresh=True)
            build_core.perform_action(context, output)
            assert run.call_args.args[0] == ['cmake', '--fresh', '--preset', 'debug']
            context.request = replace(
                context.request,
                fresh=False,
                defines=(
                    'DURIN_ENABLE_APPLICATION_TESTS=ON',
                    'DURIN_DHT_WORKERS=4',
                ),
            )
            build_core.perform_action(context, output)
            assert run.call_args.args[0] == [
                'cmake', '--preset', 'debug',
                '-DDURIN_ENABLE_APPLICATION_TESTS=ON',
                '-DDURIN_DHT_WORKERS=4',
            ]
    def test_configure_recovers_an_unusable_existing_cache_with_fresh(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        preset = self.make_preset()
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        directory = tmp_path_factory.mktemp('case')
        cache = Path(directory) / 'CMakeCache.txt'
        cache.write_text('CMAKE_MAKE_PROGRAM:FILEPATH=CMAKE_MAKE_PROGRAM-NOTFOUND\n', encoding='utf-8')
        with mock.patch.object(build_core, 'preset_build_directory', return_value=Path(directory)), mock.patch.object(build_core, 'require_english_msvc_ninja_prefix'), mock.patch.object(build_core, 'run_command') as run:
            context = build_config.BuildContext(request_fixtures.command_request(build_config.Action.CONFIGURE, options=request_fixtures.BuildActionOptions()), build_config.LocalConfig(), self.make_profile(), {'debug': preset}, preset, 'windows', cmake='cmake', environment={})
            build_core.perform_action(context, output)
        assert run.call_args.args[0] == ['cmake', '--fresh', '--preset', 'debug']
    def test_all_native_tests_build_the_explicit_aggregate_target(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        preset = self.make_preset()
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        directory = Path(tmp_path_factory.mktemp('case'))
        (directory / 'CMakeCache.txt').write_text('CMAKE_MAKE_PROGRAM:FILEPATH=ninja\n', encoding='utf-8')
        context = build_config.BuildContext(request_fixtures.command_request(build_config.Action.TEST, options=request_fixtures.TestActionOptions(target='ALL')), build_config.LocalConfig(), self.make_profile(), {'debug': preset}, preset, 'windows', cmake='cmake', jobs=4, environment={})
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
