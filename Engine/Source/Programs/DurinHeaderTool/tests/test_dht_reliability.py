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
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from durin_header_tool import io as utils
from durin_header_tool import config as configs
from durin_header_tool.cli.command import add_common_arguments
from durin_header_tool.generators import module_export_file_generator as export_generator
from durin_header_tool.generators import module_reflection_files_generator as reflection_generator
from durin_header_tool.generators import project_cmake_file_generator
from durin_header_tool.model.reflection_manifest import ModuleManifest
from durin_header_tool.runtime.worker_context import initialize_worker_config


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


class BuildIdentifierTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        configs.ARCH = "Win64"
        configs.PROFILE_NAME = "DurinEditor"
        configs.BUILD_IDENTIFIER = ""
        configs.init_configs()

    def tearDown(self):
        configs.ARCH = "Win64"
        configs.PROFILE_NAME = "DurinEditor"
        configs.BUILD_IDENTIFIER = ""

    def test_empty_identifier_preserves_existing_intermediate_path(self):
        configs.BUILD_IDENTIFIER = ""
        self.assertEqual(
            utils.get_project_intermediate_build_dir("Engine"),
            utils.get_project_intermediate_dir("Engine") / "Build" / "Win64" / "DurinEditor",
        )

    def test_agent_identifier_uses_isolated_intermediate_path_and_lock(self):
        configs.BUILD_IDENTIFIER = "Agent"
        self.assertEqual(
            utils.get_project_intermediate_build_dir("Engine"),
            utils.get_project_intermediate_dir("Engine") / "Build" / "Win64" / "DurinEditor" / "Agent",
        )
        self.assertEqual(utils.get_dht_output_lock_file_path().name, "Agent.lock")

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
        expected_root = utils.get_project_intermediate_dir("Engine") / "Build" / "Win64" / "DurinEditor" / "Agent"
        self.assertEqual(output_path, expected_root / "Engine.project.cmake")
        self.assertIn(expected_root.as_posix(), content)

    def test_cmake_commands_forward_build_identifier(self):
        workspace_root = ROOT.parents[3]
        project_setup = (workspace_root / "CMake" / "Project" / "ProjectSetup.cmake").read_text(encoding="utf-8")
        project_targets = (workspace_root / "CMake" / "Project" / "ProjectTargets.cmake").read_text(encoding="utf-8")
        self.assertEqual(project_setup.count('--build-identifier "${DURIN_BUILD_IDENTIFIER}"'), 1)
        self.assertEqual(project_targets.count('--build-identifier "${DURIN_BUILD_IDENTIFIER}"'), 2)


if __name__ == "__main__":
    unittest.main()
