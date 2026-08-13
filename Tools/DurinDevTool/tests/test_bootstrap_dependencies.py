import pytest
import io
import json
import shutil
import sys
import zipfile
from pathlib import Path
from unittest import mock
REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
PRODUCT_ROOT = REPOSITORY_ROOT / 'Tools' / 'DurinDevTool'
if str(PRODUCT_ROOT) not in sys.path:
    sys.path.insert(0, str(PRODUCT_ROOT))
from durin_dev_tool.bootstrap import dependency_service, handler, installer as dependency_installer, manifests as dependency_manifests, sources as dependency_sources
from durin_dev_tool.bootstrap.models import BootstrapError, DependencyRequest
from durin_dev_tool.context import CommandIO, RepositoryContext

REPOSITORY = RepositoryContext.load(REPOSITORY_ROOT)

class TestThirdPartyBootstrap:

    @staticmethod
    def make_manifests() -> list[dict[str, object]]:
        return [{'name': 'normal', 'kind': 'direct_source', 'source_dir': 'normal', 'source': {'type': 'git', 'tag': 'v1'}}, {'name': 'tests', 'kind': 'direct_source', 'test_only': True, 'source_dir': 'tests', 'source': {'type': 'git', 'tag': 'v1'}}, {'name': 'tracy', 'kind': 'direct_source', 'development_only': True, 'source_dir': 'tracy', 'source': {'type': 'git', 'tag': 'v0.13.1'}}, {'name': 'tracy-tools', 'kind': 'tool_package', 'development_only': True, 'allow_unsupported_platform': True, 'source_dir': 'tracy-tools', 'source': {'type': 'archive', 'platforms': {'Win64': {'url': 'https://example.invalid/tracy-tools.zip', 'archive_name': 'tracy-tools.zip', 'required_files': ['tracy-profiler.exe']}}}}]

    def test_all_excludes_development_dependencies_by_default(self) -> None:
        selected = dependency_manifests.select_manifests(self.make_manifests(), DependencyRequest(use_all=True))
        assert [manifest['name'] for manifest in selected] == ['normal']

    def test_all_can_include_development_and_test_dependencies(self) -> None:
        selected = dependency_manifests.select_manifests(self.make_manifests(), DependencyRequest(use_all=True, with_tests=True, with_development=True))
        assert [manifest['name'] for manifest in selected] == ['normal', 'tests', 'tracy', 'tracy-tools']

    def test_dependency_plan_records_only_install_configurations(self) -> None:
        plan = dependency_manifests.plan_dependencies(
            self.make_manifests(),
            DependencyRequest(use_all=True, config='All'),
        )
        assert [(item.name, item.configurations) for item in plan] == [('normal', ())]

    def test_missing_platform_entries_are_enumerated_without_inventing_artifacts(self) -> None:
        manifest = {
            'name': 'slang',
            'kind': 'prebuilt_sdk',
            'source_dir': 'slang',
            'source': {'type': 'archive', 'platforms': {'Win64': {}}},
            'required_files_by_platform': {'Win64': ['bin/slang.dll']},
        }
        assert dependency_manifests.missing_platform_entries([manifest], 'MacOS') == (
            'slang: source.platforms.MacOS, required_files_by_platform.MacOS',
        )

    def test_explicit_development_dependency_preserves_requested_order(self) -> None:
        selected = dependency_manifests.select_manifests(self.make_manifests(), DependencyRequest(libraries='tracy,normal'))
        assert [manifest['name'] for manifest in selected] == ['tracy', 'normal']

    def test_shared_install_commands_receive_toolchain_environment(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        root = Path(tmp_path_factory.mktemp('case'))
        manifest = {'name': 'library', 'kind': 'shared_install', 'cmake_dir': 'CMake', 'install_required_file_sets': {'Debug': [['include/library.h']]}}
        (root / 'CMake').mkdir()
        with mock.patch.object(dependency_sources, 'verify_any_required_file_set', side_effect=[False, True]), mock.patch.object(dependency_sources, 'run_command') as run:
            dependency_installer.install_shared_library(manifest, 'Win64', 'Debug', 'C:/cmake/bin/cmake.exe', repository=REPOSITORY.at_root(root), command_io=CommandIO.system(), environment={'PATH': 'ready'})
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
        with mock.patch.dict(
            dependency_service.os.environ,
            {'CMAKE_COMMAND': ''},
        ):
            assert dependency_service.configured_cmake_command(REPOSITORY.at_root(root)) == 'custom-cmake'

    def test_development_only_must_be_boolean(self) -> None:
        manifests = self.make_manifests()
        manifests[2]['development_only'] = 'yes'
        with pytest.raises(BootstrapError, match='must be a boolean'):
            dependency_manifests.validate_manifests(manifests)

    @staticmethod
    def make_tool_manifest(*, sha256: str='0' * 64) -> dict[str, object]:
        return {'name': 'tracy-tools', 'version': '0.13.1', 'kind': 'tool_package', 'development_only': True, 'allow_unsupported_platform': True, 'repair_command': 'DevTool.bat dependency prepare --libs tracy,tracy-tools', 'source_dir': 'packages/tracy-tools/0.13.1/Win64', 'source': {'type': 'archive', 'platforms': {'Win64': {'url': 'https://example.invalid/windows-0.13.1.zip', 'archive_name': 'windows-0.13.1.zip', 'sha256': sha256, 'required_files': ['tracy-profiler.exe', 'tracy-capture.exe']}}}}

    def test_archive_sha256_must_be_64_hexadecimal_digits(self) -> None:
        manifest = self.make_tool_manifest(sha256='not-a-digest')
        with pytest.raises(BootstrapError, match='64 hexadecimal digits'):
            dependency_manifests.validate_manifests([manifest])

    def test_archive_digest_mismatch_does_not_publish_package(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        manifest = self.make_tool_manifest()
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        archive_path = root / 'source.zip'
        with zipfile.ZipFile(archive_path, 'w') as archive:
            archive.writestr('tracy-profiler.exe', b'profiler')
            archive.writestr('tracy-capture.exe', b'capture')
        with mock.patch.object(dependency_sources.urllib.request, 'urlretrieve', side_effect=lambda _url, destination: shutil.copy2(archive_path, destination)), pytest.raises(BootstrapError, match='integrity verification failed'):
            dependency_sources.ensure_archive_source(manifest, 'Win64', REPOSITORY.at_root(root), CommandIO.system())
        assert not (root / manifest['source_dir']).exists()

    def test_archive_digest_successfully_publishes_required_files(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        archive_path = root / 'source.zip'
        with zipfile.ZipFile(archive_path, 'w') as archive:
            archive.writestr('tracy-profiler.exe', b'profiler')
            archive.writestr('tracy-capture.exe', b'capture')
        manifest = self.make_tool_manifest(sha256=dependency_sources.compute_sha256(archive_path))
        with mock.patch.object(dependency_sources.urllib.request, 'urlretrieve', side_effect=lambda _url, destination: shutil.copy2(archive_path, destination)):
            dependency_installer.prepare_manifest(manifest, platform_name='Win64', configurations=['Debug', 'Release'], cmake_command='cmake', repository=REPOSITORY.at_root(root), command_io=CommandIO.system(), environment=None)
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
        with mock.patch.object(dependency_sources.urllib.request, 'urlretrieve') as urlretrieve:
            dependency_sources.ensure_archive_source(manifest, 'Win64', REPOSITORY.at_root(root), CommandIO.system())
        urlretrieve.assert_not_called()

    def test_optional_tool_package_skips_unsupported_platform(self) -> None:
        manifest = self.make_tool_manifest()
        output = io.StringIO()
        with mock.patch('sys.stdout', output):
            dependency_installer.prepare_manifest(manifest, platform_name='Linux', configurations=['Debug', 'Release'], cmake_command='cmake', repository=REPOSITORY, command_io=CommandIO.system(), environment=None)
        assert 'Skipping tracy-tools' in output.getvalue()

    def test_status_query_is_read_only_and_reports_missing_files(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        manifest = self.make_tool_manifest()
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        status = dependency_manifests.query_manifest_status(manifest, 'Win64', REPOSITORY.at_root(root))
        assert not status['prepared']
        assert status['version'] == '0.13.1'
        assert status['missing_files'] == ['tracy-profiler.exe', 'tracy-capture.exe']
        assert not (root / 'packages').exists()

class TestRelocatedManifest:

    def test_every_relocated_manifest_validates(self) -> None:
        manifests = dependency_manifests.load_manifests(REPOSITORY)
        dependency_manifests.validate_manifests(manifests)
        assert len(manifests) == 10

    def test_tracy_repair_command_is_focused_and_runnable(self) -> None:
        manifest = next((item for item in dependency_manifests.load_manifests(REPOSITORY) if item['name'] == 'tracy-tools'))
        assert manifest['repair_command'] == 'DevTool.bat dependency prepare --libs tracy,tracy-tools'
        assert (REPOSITORY_ROOT / 'DevTool.bat').is_file()
