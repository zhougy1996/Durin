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
from durin_dev_tool.build import scaffolding as build_scaffolding
from durin_dev_tool.build.handler import request_from_namespace
from durin_dev_tool.build.output import BuildOutput
from durin_dev_tool.bootstrap import preflight
from durin_dev_tool import configuration
from durin_dev_tool.registry import CommandRegistry

def parse_build_request(arguments: list[str]) -> build_config.CommandRequest:
    _spec, namespace = CommandRegistry().parse(arguments)
    if getattr(namespace, 'selected_preset', ''):
        namespace.preset = namespace.selected_preset
    return request_from_namespace(namespace)

class TestBuildConfig:

    def test_missing_config_uses_empty_overrides(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        config = build_config.load_local_config(Path(directory) / 'missing.json')
        assert config == build_config.LocalConfig()

    def test_repository_template_uses_automatic_defaults(self) -> None:
        config = build_config.load_local_config(
            REPO_ROOT / 'Templates' / 'DurinDevTool' / 'DevTool.user.json'
        )
        assert config == build_config.LocalConfig()

    def test_valid_config_uses_typed_models(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        path = Path(directory) / 'config.json'
        path.write_text(json.dumps({'version': 1, 'build': {'defaultProfile': 'windows-msvc-x64', 'parallelJobs': 8}, 'cmake': {'command': 'custom-cmake'}, 'toolchain': {'environmentScript': 'setup.cmd', 'environmentArguments': ['x64']}}), encoding='utf-8')
        config = build_config.load_local_config(path)
        assert config.cmake_command == 'custom-cmake'
        assert config.jobs == 8
        assert config.environment_setup.arguments == ('x64',)

    def test_invalid_json_and_field_types_are_rejected(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        path = Path(directory) / 'config.json'
        path.write_text('{', encoding='utf-8')
        with pytest.raises(build_config.BuildToolError, match='invalid JSON'):
            build_config.load_local_config(path)
        path.write_text(json.dumps({'version': 1, 'cmake': {'command': 42}}), encoding='utf-8')
        with pytest.raises(build_config.BuildToolError, match='null or a non-empty string'):
            build_config.load_local_config(path)
        path.write_text(json.dumps({'version': 1, 'build': {'parallelJobs': 257}}), encoding='utf-8')
        with pytest.raises(build_config.BuildToolError, match='integer from 1 to 256'):
            build_config.load_local_config(path)
        path.write_text(json.dumps({'cmakeCommand': 'legacy-cmake'}), encoding='utf-8')
        with pytest.raises(build_config.BuildToolError, match='unknown field'):
            build_config.load_local_config(path)

    def test_repository_profiles_reference_existing_presets(self) -> None:
        profiles = build_config.load_profiles()
        presets = build_config.load_configure_presets()
        for profile in profiles.values():
            assert profile.default_preset in profile.presets
            assert set(profile.presets).issubset(presets)

    def test_repository_profile_orders_presets_for_interactive_selection(self) -> None:
        profile = build_config.load_profiles()['windows-msvc-x64']
        assert profile.presets == ('Win64-Debug-DurinEditor-Tests', 'Win64-Debug-DurinEditor', 'Win64-Release-DurinEditor', 'Win64-Release-DurinEditor-Profiling', 'Win64-Debug-DurinGame', 'Win64-Release-DurinGame', 'Win64-Release-DurinGame-Profiling', 'Win64-Shipping-DurinGame')

    def test_profiling_presets_are_release_isolated_and_enable_tracy(self) -> None:
        presets = build_config.load_configure_presets()
        for runtime_variant in ('DurinEditor', 'DurinGame'):
            preset = presets[f'Win64-Release-{runtime_variant}-Profiling']
            assert build_config.preset_cache_string(preset, 'CMAKE_BUILD_TYPE') == 'Release'
            assert build_config.preset_cache_string(preset, 'DURIN_RUNTIME_VARIANT') == runtime_variant
            assert build_config.preset_cache_string(preset, 'DURIN_PRESET_ROLE') == 'Profiling'
            assert build_config.preset_cache_bool(preset, 'DURIN_ENABLE_TRACY')
            assert build_config.preset_output_configuration(preset) == 'Release-Profiling'

    def test_fast_configure_is_code_model_only_and_not_buildtool_owned(self) -> None:
        profiles = build_config.load_profiles()
        presets = build_config.load_configure_presets()
        preset_name = 'Win64-Debug-DurinEditor-FastConfigure'
        assert build_config.preset_cache_bool(presets[preset_name], 'DURIN_IDE_CODE_MODEL_ONLY')
        for profile in profiles.values():
            assert preset_name not in profile.presets

    def test_cmake_preset_inheritance_is_resolved(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        path = Path(directory) / 'CMakePresets.json'
        path.write_text(json.dumps({'configurePresets': [{'name': 'base', 'binaryDir': '${sourceDir}/Build/${presetName}', 'cacheVariables': {'CMAKE_BUILD_TYPE': 'Debug', 'BUILD_TESTING': 'OFF'}}, {'name': 'tests', 'inherits': 'base', 'cacheVariables': {'BUILD_TESTING': 'ON'}}]}), encoding='utf-8')
        presets = build_config.load_configure_presets(path)
        assert build_config.preset_cache_string(presets['tests'], 'CMAKE_BUILD_TYPE') == 'Debug'
        assert build_config.preset_cache_bool(presets['tests'], 'BUILD_TESTING')

    def test_profile_precedence_and_host_validation(self) -> None:
        profiles = {'default': build_config.BuildProfile('default', 'windows', 'debug', ('debug',), build_config.EnvironmentProvider.INHERIT, 'Win64', '.exe', True, ()), 'other': build_config.BuildProfile('other', 'windows', 'debug', ('debug',), build_config.EnvironmentProvider.INHERIT, 'Win64', '.exe', False, ())}
        selected = build_config.select_profile(profiles, requested='other', environment={build_config.PROFILE_ENV_VAR: 'default'}, current_host='windows')
        assert selected.name == 'other'
        with pytest.raises(build_config.BuildToolError, match='current host'):
            build_config.select_profile(profiles, requested='other', current_host='linux')

    def test_job_precedence_and_cpu_fallback(self) -> None:
        assert build_config.resolve_jobs(3, 6, environment={}, cpu_count=20) == 3
        assert build_config.resolve_jobs(None, 6, environment={build_config.JOBS_ENV_VAR: '4'}, cpu_count=20) == 4
        assert build_config.resolve_jobs(None, 6, environment={}, cpu_count=20) == 6
        assert build_config.resolve_jobs(None, 0, environment={}, cpu_count=20) == 18

    def test_invalid_job_environment_is_rejected(self) -> None:
        with pytest.raises(build_config.BuildToolError, match=build_config.JOBS_ENV_VAR):
            build_config.resolve_jobs(None, 0, environment={build_config.JOBS_ENV_VAR: 'many'})

    def test_unknown_preset_is_rejected_with_available_values(self) -> None:
        profile = next(iter(build_config.load_profiles().values()))
        presets = build_config.load_configure_presets()
        with pytest.raises(build_config.BuildToolError, match='Available presets'):
            build_config.select_preset(profile, presets, requested='missing')

    def test_output_configuration_uses_preset_role(self) -> None:
        standard = build_config.ConfigurePreset('debug', {'cacheVariables': {'CMAKE_BUILD_TYPE': 'Debug'}})
        profiling = build_config.ConfigurePreset('profiling', {'cacheVariables': {'CMAKE_BUILD_TYPE': 'Release', 'DURIN_PRESET_ROLE': 'Profiling'}})
        assert build_config.preset_output_configuration(standard) == 'Debug'
        assert build_config.preset_output_configuration(profiling) == 'Release-Profiling'

    def test_explicit_cmake_path_takes_precedence(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        requested = Path(directory) / 'cmake.exe'
        requested.touch()
        resolved = build_config.resolve_cmake_command(str(requested), 'configured', environment={'DURIN_CMAKE_COMMAND': 'environment'})
        assert Path(resolved) == requested.resolve()

    def test_preset_build_path_cannot_escape_checkout(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        preset = build_config.ConfigurePreset('escape', {'binaryDir': '${sourceDir}/../outside'})
        directory = tmp_path_factory.mktemp('case')
        with pytest.raises(build_config.BuildToolError, match='inside the checkout'):
            build_config.preset_build_directory(preset, root=Path(directory))

class TestRepositoryConfig:

    def test_repository_config_resolves_tracked_layout(self) -> None:
        config = configuration.load_repository_config(REPO_ROOT)
        assert config.paths.local_build_config == Path('.agents/DevTool.user.json')
        assert config.paths.local_build_config_template == Path(
            'Templates/DurinDevTool/DevTool.user.json'
        )
        assert config.resolve(config.paths.cmake_presets).is_file()
        assert config.resolve(config.paths.build_profiles).is_file()
        assert config.resolve(config.paths.local_build_config_template).is_file()
        assert config.paths.scaffolding_templates == Path('Templates/Scaffolding')
        assert config.resolve(config.paths.scaffolding_templates).is_dir()
        assert config.feature_enabled('build')

    def test_repository_config_rejects_unknown_and_escaping_paths(
        self,
        tmp_path_factory: pytest.TempPathFactory,
    ) -> None:
        directory = Path(tmp_path_factory.mktemp('case'))
        source = json.loads(
            (REPO_ROOT / configuration.CONFIG_RELATIVE_PATH).read_text(encoding='utf-8')
        )
        config_path = directory / 'DevTool.json'
        source['unexpected'] = True
        config_path.write_text(json.dumps(source), encoding='utf-8')
        with pytest.raises(configuration.RepositoryConfigError, match='unknown field'):
            configuration.load_repository_config(directory, path=config_path)
        del source['unexpected']
        source['paths']['stateDirectory'] = '../outside'
        config_path.write_text(json.dumps(source), encoding='utf-8')
        with pytest.raises(configuration.RepositoryConfigError, match='inside the repository'):
            configuration.load_repository_config(directory, path=config_path)

    def test_repository_config_requires_explicit_boolean_features(
        self,
        tmp_path_factory: pytest.TempPathFactory,
    ) -> None:
        directory = Path(tmp_path_factory.mktemp('case'))
        source = json.loads(
            (REPO_ROOT / configuration.CONFIG_RELATIVE_PATH).read_text(encoding='utf-8')
        )
        source['features']['documentation'] = 'yes'
        config_path = directory / 'DevTool.json'
        config_path.write_text(json.dumps(source), encoding='utf-8')
        with pytest.raises(configuration.RepositoryConfigError, match='must be a boolean'):
            configuration.load_repository_config(directory, path=config_path)


class TestCMakeCodeModelGuard:

    def test_code_model_guard_fails_before_target_command_runs(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        local_config = build_config.load_local_config()
        cmake = local_config.cmake_command or shutil.which('cmake')
        if not cmake:
            pytest.skip('CMake is not available')
        environment = dict(os.environ)
        ninja = preflight.find_ninja(environment)
        if not ninja and os.name == 'nt':
            script, arguments = preflight.configured_visual_studio_environment(REPO_ROOT)
            script = script or preflight.find_vsdevcmd(environment)
            environment = preflight.capture_visual_studio_environment(script, arguments)
            ninja = preflight.find_ninja(environment)
        if not ninja:
            pytest.skip('Ninja is not available')
        build_options = (REPO_ROOT / 'CMake' / 'Config' / 'BuildOptions.cmake').as_posix()
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        source = root / 'source'
        build = root / 'build'
        module = source / 'Module'
        module.mkdir(parents=True)
        (source / 'CMakeLists.txt').write_text('\n'.join(['cmake_minimum_required(VERSION 3.24)', 'project(CodeModelGuard NONE)', f'include("{build_options}")', 'add_subdirectory(Module)', 'durin_enforce_code_model_only_build()']), encoding='utf-8')
        (module / 'CMakeLists.txt').write_text('\n'.join(['add_custom_target(WouldBuild', '  COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_BINARY_DIR}/target-ran"', ')']), encoding='utf-8')
        configure = subprocess.run([cmake, '-S', str(source), '-B', str(build), '-G', 'Ninja', f'-DCMAKE_MAKE_PROGRAM={Path(ninja).as_posix()}', '-DDURIN_IDE_CODE_MODEL_ONLY=ON'], capture_output=True, text=True, check=False)
        assert configure.returncode == 0, configure.stdout + configure.stderr
        guarded_build = subprocess.run([cmake, '--build', str(build), '--target', 'WouldBuild'], capture_output=True, text=True, check=False)
        output = guarded_build.stdout + guarded_build.stderr
        assert guarded_build.returncode != 0, output
        assert 'This IDE preset is code-model-only and cannot build' in output
        assert not (build / 'target-ran').exists()
