import pytest
from pathlib import Path
from . import build_request_fixtures as request_fixtures
from durin_dev_tool.build import config as build_config
from durin_dev_tool.build import purge as build_purge


class TestCore:
    make_profile = staticmethod(request_fixtures.make_profile)
    make_preset = staticmethod(request_fixtures.make_preset)

    def test_purge_paths_cover_build_outputs_and_metadata(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        project = root / 'Engine'
        project.mkdir()
        (project / 'Engine.dproject').touch()
        paths = set(build_purge.collect_purge_paths(self.make_profile(), [self.make_preset()], root=root))
        assert root / 'Build/debug' in paths
        assert root / 'Engine/Binaries/Win64/Debug' in paths
        assert root / 'Engine/Binaries/Win64/Debug/ThirdParty' in paths
        assert root / 'Engine/Binaries/Win64/ThirdParty/Debug' in paths
        assert root / 'Engine/Intermediate/Build/Win64/DurinEditor' in paths

    def test_profiling_purge_reuses_release_third_party_directory(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        root = Path(tmp_path_factory.mktemp('case'))
        project = root / 'Engine'
        project.mkdir()
        (project / 'Engine.dproject').touch()
        values = dict(self.make_preset(name='profiling').values)
        cache = dict(values['cacheVariables'])
        cache['CMAKE_BUILD_TYPE'] = 'Release'
        cache['DURIN_PRESET_ROLE'] = 'Profiling'
        preset = build_config.ConfigurePreset('profiling', {**values, 'cacheVariables': cache})

        paths = set(build_purge.collect_purge_paths(self.make_profile(), [preset], root=root))

        assert root / 'Engine/Binaries/Win64/Release-Profiling' in paths
        assert root / 'Engine/Binaries/Win64/Release/ThirdParty' in paths
        assert root / 'Engine/Binaries/Win64/Release-Profiling/ThirdParty' not in paths
        assert root / 'Engine/Binaries/Win64/ThirdParty/Release' in paths

    def test_project_purge_removes_persistent_dht_cache(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        root = Path(tmp_path_factory.mktemp('case'))
        project = root / 'Engine'
        project.mkdir()
        (project / 'Engine.dproject').touch()
        cache_entry = project / 'Intermediate/Build/Win64/DurinEditor/DHTCache/Engine/export/entry.json'
        cache_entry.parent.mkdir(parents=True)
        cache_entry.write_text('{}', encoding='utf-8')

        paths = build_purge.collect_purge_paths(self.make_profile(), [self.make_preset()], root=root)
        build_purge.remove_purge_paths(paths, root=root)

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

        paths = build_purge.collect_purge_paths(self.make_profile(), presets, root=root)

        intermediate = project / 'Intermediate/Build/Win64/DurinEditor'
        assert paths.count(intermediate) == 1
    def test_purge_rejects_checkout_root(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        with pytest.raises(build_config.BuildToolError, match='checkout root'):
            build_purge.remove_purge_paths([root], root=root)
    def test_purge_removes_only_selected_artifact(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        artifact = root / 'Build' / 'debug'
        artifact.mkdir(parents=True)
        preserved = root / 'Build' / 'ThirdParty' / 'library.lib'
        preserved.parent.mkdir(parents=True)
        preserved.touch()
        build_purge.remove_purge_paths([artifact], root=root)
        assert not artifact.exists()
        assert preserved.exists()
