import argparse
import json
import multiprocessing
import os
import queue
import sys
import tempfile
import time
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

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
from durin_header_tool.model.export_info import ModuleExportManifest
from durin_header_tool.model.reflection_manifest import ModuleManifest
from durin_header_tool.runtime.worker_context import initialize_worker_config
from durin_header_tool.runtime.parallelism import resolve_worker_count


def _lock_worker(lock_path: str, operation: str, release_event, result_queue) -> None:
    with utils.acquire_output_lock(Path(lock_path), operation):
        result_queue.put((operation, "acquired"))
        release_event.wait(10)
    result_queue.put((operation, "released"))


class AtomicFileTests(unittest.TestCase):
    def test_generate_file_replaces_content_and_skips_unchanged_write(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            output_path = Path(temp_dir) / "generated.txt"
            utils.generate_file(output_path, "first")
            first_mtime = output_path.stat().st_mtime_ns

            utils.generate_file(output_path, "first")
            self.assertEqual(output_path.stat().st_mtime_ns, first_mtime)

            utils.generate_file(output_path, "second")
            self.assertEqual(output_path.read_text(encoding="utf-8"), "second")
            self.assertEqual(list(output_path.parent.glob(f".{output_path.name}.*.tmp")), [])

    def test_replace_failure_preserves_old_file_and_removes_temporary_file(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            output_path = Path(temp_dir) / "generated.txt"
            output_path.write_text("old", encoding="utf-8")

            with mock.patch("durin_header_tool.io.file_helper.os.replace", side_effect=OSError("replace failed")):
                with self.assertRaisesRegex(OSError, "replace failed"):
                    utils.generate_file(output_path, "new")

            self.assertEqual(output_path.read_text(encoding="utf-8"), "old")
            self.assertEqual(list(output_path.parent.glob(f".{output_path.name}.*.tmp")), [])

    def test_invalid_utf8_output_is_replaced(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            output_path = Path(temp_dir) / "generated.txt"
            output_path.write_bytes(b"\xff\xfe")

            utils.generate_file(output_path, "recovered")

            self.assertEqual(output_path.read_text(encoding="utf-8"), "recovered")


class OutputLockTests(unittest.TestCase):
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
                self.assertEqual(results.get(timeout=5), ("first", "acquired"))
                second.start()
                with self.assertRaises(queue.Empty):
                    results.get(timeout=0.4)

                first_release.set()
                self.assertEqual(results.get(timeout=5), ("first", "released"))
                self.assertEqual(results.get(timeout=5), ("second", "acquired"))
                second_release.set()
                self.assertEqual(results.get(timeout=5), ("second", "released"))
            finally:
                first_release.set()
                second_release.set()
                first.join(timeout=5)
                second.join(timeout=5)
                if first.is_alive():
                    first.terminate()
                if second.is_alive():
                    second.terminate()

            self.assertEqual(first.exitcode, 0)
            self.assertEqual(second.exitcode, 0)

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
                self.assertEqual(acquired, {("first", "acquired"), ("second", "acquired")})
                release.set()
                released = {results.get(timeout=5), results.get(timeout=5)}
                self.assertEqual(released, {("first", "released"), ("second", "released")})
            finally:
                release.set()
                first.join(timeout=5)
                second.join(timeout=5)
                if first.is_alive():
                    first.terminate()
                if second.is_alive():
                    second.terminate()

            self.assertEqual(first.exitcode, 0)
            self.assertEqual(second.exitcode, 0)


class CacheRecoveryTests(unittest.TestCase):
    def test_tool_fingerprint_invalidates_reflection_manifest(self):
        old_manifest = ModuleManifest(module_name="Engine", tool_fingerprint="old")
        new_manifest = ModuleManifest(module_name="Engine", tool_fingerprint="new")

        self.assertTrue(reflection_manifest_contract_changed(old_manifest, new_manifest))

    def test_tool_fingerprint_invalidates_export_manifest(self):
        old_manifest = ModuleExportManifest(Module="Engine", ToolFingerprint="old")
        new_manifest = ModuleExportManifest(Module="Engine", ToolFingerprint="new")

        self.assertFalse(export_generator._is_export_current(old_manifest, new_manifest, True))

    def test_invalid_reflection_manifest_is_a_cache_miss(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            manifest_path = Path(temp_dir) / "Engine.manifest"
            manifest_path.write_text("{", encoding="utf-8")
            with mock.patch.object(reflection_generator.utils, "get_module_manifest_file_path", return_value=manifest_path):
                self.assertIsNone(reflection_generator._load_previous_manifest("Engine"))

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
                self.assertEqual(export_generator._load_previous_export("Engine"), (None, None))

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

        self.assertEqual(result, ["Public/Engine/Actor.h"])

    def test_reflection_failure_does_not_commit_manifest(self):
        with (
            mock.patch.object(reflection_generator, "_load_previous_manifest", return_value=None),
            mock.patch.object(
                reflection_generator,
                "make_new_module_manifest",
                return_value=ModuleManifest(module_name="Engine"),
            ),
            mock.patch.object(reflection_generator, "_write_reflection_files", side_effect=RuntimeError("generation failed")),
            mock.patch.object(reflection_generator, "save_module_manifest_file") as save_manifest,
        ):
            with self.assertRaisesRegex(RuntimeError, "generation failed"):
                reflection_generator.generate_reflection_files("Engine")

        save_manifest.assert_not_called()


class ModuleDependencyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
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
                "is_module_enabled_for_active_profile",
                side_effect=lambda module_name, profile_name: module_name in {"Optional", "OptionalLeaf"},
            ),
        ):
            dependencies = configs.collect_all_dependent_modules("Root", "DurinEditor")

        self.assertEqual(dependencies, {"Optional", "RequiredLeaf", "OptionalLeaf"})

    def test_disabled_optional_dependency_does_not_join_dependency_graph(self):
        root_config = SimpleNamespace(
            private_dependencies=[],
            public_dependencies=[],
            optional_private_dependencies=["Optional"],
            optional_public_dependencies=[],
        )

        with (
            mock.patch.object(configs.module_config, "get_module_config", return_value=root_config),
            mock.patch.object(configs.module_config, "is_module_enabled_for_active_profile", return_value=False),
        ):
            dependencies = configs.collect_all_dependent_modules("Root", "DurinGame")

        self.assertEqual(dependencies, set())

    def test_launch_reflection_exports_follow_active_profile(self):
        editor_exports = configs.collect_all_dependent_module_with_export_file("Launch", "DurinEditor")
        game_exports = configs.collect_all_dependent_module_with_export_file("Launch", "DurinGame")

        self.assertIn("DurinEd", editor_exports)
        self.assertNotIn("DurinEd", game_exports)


class IntermediateLayoutTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        configs.ARCH = "Win64"
        configs.PROFILE_NAME = "DurinEditor"
        configs.BUILD_IDENTIFIER = ""
        configs.TOOL_FINGERPRINT = ""
        configs.init_configs()

    def tearDown(self):
        configs.ARCH = "Win64"
        configs.PROFILE_NAME = "DurinEditor"
        configs.BUILD_IDENTIFIER = ""
        configs.TOOL_FINGERPRINT = ""

    def test_empty_identifier_uses_shared_user_intermediate_path(self):
        configs.BUILD_IDENTIFIER = ""
        self.assertEqual(
            utils.get_project_intermediate_build_dir("Engine"),
            utils.get_project_intermediate_dir("Engine") / "Build" / "Win64" / "DurinEditor",
        )

    def test_agent_identifier_uses_isolated_intermediate_path_and_locks(self):
        configs.BUILD_IDENTIFIER = "Agent"
        self.assertEqual(
            utils.get_project_intermediate_build_dir("Engine"),
            utils.get_project_intermediate_dir("Engine") / "Build-Agent" / "Win64" / "DurinEditor",
        )
        self.assertEqual(
            utils.get_dht_module_lock_file_path("Core"),
            utils.get_project_intermediate_dir("Engine")
            / "Build-Agent"
            / ".dht-locks"
            / "Win64"
            / "DurinEditor"
            / "modules"
            / "Core.lock",
        )

    def test_custom_identifier_selects_its_own_intermediate_root(self):
        configs.BUILD_IDENTIFIER = "CI.2"
        self.assertEqual(
            utils.get_project_intermediate_build_dir("Engine"),
            utils.get_project_intermediate_dir("Engine") / "Build-CI.2" / "Win64" / "DurinEditor",
        )
        self.assertEqual(utils.get_dht_profile_lock_file_path().name, "profile.lock")

    def test_module_locks_are_independent_within_a_profile(self):
        self.assertNotEqual(
            utils.get_dht_module_lock_file_path("Core"),
            utils.get_dht_module_lock_file_path("Engine"),
        )

    def test_module_generation_commands_share_their_module_lock(self):
        export_args = SimpleNamespace(function="generate_module_export_file", module="Core")
        reflection_args = SimpleNamespace(function="generate_reflection_files", module="Core")

        self.assertEqual(_get_output_lock_paths(export_args), _get_output_lock_paths(reflection_args))
        self.assertEqual(_get_output_lock_paths(export_args), [utils.get_dht_module_lock_file_path("Core")])

    def test_project_preparation_locks_metadata_and_all_owned_modules(self):
        project_file = configs.environment.DURIN_ENGINE_PROJECT_DIR / "Engine.dproject"
        lock_paths = _get_output_lock_paths(
            SimpleNamespace(function="prepare_project_build", project=project_file)
        )
        engine_config = configs.get_project_config("Engine")

        self.assertEqual(lock_paths[0], utils.get_dht_project_lock_file_path("Engine"))
        self.assertEqual(
            lock_paths[1:],
            [
                utils.get_dht_module_lock_file_path(module_name)
                for module_name in sorted(engine_config.modules)
            ],
        )

    def test_platform_and_profile_remain_independent_dimensions(self):
        configs.ARCH = "Linux"
        configs.PROFILE_NAME = "DurinGame"
        self.assertEqual(
            utils.get_project_intermediate_build_dir("Engine"),
            utils.get_project_intermediate_dir("Engine") / "Build" / "Linux" / "DurinGame",
        )

    def test_cli_allows_identifier_to_be_omitted(self):
        parser = argparse.ArgumentParser()
        add_common_arguments(parser)
        args = parser.parse_args([])
        self.assertEqual(args.build_identifier, "")

    def test_cli_accepts_tool_fingerprint(self):
        parser = argparse.ArgumentParser()
        add_common_arguments(parser)
        args = parser.parse_args(["--tool-fingerprint", "abc123"])
        self.assertEqual(args.tool_fingerprint, "abc123")

    def test_cli_accepts_bounded_worker_count(self):
        parser = argparse.ArgumentParser()
        add_common_arguments(parser)
        self.assertEqual(parser.parse_args(["--workers", "2"]).workers, 2)
        with self.assertRaises(SystemExit):
            parser.parse_args(["--workers", "9"])

    def test_worker_parallelism_requires_a_large_task_set(self):
        self.assertEqual(resolve_worker_count(7, 8), 1)
        self.assertEqual(resolve_worker_count(8, 2), 2)
        self.assertEqual(resolve_worker_count(15, 8), 2)
        self.assertEqual(resolve_worker_count(16, 8), 4)
        self.assertEqual(resolve_worker_count(31, 8), 4)
        self.assertEqual(resolve_worker_count(32, 8), 8)
        self.assertEqual(resolve_worker_count(32, 4), 4)

    def test_cli_rejects_invalid_identifier(self):
        parser = argparse.ArgumentParser()
        add_common_arguments(parser)
        with self.assertRaises(SystemExit):
            parser.parse_args(["--build-identifier", "../Agent"])

    def test_worker_receives_identifier(self):
        with mock.patch.object(configs, "init_configs") as init_configs:
            initialize_worker_config("Win64", "DurinEditor", "Agent")

        self.assertEqual(configs.ARCH, "Win64")
        self.assertEqual(configs.PROFILE_NAME, "DurinEditor")
        self.assertEqual(configs.BUILD_IDENTIFIER, "Agent")
        init_configs.assert_called_once_with()

    def test_generated_project_metadata_uses_identifier_path(self):
        configs.BUILD_IDENTIFIER = "Agent"
        with mock.patch.object(project_cmake_file_generator.utils, "generate_file") as generate_file:
            project_cmake_file_generator.generate_project_cmake_file("Engine")

        output_path, content = generate_file.call_args.args
        expected_root = utils.get_project_intermediate_dir("Engine") / "Build-Agent" / "Win64" / "DurinEditor"
        self.assertEqual(output_path, expected_root / "Engine.project.cmake")
        self.assertIn(expected_root.as_posix(), content)

    def test_cmake_commands_forward_build_identifier(self):
        workspace_root = ROOT.parents[3]
        project_setup = (workspace_root / "CMake" / "Project" / "ProjectSetup.cmake").read_text(encoding="utf-8")
        project_targets = (workspace_root / "CMake" / "Project" / "ProjectTargets.cmake").read_text(encoding="utf-8")
        self.assertIn("list(APPEND DURIN_DHT_CONTEXT_ARGS --build-identifier ${DURIN_BUILD_IDENTIFIER})", project_setup)
        self.assertNotIn("--config ${CMAKE_BUILD_TYPE}", project_setup)
        self.assertEqual(project_targets.count("${DURIN_DHT_CONTEXT_ARGS}"), 2)
        self.assertNotIn('--build-identifier "${DURIN_BUILD_IDENTIFIER}"', project_setup + project_targets)

    def test_generated_module_metadata_leaves_source_discovery_to_cmake(self):
        with mock.patch.object(module_cmake_file_generator.utils, "generate_file") as generate_file:
            module_cmake_file_generator.generate_module_cmake_file("Engine")

        _, content = generate_file.call_args.args
        self.assertNotIn("module_public_srcs", content)
        self.assertNotIn("module_private_srcs", content)
        self.assertIn("module_export_manifest_file", content)
        self.assertIn("module_manifest_file", content)
        self.assertIn("module_reflection_export_dependencies", content)

    def test_cmake_declares_tool_and_generated_file_contracts(self):
        workspace_root = ROOT.parents[3]
        build_options = (workspace_root / "CMake" / "Config" / "BuildOptions.cmake").read_text(encoding="utf-8")
        project_setup = (workspace_root / "CMake" / "Project" / "ProjectSetup.cmake").read_text(encoding="utf-8")
        project_targets = (workspace_root / "CMake" / "Project" / "ProjectTargets.cmake").read_text(encoding="utf-8")

        self.assertIn("DURIN_DHT_TOOL_FINGERPRINT_FILE", project_setup)
        self.assertIn("--tool-fingerprint ${DURIN_DHT_TOOL_FINGERPRINT}", project_setup)
        self.assertEqual(project_targets.count("\n\t\t\tBYPRODUCTS "), 2)
        self.assertIn("GLOB_RECURSE module_public_srcs CONFIGURE_DEPENDS", project_targets)
        self.assertIn("GLOB_RECURSE module_private_srcs CONFIGURE_DEPENDS", project_targets)
        self.assertIn(".export.stamp", project_targets)
        self.assertIn(".reflection.stamp", project_targets)
        self.assertIn("JOB_POOLS durin_dht=${DURIN_DHT_JOB_POOL_SIZE}", build_options)
        self.assertEqual(project_targets.count("JOB_POOL durin_dht"), 2)
        self.assertEqual(project_targets.count("--workers ${DURIN_DHT_WORKERS}"), 2)
        self.assertIn("set(DURIN_DHT_LOG_LEVEL INFO CACHE STRING", build_options)
        self.assertEqual(project_targets.count("--log ${DURIN_DHT_LOG_LEVEL}"), 2)


if __name__ == "__main__":
    unittest.main()
