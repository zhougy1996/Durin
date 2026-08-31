import pytest
import importlib.util
import json
import os
import shutil
import subprocess
from pathlib import Path
from unittest import mock
REPO_ROOT = Path(__file__).resolve().parents[3]
DEV_TOOL_DIR = REPO_ROOT / 'Tools' / 'DurinDevTool'
from durin_dev_tool.build import config_io, errors, models, selection, settings, toolchain_context
from durin_dev_tool.bootstrap import preflight, toolchain_selection
from durin_dev_tool import toolchain
from durin_dev_tool import configuration
from durin_dev_tool.context import RepositoryContext
from durin_dev_tool.errors import DevToolError

BUILD_PATHS = settings.BuildPaths.from_repository(RepositoryContext.load(REPO_ROOT))

class TestBuildConfig:

    def test_legacy_config_facade_is_removed(self) -> None:
        assert importlib.util.find_spec('durin_dev_tool.build.config') is None

    def test_build_paths_are_derived_per_repository_context(self, tmp_path: Path) -> None:
        repository = RepositoryContext.load(REPO_ROOT)
        first = settings.BuildPaths.from_repository(repository.at_root(tmp_path / 'first'))
        second = settings.BuildPaths.from_repository(repository.at_root(tmp_path / 'second'))

        assert first.root == (tmp_path / 'first').resolve()
        assert second.root == (tmp_path / 'second').resolve()
        assert first.profile_file != second.profile_file
        for legacy_name in ('REPO_ROOT', 'REPOSITORY_CONFIG', 'PROFILE_FILE', 'PRESET_FILE', 'LOCAL_CONFIG_FILE', 'STATE_DIR', 'LOCK_DIR'):
            assert not hasattr(settings, legacy_name)

    def test_build_modules_import_without_loading_repository_context(self) -> None:
        script = '''
from durin_dev_tool.context import RepositoryContext
def fail(cls, repository_root=None):
    raise RuntimeError("repository context loaded during import")
RepositoryContext.load = classmethod(fail)
from durin_dev_tool.build import core, crash, locations, locking, settings
from durin_dev_tool.build import native_test_registry, opener, operations, process
from durin_dev_tool.build import purge, recovery, runtime
'''
        environment = dict(os.environ)
        environment['PYTHONPATH'] = str(DEV_TOOL_DIR)
        completed = subprocess.run(
            [os.sys.executable, '-c', script],
            cwd=REPO_ROOT,
            env=environment,
            capture_output=True,
            text=True,
            check=False,
        )
        assert completed.returncode == 0, completed.stderr

    def test_missing_config_uses_empty_overrides(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        config = config_io.load_local_config(Path(directory) / 'missing.json')
        assert config == models.LocalConfig()

    def test_repository_template_uses_automatic_defaults(self) -> None:
        config = config_io.load_local_config(
            REPO_ROOT / 'Templates' / 'DurinDevTool' / 'DevTool.user.json'
        )
        assert config == models.LocalConfig()

    def test_valid_config_uses_typed_models(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        path = Path(directory) / 'config.json'
        path.write_text(json.dumps({'version': 1, 'build': {'defaultProfile': 'windows-msvc-x64', 'parallelJobs': 8}, 'cmake': {'command': 'custom-cmake'}, 'toolchain': {'environmentScript': 'setup.cmd', 'environmentArguments': ['x64']}}), encoding='utf-8')
        config = config_io.load_local_config(path)
        assert config.cmake_command == 'custom-cmake'
        assert config.jobs == 8
        assert config.environment_setup.arguments == ('x64',)

    def test_invalid_json_and_field_types_are_rejected(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        path = Path(directory) / 'config.json'
        path.write_text('{', encoding='utf-8')
        with pytest.raises(errors.BuildToolError, match='malformed JSON'):
            config_io.load_local_config(path)
        path.write_text(json.dumps({'version': 1, 'cmake': {'command': 42}}), encoding='utf-8')
        with pytest.raises(errors.BuildToolError, match=r'cmake.*command.*null.*string'):
            config_io.load_local_config(path)
        path.write_text(json.dumps({'version': 1, 'build': {'parallelJobs': 257}}), encoding='utf-8')
        with pytest.raises(errors.BuildToolError, match=r'build.*parallelJobs'):
            config_io.load_local_config(path)
        path.write_text(json.dumps({'cmakeCommand': 'legacy-cmake'}), encoding='utf-8')
        with pytest.raises(errors.BuildToolError, match='unexpected.*cmakeCommand'):
            config_io.load_local_config(path)

    def test_repository_profiles_reference_existing_presets(self) -> None:
        profiles = config_io.load_profiles(BUILD_PATHS.profile_file)
        presets = config_io.load_configure_presets(BUILD_PATHS.preset_file)
        for profile in profiles.values():
            assert profile.default_preset in profile.presets
            assert set(profile.presets).issubset(presets)

    def test_repository_profile_orders_presets_for_interactive_selection(self) -> None:
        profile = config_io.load_profiles(BUILD_PATHS.profile_file)['windows-msvc-x64']
        assert profile.presets == (
            'Win64-Debug-DurinEditor',
            'Win64-Release-DurinEditor',
            'Win64-Release-DurinEditor-Profiling',
            'Win64-Debug-DurinGame',
            'Win64-Release-DurinGame',
            'Win64-Release-DurinGame-Profiling',
            'Win64-Shipping-DurinGame',
        )

    def test_macos_profile_keeps_application_tests_in_the_default_build_tree(self) -> None:
        profile = config_io.load_profiles(BUILD_PATHS.profile_file)['macos-xcode-arm64']
        assert profile.host == 'macos'
        assert profile.environment_provider is models.EnvironmentProvider.INHERIT
        assert profile.platform == 'MacOS'
        assert profile.default_preset == 'MacOS-arm64-Debug-DurinEditor'
        assert profile.presets == (
            'MacOS-arm64-Debug-DurinEditor',
            'MacOS-arm64-Release-DurinEditor',
        )
        assert profile.test_executable_suffix == ''
        assert {'ninja', 'clang', 'xcrun'} <= set(profile.required_commands)

    def test_repository_profile_presets_enable_native_tests(self) -> None:
        profiles = config_io.load_profiles(BUILD_PATHS.profile_file)
        presets = config_io.load_configure_presets(BUILD_PATHS.preset_file)
        for profile in profiles.values():
            for preset_name in profile.presets:
                assert selection.preset_cache_bool(
                    presets[preset_name], 'BUILD_TESTING'
                ), preset_name

    def test_macos_presets_keep_application_tests_explicit(self) -> None:
        manifest = json.loads((REPO_ROOT / 'CMakePresets.json').read_text(encoding='utf-8'))
        macos_presets = [
            preset for preset in manifest['configurePresets']
            if preset['name'].startswith('MacOS-')
        ]
        assert [preset['name'] for preset in macos_presets] == [
            'MacOS-arm64-Debug-DurinEditor',
            'MacOS-arm64-Release-DurinEditor',
        ]
        presets = config_io.load_configure_presets(BUILD_PATHS.preset_file)
        for name, build_type in (
            ('MacOS-arm64-Debug-DurinEditor', 'Debug'),
            ('MacOS-arm64-Release-DurinEditor', 'Release'),
        ):
            preset = presets[name]
            assert selection.preset_cache_string(preset, 'CMAKE_BUILD_TYPE') == build_type
            assert selection.preset_cache_string(preset, 'CMAKE_OSX_ARCHITECTURES') == 'arm64'
            assert selection.preset_cache_string(preset, 'DURIN_RUNTIME_VARIANT') == 'DurinEditor'
            assert not selection.preset_cache_bool(preset, 'DURIN_ENABLE_APPLICATION_TESTS')
        base = next(
            item for item in manifest['configurePresets'] if item['name'] == 'macos-base'
        )
        assert base['architecture']['value'] == 'arm64'
        assert base['cacheVariables']['BUILD_TESTING'] == 'ON'

    def test_profiling_presets_are_release_isolated_and_enable_tracy(self) -> None:
        presets = config_io.load_configure_presets(BUILD_PATHS.preset_file)
        for runtime_variant in ('DurinEditor', 'DurinGame'):
            preset = presets[f'Win64-Release-{runtime_variant}-Profiling']
            assert selection.preset_cache_string(preset, 'CMAKE_BUILD_TYPE') == 'Release'
            assert selection.preset_cache_string(preset, 'DURIN_RUNTIME_VARIANT') == runtime_variant
            assert selection.preset_cache_string(preset, 'DURIN_PRESET_ROLE') == 'Profiling'
            assert selection.preset_cache_bool(preset, 'DURIN_ENABLE_TRACY')
            assert selection.preset_output_configuration(preset) == 'Release-Profiling'

    def test_fast_configure_is_code_model_only_and_not_buildtool_owned(self) -> None:
        profiles = config_io.load_profiles(BUILD_PATHS.profile_file)
        presets = config_io.load_configure_presets(BUILD_PATHS.preset_file)
        preset_name = 'Win64-Debug-DurinEditor-FastConfigure'
        assert selection.preset_cache_bool(presets[preset_name], 'DURIN_IDE_CODE_MODEL_ONLY')
        for profile in profiles.values():
            assert preset_name not in profile.presets

    def test_cmake_preset_inheritance_is_resolved(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        path = Path(directory) / 'CMakePresets.json'
        path.write_text(json.dumps({'configurePresets': [{'name': 'base', 'binaryDir': '${sourceDir}/Build/${presetName}', 'cacheVariables': {'CMAKE_BUILD_TYPE': 'Debug', 'BUILD_TESTING': 'OFF'}}, {'name': 'tests', 'inherits': 'base', 'cacheVariables': {'BUILD_TESTING': 'ON'}}]}), encoding='utf-8')
        presets = config_io.load_configure_presets(path)
        assert selection.preset_cache_string(presets['tests'], 'CMAKE_BUILD_TYPE') == 'Debug'
        assert selection.preset_cache_bool(presets['tests'], 'BUILD_TESTING')

    def test_profile_precedence_and_host_validation(self) -> None:
        profiles = {'default': models.BuildProfile('default', 'windows', 'debug', ('debug',), models.EnvironmentProvider.INHERIT, 'Win64', '.exe', True, ()), 'other': models.BuildProfile('other', 'windows', 'debug', ('debug',), models.EnvironmentProvider.INHERIT, 'Win64', '.exe', False, ())}
        selected = selection.select_profile(profiles, requested='other', environment={settings.PROFILE_ENV_VAR: 'default'}, current_host='windows', profile_file=BUILD_PATHS.profile_file)
        assert selected.name == 'other'
        with pytest.raises(errors.BuildToolError, match='current host'):
            selection.select_profile(profiles, requested='other', current_host='linux', profile_file=BUILD_PATHS.profile_file)

    def test_job_precedence_and_cpu_fallback(self) -> None:
        assert toolchain_context.resolve_jobs(3, 6, environment={}, cpu_count=20) == 3
        assert toolchain_context.resolve_jobs(None, 6, environment={settings.JOBS_ENV_VAR: '4'}, cpu_count=20) == 4
        assert toolchain_context.resolve_jobs(None, 6, environment={}, cpu_count=20) == 6
        assert toolchain_context.resolve_jobs(None, 0, environment={}, cpu_count=20) == 18

    def test_invalid_job_environment_is_rejected(self) -> None:
        with pytest.raises(errors.BuildToolError, match=settings.JOBS_ENV_VAR):
            toolchain_context.resolve_jobs(None, 0, environment={settings.JOBS_ENV_VAR: 'many'})

    def test_unknown_preset_is_rejected_with_available_values(self) -> None:
        profile = next(iter(config_io.load_profiles(BUILD_PATHS.profile_file).values()))
        presets = config_io.load_configure_presets(BUILD_PATHS.preset_file)
        with pytest.raises(errors.BuildToolError, match='Available presets'):
            selection.select_preset(profile, presets, requested='missing', preset_file=BUILD_PATHS.preset_file)

    def test_output_configuration_uses_preset_role(self) -> None:
        standard = models.ConfigurePreset('debug', {'cacheVariables': {'CMAKE_BUILD_TYPE': 'Debug'}})
        profiling = models.ConfigurePreset('profiling', {'cacheVariables': {'CMAKE_BUILD_TYPE': 'Release', 'DURIN_PRESET_ROLE': 'Profiling'}})
        assert selection.preset_output_configuration(standard) == 'Debug'
        assert selection.preset_output_configuration(profiling) == 'Release-Profiling'

    def test_explicit_cmake_path_takes_precedence(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        requested = Path(directory) / 'cmake.exe'
        requested.touch()
        resolved = toolchain_context.resolve_cmake_command(str(requested), 'configured', environment={'DURIN_CMAKE_COMMAND': 'environment'})
        assert Path(resolved) == requested.resolve()

    def test_bare_cmake_command_uses_environment_path(self) -> None:
        with mock.patch.object(toolchain_context, 'find_command', return_value='custom/cmake') as which:
            resolved = toolchain_context.resolve_cmake_command(
                '',
                '',
                environment={'Path': 'custom-path'},
            )
        assert resolved == 'custom/cmake'
        which.assert_called_once_with('cmake', {'Path': 'custom-path'})

    def test_preset_build_path_cannot_escape_checkout(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        preset = models.ConfigurePreset('escape', {'binaryDir': '${sourceDir}/../outside'})
        directory = tmp_path_factory.mktemp('case')
        with pytest.raises(errors.BuildToolError, match='inside the checkout'):
            selection.preset_build_directory(preset, root=Path(directory))

class TestRepositoryConfig:

    def test_bootstrap_config_validators_match_editor_schemas(self) -> None:
        repository_schema = json.loads(
            (DEV_TOOL_DIR / 'DevTool.schema.json').read_text(encoding='utf-8')
        )
        assert set(repository_schema['required']) == configuration.CONFIG_FIELDS
        assert set(repository_schema['properties']) == (
            configuration.CONFIG_FIELDS | configuration.CONFIG_OPTIONAL_FIELDS
        )
        assert set(repository_schema['properties']['paths']['required']) == set(
            configuration.PATH_FIELDS
        )
        assert set(repository_schema['properties']['worktrees']['required']) == set(
            configuration.WORKTREE_PATH_FIELDS
        )

        local_schema = json.loads(
            (DEV_TOOL_DIR / 'DevTool.user.schema.json').read_text(encoding='utf-8')
        )
        assert set(local_schema['properties']) == config_io.LOCAL_CONFIG_FIELDS
        assert set(local_schema['properties']['build']['properties']) == (
            config_io.LOCAL_BUILD_FIELDS
        )
        assert set(local_schema['properties']['cmake']['properties']) == (
            config_io.LOCAL_CMAKE_FIELDS
        )
        assert set(local_schema['properties']['toolchain']['properties']) == (
            config_io.LOCAL_TOOLCHAIN_FIELDS
        )

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
        with pytest.raises(configuration.RepositoryConfigError, match='unexpected'):
            configuration.load_repository_config(directory, path=config_path)
        del source['unexpected']
        source['paths']['stateDirectory'] = '../outside'
        config_path.write_text(json.dumps(source), encoding='utf-8')
        with pytest.raises(configuration.RepositoryConfigError, match='inside the repository'):
            configuration.load_repository_config(directory, path=config_path)

    def test_repository_and_local_config_load_without_jsonschema(
        self, tmp_path_factory: pytest.TempPathFactory
    ) -> None:
        directory = Path(tmp_path_factory.mktemp('case'))
        local_config = directory / 'DevTool.user.json'
        local_config.write_text(
            json.dumps(
                {
                    'version': 1,
                    'build': {'parallelJobs': 'auto'},
                    'cmake': {'command': None},
                    'toolchain': {
                        'environmentScript': None,
                        'environmentArguments': [],
                    },
                }
            ),
            encoding='utf-8',
        )
        real_import = __import__

        def reject_jsonschema(name: str, *args: object, **kwargs: object) -> object:
            if name == 'jsonschema' or name.startswith('jsonschema.'):
                raise ModuleNotFoundError(name)
            return real_import(name, *args, **kwargs)

        with mock.patch('builtins.__import__', side_effect=reject_jsonschema):
            assert configuration.load_repository_config(REPO_ROOT).repository_root == REPO_ROOT
            assert config_io.load_local_config(local_config) == models.LocalConfig()

    @pytest.mark.parametrize(
        ('mutation', 'diagnostic'),
        (
            (lambda value: value.update({'unexpected': True}), 'unexpected'),
            (lambda value: value.update({'version': 2}), 'version'),
            (lambda value: value.update({'$schema': 1}), '\\$schema'),
            (
                lambda value: value.setdefault('build', {}).update(
                    {'parallelJobs': 0}
                ),
                'parallelJobs',
            ),
            (
                lambda value: value.setdefault('toolchain', {}).update(
                    {'environmentArguments': ['valid', 1]}
                ),
                'environmentArguments',
            ),
        ),
    )
    def test_local_config_bootstrap_validation_is_strict(
        self,
        tmp_path_factory: pytest.TempPathFactory,
        mutation: object,
        diagnostic: str,
    ) -> None:
        directory = Path(tmp_path_factory.mktemp('case'))
        path = directory / 'DevTool.user.json'
        value: dict[str, object] = {'version': 1}
        mutation(value)
        path.write_text(json.dumps(value), encoding='utf-8')
        with pytest.raises(errors.BuildToolError, match=diagnostic):
            config_io.load_local_config(path)



class TestCMakeCodeModelGuard:

    def test_code_model_guard_fails_before_target_command_runs(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        local_config = config_io.load_local_config(BUILD_PATHS.local_config_file)
        cmake = local_config.cmake_command or shutil.which('cmake')
        if not cmake:
            pytest.skip('CMake is not available')
        environment = dict(os.environ)
        ninja = preflight.find_ninja(environment)
        if not ninja and os.name == 'nt':
            try:
                script, arguments = toolchain_selection.configured_visual_studio_environment(REPO_ROOT)
                script = script or toolchain.find_vsdevcmd(environment)
                environment = toolchain.capture_windows_environment(script, arguments)
            except DevToolError as exc:
                pytest.skip(f'Visual Studio environment is not available: {exc}')
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
