import argparse
import json
import multiprocessing
import os
import queue
import sys
import tempfile
import time
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

import pytest

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from durin_header_tool import io as utils
from durin_header_tool import config as configs
from durin_header_tool.cli.command import add_common_arguments
from durin_header_tool.cli.main import _get_output_lock_paths
from durin_header_tool.generators import module_export_file_generator as export_generator
from durin_header_tool.generators import module_reflection_files_generator as reflection_generator
from durin_header_tool.generators import module_cmake_file_generator
from durin_header_tool.generators import project_cmake_file_generator
from durin_header_tool.cache.reflection_cache import reflection_manifest_contract_changed
from durin_header_tool.io import FileFingerprint
from durin_header_tool.model.export_info import ModuleExportInfo, ModuleExportManifest
from durin_header_tool.model.reflection_manifest import ModuleManifest
from durin_header_tool.runtime.worker_context import initialize_worker_config
from durin_header_tool.runtime.parallelism import resolve_worker_count


def _lock_worker(lock_path: str, operation: str, release_event, result_queue) -> None:
    with utils.acquire_output_lock(Path(lock_path), operation):
        result_queue.put((operation, "acquired"))
        release_event.wait(10)
    result_queue.put((operation, "released"))


class TestAtomicFile:
    def test_generate_file_replaces_content_and_skips_unchanged_write(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            output_path = Path(temp_dir) / "generated.txt"
            utils.generate_file(output_path, "first")
            first_mtime = output_path.stat().st_mtime_ns

            utils.generate_file(output_path, "first")
            assert output_path.stat().st_mtime_ns == first_mtime

            utils.generate_file(output_path, "second")
            assert output_path.read_text(encoding="utf-8") == "second"
            assert list(output_path.parent.glob(f".{output_path.name}.*.tmp")) == []

    def test_replace_failure_preserves_old_file_and_removes_temporary_file(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            output_path = Path(temp_dir) / "generated.txt"
            output_path.write_text("old", encoding="utf-8")

            with mock.patch("durin_header_tool.io.file_helper.os.replace", side_effect=OSError("replace failed")):
                with pytest.raises(OSError, match="replace failed"):
                    utils.generate_file(output_path, "new")

            assert output_path.read_text(encoding="utf-8") == "old"
            assert list(output_path.parent.glob(f".{output_path.name}.*.tmp")) == []

    def test_invalid_utf8_output_is_replaced(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            output_path = Path(temp_dir) / "generated.txt"
            output_path.write_bytes(b"\xff\xfe")

            utils.generate_file(output_path, "recovered")

            assert output_path.read_text(encoding="utf-8") == "recovered"


class TestOutputLock:
    def test_same_output_lock_serializes_processes(self):
        context = multiprocessing.get_context("spawn")
        with tempfile.TemporaryDirectory() as temp_dir:
            lock_path = str(Path(temp_dir) / "shared.lock")
            first_release = context.Event()
            second_release = context.Event()
            results = context.Queue()
            first = context.Process(target=_lock_worker, args=(lock_path, "first", first_release, results))
            second = context.Process(target=_lock_worker, args=(lock_path, "second", second_release, results))
            try:
                first.start()
                assert results.get(timeout=5) == ("first", "acquired")
                second.start()
                with pytest.raises(queue.Empty):
                    results.get(timeout=0.4)

                first_release.set()
                assert results.get(timeout=5) == ("first", "released")
                assert results.get(timeout=5) == ("second", "acquired")
                second_release.set()
                assert results.get(timeout=5) == ("second", "released")
            finally:
                first_release.set()
                second_release.set()
                first.join(timeout=5)
                second.join(timeout=5)
                if first.is_alive():
                    first.terminate()
                if second.is_alive():
                    second.terminate()

            assert first.exitcode == 0
            assert second.exitcode == 0

    def test_different_output_locks_can_be_acquired_together(self):
        context = multiprocessing.get_context("spawn")
        with tempfile.TemporaryDirectory() as temp_dir:
            release = context.Event()
            results = context.Queue()
            first = context.Process(
                target=_lock_worker,
                args=(str(Path(temp_dir) / "first.lock"), "first", release, results),
            )
            second = context.Process(
                target=_lock_worker,
                args=(str(Path(temp_dir) / "second.lock"), "second", release, results),
            )
            try:
                first.start()
                second.start()
                acquired = {results.get(timeout=5), results.get(timeout=5)}
                assert acquired == {("first", "acquired"), ("second", "acquired")}
                release.set()
                released = {results.get(timeout=5), results.get(timeout=5)}
                assert released == {("first", "released"), ("second", "released")}
            finally:
                release.set()
                first.join(timeout=5)
                second.join(timeout=5)
                if first.is_alive():
                    first.terminate()
                if second.is_alive():
                    second.terminate()

            assert first.exitcode == 0
            assert second.exitcode == 0


class TestCacheRecovery:
    def test_file_fingerprint_content_identity_ignores_timestamp_and_size(self):
        old_fingerprint = FileFingerprint(timestamp=1.0, file_size=10, md5="same-content")
        touched_fingerprint = FileFingerprint(timestamp=2.0, file_size=10, md5="same-content")
        changed_fingerprint = FileFingerprint(timestamp=2.0, file_size=11, md5="different-content")

        assert old_fingerprint == touched_fingerprint
        assert old_fingerprint != changed_fingerprint

    def test_touched_header_keeps_export_and_reflection_cache_current(self):
        header = "Public/Engine/Actor.h"
        old_fingerprint = FileFingerprint(timestamp=1.0, file_size=10, md5="same-content")
        touched_fingerprint = FileFingerprint(timestamp=2.0, file_size=10, md5="same-content")
        old_export_manifest = ModuleExportManifest(Module="Engine")
        new_export_manifest = ModuleExportManifest(Module="Engine")
        old_export_manifest.ReflectHeaders[header] = old_fingerprint
        new_export_manifest.ReflectHeaders[header] = touched_fingerprint

        assert export_generator._is_export_current(old_export_manifest, new_export_manifest, True)

        old_reflection_manifest = ModuleManifest(module_name="Engine")
        new_reflection_manifest = ModuleManifest(module_name="Engine")
        old_reflection_manifest.reflect_headers[header] = old_fingerprint
        new_reflection_manifest.reflect_headers[header] = touched_fingerprint
        old_reflection_manifest.resolved_symbol_dependencies[header] = {}
        with mock.patch.object(reflection_generator, "_generated_outputs_missing", return_value=False):
            headers = reflection_generator.get_reflection_headers_requiring_regeneration(
                "Engine",
                old_reflection_manifest,
                new_reflection_manifest,
            )

        assert headers == []

    def test_unchanged_zero_symbol_header_reuses_empty_export_result(self):
        header = "Public/Engine/Empty.h"
        fingerprint = FileFingerprint(timestamp=1.0, file_size=10, md5="content")
        old_manifest = ModuleExportManifest(Module="Engine", ReflectHeaders={header: fingerprint})
        new_manifest = ModuleExportManifest(Module="Engine", ReflectHeaders={header: fingerprint})
        old_export = ModuleExportInfo(Module="Engine")

        symbols = export_generator._load_or_parse_header_export(
            "Engine",
            header,
            old_manifest,
            new_manifest,
            old_export,
        )

        assert symbols == {}

    def test_tool_fingerprint_invalidates_reflection_manifest(self):
        old_manifest = ModuleManifest(module_name="Engine", tool_fingerprint="old")
        new_manifest = ModuleManifest(module_name="Engine", tool_fingerprint="new")

        assert reflection_manifest_contract_changed(old_manifest, new_manifest)

    def test_runtime_variant_invalidates_reflection_manifest(self):
        old_manifest = ModuleManifest(module_name="Engine", runtime_variant="DurinEditor")
        new_manifest = ModuleManifest(module_name="Engine", runtime_variant="DurinGame")

        assert reflection_manifest_contract_changed(old_manifest, new_manifest)

    def test_tool_fingerprint_invalidates_export_manifest(self):
        old_manifest = ModuleExportManifest(Module="Engine", ToolFingerprint="old")
        new_manifest = ModuleExportManifest(Module="Engine", ToolFingerprint="new")

        assert not export_generator._is_export_current(old_manifest, new_manifest, True)

    def test_invalid_reflection_manifest_is_a_cache_miss(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            manifest_path = Path(temp_dir) / "Engine.manifest"
            manifest_path.write_text("{", encoding="utf-8")
            with mock.patch.object(reflection_generator.utils, "get_module_manifest_file_path", return_value=manifest_path):
                assert reflection_generator._load_previous_manifest("Engine") is None

    def test_v3_reflection_manifest_derives_generated_output_ownership(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            manifest_path = Path(temp_dir) / "Engine.manifest"
            manifest_path.write_text(
                json.dumps(
                    {
                        "SchemaVersion": 3,
                        "ModuleName": "Engine",
                        "ReflectHeaders": {
                            "Public/Engine/Actor.h": {
                                "Timestamp": 0.0,
                                "FileSize": 0,
                                "MD5": "",
                            }
                        },
                        "DependencyExports": {},
                        "ResolvedSymbolDependencies": {},
                    }
                ),
                encoding="utf-8",
            )
            with mock.patch.object(reflection_generator.utils, "get_module_manifest_file_path", return_value=manifest_path):
                manifest = reflection_generator.load_module_manifest_file("Engine")

        assert manifest.generated_outputs == [
            "Actor.gen.cpp",
            "Actor.gen.h",
            "Engine.module.gen.cpp",
        ]
        assert manifest.pending_cleanup_outputs == []

    def test_invalid_generated_output_ownership_is_a_cache_miss(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            manifest_path = Path(temp_dir) / "Engine.manifest"
            manifest_path.write_text(
                json.dumps(
                    {
                        "SchemaVersion": 4,
                        "ModuleName": "Engine",
                        "ReflectHeaders": {},
                        "DependencyExports": {},
                        "ResolvedSymbolDependencies": {},
                        "GeneratedOutputs": ["../Actor.gen.h"],
                        "PendingCleanupOutputs": [],
                    }
                ),
                encoding="utf-8",
            )
            with mock.patch.object(reflection_generator.utils, "get_module_manifest_file_path", return_value=manifest_path):
                assert reflection_generator._load_previous_manifest("Engine") is None

    def test_invalid_export_manifest_discards_valid_export_cache(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            export_path = Path(temp_dir) / "Engine.export"
            manifest_path = Path(temp_dir) / "Engine.export.manifest"
            export_path.write_text(
                json.dumps({"SchemaVersion": 4, "Module": "Engine", "Symbols": {}}),
                encoding="utf-8",
            )
            manifest_path.write_text("{", encoding="utf-8")
            with (
                mock.patch.object(export_generator.utils, "get_module_export_file_path", return_value=export_path),
                mock.patch.object(export_generator.utils, "get_module_export_manifest_file_path", return_value=manifest_path),
            ):
                assert export_generator._load_previous_export("Engine") == (None, None)

    def test_missing_generated_header_forces_regeneration(self):
        old_manifest = ModuleManifest(module_name="Engine")
        new_manifest = ModuleManifest(module_name="Engine")
        old_manifest.reflect_headers["Public/Engine/Actor.h"] = mock.sentinel.fingerprint
        new_manifest.reflect_headers["Public/Engine/Actor.h"] = mock.sentinel.fingerprint
        old_manifest.resolved_symbol_dependencies["Public/Engine/Actor.h"] = {}

        with mock.patch.object(reflection_generator, "_generated_outputs_missing", return_value=True):
            result = reflection_generator.get_reflection_headers_requiring_regeneration(
                "Engine",
                old_manifest,
                new_manifest,
            )

        assert result == ["Public/Engine/Actor.h"]

    def test_reflection_failure_does_not_commit_manifest(self):
        save_manifest = mock.Mock()
        with (
            mock.patch.object(reflection_generator, "_load_previous_manifest", return_value=None),
            mock.patch.object(
                reflection_generator,
                "make_new_module_manifest",
                return_value=ModuleManifest(module_name="Engine"),
            ),
            mock.patch.object(reflection_generator, "_write_reflection_files", side_effect=RuntimeError("generation failed")),
            mock.patch.object(reflection_generator, "save_module_manifest_file", new=save_manifest),
        ):
            with pytest.raises(RuntimeError, match="generation failed"):
                reflection_generator.generate_reflection_files("Engine")

        save_manifest.assert_not_called()

    def test_removed_reflect_header_outputs_are_deleted_after_manifest_commit(self):
        old_manifest = ModuleManifest(
            module_name="Engine",
            generated_outputs=[
                "Actor.gen.cpp",
                "Actor.gen.h",
                "Engine.module.gen.cpp",
                "World.gen.cpp",
                "World.gen.h",
            ],
            pending_cleanup_outputs=["PreviouslyPending.gen.cpp"],
        )
        new_manifest = ModuleManifest(
            module_name="Engine",
            generated_outputs=[
                "Engine.module.gen.cpp",
                "World.gen.cpp",
                "World.gen.h",
            ],
        )

        with tempfile.TemporaryDirectory() as temp_dir:
            output_dir = Path(temp_dir)
            stale_names = ["Actor.gen.cpp", "Actor.gen.h", "PreviouslyPending.gen.cpp"]
            for output_name in stale_names + ["World.gen.h", "Unowned.gen.h"]:
                (output_dir / output_name).write_text("generated", encoding="utf-8")

            manifest_commits = []

            def record_manifest_commit(manifest):
                manifest_commits.append(
                    (
                        list(manifest.pending_cleanup_outputs),
                        [(output_dir / output_name).exists() for output_name in stale_names],
                    )
                )

            with (
                mock.patch.object(reflection_generator, "_load_previous_manifest", return_value=old_manifest),
                mock.patch.object(reflection_generator, "make_new_module_manifest", return_value=new_manifest),
                mock.patch.object(
                    reflection_generator,
                    "get_reflection_headers_requiring_regeneration",
                    return_value=[],
                ),
                mock.patch.object(reflection_generator, "_write_reflection_files", return_value=(0, 0)),
                mock.patch.object(reflection_generator, "save_module_manifest_file", side_effect=record_manifest_commit),
                mock.patch.object(reflection_generator.utils, "get_module_dht_output_dir", return_value=output_dir),
            ):
                reflection_generator.generate_reflection_files("Engine")

            assert manifest_commits == [
                (stale_names, [True, True, True]),
                ([], [False, False, False]),
            ]
            assert (output_dir / "World.gen.h").exists()
            assert (output_dir / "Unowned.gen.h").exists()

    def test_cleanup_failure_keeps_pending_outputs_in_committed_manifest(self):
        old_manifest = ModuleManifest(
            module_name="Engine",
            generated_outputs=["Actor.gen.h", "Engine.module.gen.cpp"],
        )
        new_manifest = ModuleManifest(
            module_name="Engine",
            generated_outputs=["Engine.module.gen.cpp"],
        )
        manifest_commits = []

        with (
            mock.patch.object(reflection_generator, "_load_previous_manifest", return_value=old_manifest),
            mock.patch.object(reflection_generator, "make_new_module_manifest", return_value=new_manifest),
            mock.patch.object(
                reflection_generator,
                "get_reflection_headers_requiring_regeneration",
                return_value=[],
            ),
            mock.patch.object(reflection_generator, "_write_reflection_files", return_value=(0, 0)),
            mock.patch.object(
                reflection_generator,
                "save_module_manifest_file",
                side_effect=lambda manifest: manifest_commits.append(list(manifest.pending_cleanup_outputs)),
            ),
            mock.patch.object(
                reflection_generator,
                "_cleanup_stale_generated_outputs",
                side_effect=OSError("cleanup failed"),
            ),
        ):
            with pytest.raises(OSError, match="cleanup failed"):
                reflection_generator.generate_reflection_files("Engine")

        assert manifest_commits == [["Actor.gen.h"]]


class TestModuleDependency:
    @classmethod
    def setup_class(cls):
        configs.init_configs()

    def test_enabled_optional_dependencies_join_recursive_dependency_graph(self):
        module_configs = {
            "Root": SimpleNamespace(
                private_dependencies=[],
                public_dependencies=[],
                optional_private_dependencies=["Optional"],
                optional_public_dependencies=[],
            ),
            "Optional": SimpleNamespace(
                private_dependencies=["RequiredLeaf"],
                public_dependencies=[],
                optional_private_dependencies=[],
                optional_public_dependencies=["OptionalLeaf"],
            ),
            "RequiredLeaf": SimpleNamespace(
                private_dependencies=[],
                public_dependencies=[],
                optional_private_dependencies=[],
                optional_public_dependencies=[],
            ),
            "OptionalLeaf": SimpleNamespace(
                private_dependencies=[],
                public_dependencies=[],
                optional_private_dependencies=[],
                optional_public_dependencies=[],
            ),
        }

        with (
            mock.patch.object(configs.module_config, "get_module_config", side_effect=module_configs.__getitem__),
            mock.patch.object(
                configs.module_config,
                "is_module_enabled_for_active_runtime_variant",
                side_effect=lambda module_name, runtime_variant: module_name in {"Optional", "OptionalLeaf"},
            ),
        ):
            dependencies = configs.collect_all_dependent_modules("Root", "DurinEditor")

        assert dependencies == {"Optional", "RequiredLeaf", "OptionalLeaf"}

    def test_disabled_optional_dependency_does_not_join_dependency_graph(self):
        root_config = SimpleNamespace(
            private_dependencies=[],
            public_dependencies=[],
            optional_private_dependencies=["Optional"],
            optional_public_dependencies=[],
        )

        with (
            mock.patch.object(configs.module_config, "get_module_config", return_value=root_config),
            mock.patch.object(
                configs.module_config,
                "is_module_enabled_for_active_runtime_variant",
                return_value=False,
            ),
        ):
            dependencies = configs.collect_all_dependent_modules("Root", "DurinGame")

        assert dependencies == set()

    def test_launch_reflection_exports_follow_active_runtime_variant(self):
        editor_exports = configs.collect_all_dependent_module_with_export_file("Launch", "DurinEditor")
        game_exports = configs.collect_all_dependent_module_with_export_file("Launch", "DurinGame")

        assert "DurinEd" in editor_exports
        assert "DurinEd" not in game_exports


class TestIntermediateLayout:
    @classmethod
    def setup_class(cls):
        configs.ARCH = "Win64"
        configs.RUNTIME_VARIANT = "DurinEditor"
        configs.TOOL_FINGERPRINT = ""
        configs.init_configs()

    def teardown_method(self):
        configs.ARCH = "Win64"
        configs.RUNTIME_VARIANT = "DurinEditor"
        configs.TOOL_FINGERPRINT = ""

    def test_intermediate_path_uses_platform_and_runtime_variant(self):
        assert (
            utils.get_project_intermediate_build_dir("Engine")
            ==
            utils.get_project_intermediate_dir("Engine") / "Build" / "Win64" / "DurinEditor"
        )

    def test_locks_use_shared_intermediate_root(self):
        assert utils.get_dht_module_lock_file_path("Core") == (
            utils.get_project_intermediate_dir("Engine")
            / "Build"
            / ".dht-locks"
            / "Win64"
            / "DurinEditor"
            / "modules"
            / "Core.lock"
        )

    def test_runtime_variant_lock_uses_shared_intermediate_root(self):
        assert utils.get_dht_runtime_variant_lock_file_path().name == "runtime-variant.lock"

    def test_module_locks_are_independent_within_a_runtime_variant(self):
        assert (
            utils.get_dht_module_lock_file_path("Core")
            !=
            utils.get_dht_module_lock_file_path("Engine")
        )

    def test_module_generation_commands_share_their_module_lock(self):
        export_args = SimpleNamespace(function="generate_module_export_file", module="Core")
        reflection_args = SimpleNamespace(function="generate_reflection_files", module="Core")

        assert _get_output_lock_paths(export_args) == _get_output_lock_paths(reflection_args)
        assert _get_output_lock_paths(export_args) == [utils.get_dht_module_lock_file_path("Core")]

    def test_project_preparation_locks_metadata_and_all_owned_modules(self):
        project_file = configs.environment.DURIN_ENGINE_PROJECT_DIR / "Engine.dproject"
        lock_paths = _get_output_lock_paths(
            SimpleNamespace(function="prepare_project_build", project=project_file)
        )
        engine_config = configs.get_project_config("Engine")

        assert lock_paths[0] == utils.get_dht_project_lock_file_path("Engine")
        assert lock_paths[1:] == [
            utils.get_dht_module_lock_file_path(module_name)
            for module_name in sorted(engine_config.modules)
        ]

    def test_platform_and_runtime_variant_remain_independent_dimensions(self):
        configs.ARCH = "Linux"
        configs.RUNTIME_VARIANT = "DurinGame"
        assert (
            utils.get_project_intermediate_build_dir("Engine")
            ==
            utils.get_project_intermediate_dir("Engine") / "Build" / "Linux" / "DurinGame"
        )

    def test_cli_accepts_tool_fingerprint(self):
        parser = argparse.ArgumentParser()
        add_common_arguments(parser)
        args = parser.parse_args(["--tool-fingerprint", "abc123"])
        assert args.tool_fingerprint == "abc123"

    def test_cli_accepts_bounded_worker_count(self):
        parser = argparse.ArgumentParser()
        add_common_arguments(parser)
        assert parser.parse_args(["--workers", "2"]).workers == 2
        with pytest.raises(SystemExit):
            parser.parse_args(["--workers", "9"])

    def test_worker_parallelism_requires_a_large_task_set(self):
        assert resolve_worker_count(7, 8) == 1
        assert resolve_worker_count(8, 2) == 2
        assert resolve_worker_count(15, 8) == 2
        assert resolve_worker_count(16, 8) == 4
        assert resolve_worker_count(31, 8) == 4
        assert resolve_worker_count(32, 8) == 8
        assert resolve_worker_count(32, 4) == 4

    def test_worker_receives_build_context(self):
        with mock.patch.object(configs, "init_configs") as init_configs:
            initialize_worker_config("Win64", "DurinEditor")

        assert configs.ARCH == "Win64"
        assert configs.RUNTIME_VARIANT == "DurinEditor"
        init_configs.assert_called_once_with()

    def test_generated_project_metadata_uses_shared_build_path(self):
        with mock.patch.object(project_cmake_file_generator.utils, "generate_file") as generate_file:
            project_cmake_file_generator.generate_project_cmake_file("Engine")

        output_path, content = generate_file.call_args.args
        expected_root = utils.get_project_intermediate_dir("Engine") / "Build" / "Win64" / "DurinEditor"
        assert output_path == expected_root / "Engine.project.cmake"
        assert expected_root.as_posix() in content
        assert (
            "${DURIN_PROJECT_BINARY_DIR}/${DURIN_ARCH}/ThirdParty/${DURIN_THIRDPARTY_OUTPUT_CONFIG}"
            in content
        )

    def test_cmake_commands_forward_shared_dht_context(self):
        workspace_root = ROOT.parents[3]
        project_setup = (workspace_root / "CMake" / "Project" / "ProjectSetup.cmake").read_text(encoding="utf-8")
        project_targets = (workspace_root / "CMake" / "Project" / "ProjectTargets.cmake").read_text(encoding="utf-8")
        assert "--config ${CMAKE_BUILD_TYPE}" not in project_setup
        assert project_targets.count("${DURIN_DHT_CONTEXT_ARGS}") == 2

    def test_generated_module_metadata_leaves_source_discovery_to_cmake(self):
        with mock.patch.object(module_cmake_file_generator.utils, "generate_file") as generate_file:
            module_cmake_file_generator.generate_module_cmake_file("Engine")

        _, content = generate_file.call_args.args
        assert "module_public_srcs" not in content
        assert "module_private_srcs" not in content
        assert "module_export_manifest_file" in content
        assert "module_manifest_file" in content
        assert "module_reflection_export_dependencies" in content

    def test_cmake_declares_tool_and_generated_file_contracts(self):
        workspace_root = ROOT.parents[3]
        build_options = (workspace_root / "CMake" / "Config" / "BuildOptions.cmake").read_text(encoding="utf-8")
        project_setup = (workspace_root / "CMake" / "Project" / "ProjectSetup.cmake").read_text(encoding="utf-8")
        project_targets = (workspace_root / "CMake" / "Project" / "ProjectTargets.cmake").read_text(encoding="utf-8")

        assert "DURIN_DHT_TOOL_FINGERPRINT_FILE" in project_setup
        assert "--tool-fingerprint ${DURIN_DHT_TOOL_FINGERPRINT}" in project_setup
        assert project_targets.count("\n\t\t\tBYPRODUCTS ") == 2
        assert "GLOB_RECURSE module_public_srcs CONFIGURE_DEPENDS" in project_targets
        assert "GLOB_RECURSE module_private_srcs CONFIGURE_DEPENDS" in project_targets
        assert ".export.stamp" in project_targets
        assert ".reflection.stamp" in project_targets
        assert "JOB_POOLS durin_dht=${DURIN_DHT_JOB_POOL_SIZE}" in build_options
        assert project_targets.count("JOB_POOL durin_dht") == 2
        assert project_targets.count("--workers ${DURIN_DHT_WORKERS}") == 2
        assert "set(DURIN_DHT_LOG_LEVEL INFO CACHE STRING" in build_options
        assert project_targets.count("--log ${DURIN_DHT_LOG_LEVEL}") == 2
