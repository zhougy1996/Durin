from __future__ import annotations

import argparse
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[3]


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Could not load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


agent_build = load_module("agent_build", REPO_ROOT / "Engine/Scripts/Build/agent_build.py")
agent_config = load_module("agent_config", REPO_ROOT / "Engine/Scripts/Bootstrap/agent_config.py")


class AgentBuildConfigTests(unittest.TestCase):
    def test_missing_config_uses_empty_overrides(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            config = agent_build.load_local_config(Path(directory) / "missing.json")

        self.assertEqual(config["cmakeCommand"], "")
        self.assertEqual(config["defaultBuildProfile"], "")
        self.assertEqual(config["jobs"], 0)
        self.assertEqual(config["environmentSetup"], {"script": "", "arguments": []})

    def test_valid_config_is_loaded(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "config.json"
            path.write_text(
                json.dumps(
                    {
                        "cmakeCommand": "custom-cmake",
                        "defaultBuildProfile": "windows-msvc-x64",
                        "jobs": 8,
                        "environmentSetup": {"script": "setup.cmd", "arguments": ["x64"]},
                    }
                ),
                encoding="utf-8",
            )
            config = agent_build.load_local_config(path)

        self.assertEqual(config["cmakeCommand"], "custom-cmake")
        self.assertEqual(config["jobs"], 8)
        self.assertEqual(config["environmentSetup"]["arguments"], ["x64"])

    def test_invalid_json_and_field_types_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "config.json"
            path.write_text("{", encoding="utf-8")
            with self.assertRaisesRegex(agent_build.AgentBuildError, "invalid JSON"):
                agent_build.load_local_config(path)

            path.write_text(json.dumps({"cmakeCommand": 42}), encoding="utf-8")
            with self.assertRaisesRegex(agent_build.AgentBuildError, "must be a string"):
                agent_build.load_local_config(path)

            path.write_text(json.dumps({"jobs": 257}), encoding="utf-8")
            with self.assertRaisesRegex(agent_build.AgentBuildError, "integer from 0 to 256"):
                agent_build.load_local_config(path)


class AgentBuildProfileTests(unittest.TestCase):
    def setUp(self) -> None:
        self.profiles = {
            "windows-default": {"host": "windows", "default": True},
            "windows-other": {"host": "windows", "default": False},
            "linux-default": {"host": "linux", "default": True},
        }

    def test_profile_precedence(self) -> None:
        name, _ = agent_build.select_profile(
            self.profiles,
            requested="windows-other",
            environment={agent_build.PROFILE_ENV_VAR: "windows-default"},
            configured="windows-default",
            current_host="windows",
        )
        self.assertEqual(name, "windows-other")

        name, _ = agent_build.select_profile(
            self.profiles,
            environment={agent_build.PROFILE_ENV_VAR: "windows-other"},
            configured="windows-default",
            current_host="windows",
        )
        self.assertEqual(name, "windows-other")

    def test_unknown_and_wrong_host_profiles_are_rejected(self) -> None:
        with self.assertRaisesRegex(agent_build.AgentBuildError, "Unknown"):
            agent_build.select_profile(self.profiles, requested="missing", environment={}, current_host="windows")
        with self.assertRaisesRegex(agent_build.AgentBuildError, "current host"):
            agent_build.select_profile(
                self.profiles, requested="linux-default", environment={}, current_host="windows"
            )

    def test_host_without_profile_does_not_fall_back(self) -> None:
        with self.assertRaisesRegex(agent_build.AgentBuildError, "No isolated"):
            agent_build.select_profile(self.profiles, environment={}, current_host="macos")

    def test_cmake_precedence_and_invalid_path(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            requested = Path(directory) / "requested-cmake"
            configured = Path(directory) / "configured-cmake"
            requested.touch()
            configured.touch()
            resolved = agent_build.resolve_cmake_command(
                str(requested), str(configured), environment={"DURIN_CMAKE_COMMAND": str(configured)}
            )
            self.assertEqual(Path(resolved), requested.resolve())

            with self.assertRaisesRegex(agent_build.AgentBuildError, "does not exist"):
                agent_build.resolve_cmake_command(str(Path(directory) / "missing"), "", environment={})

    def test_failed_generator_cache_is_not_reused(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory) / "CMakeCache.txt"
            cache.write_text(
                "CMAKE_MAKE_PROGRAM:FILEPATH=CMAKE_MAKE_PROGRAM-NOTFOUND\n",
                encoding="utf-8",
            )
            self.assertFalse(agent_build.cache_is_usable(cache))
            cache.write_text("CMAKE_MAKE_PROGRAM:FILEPATH=C:/Ninja/ninja.exe\n", encoding="utf-8")
            self.assertTrue(agent_build.cache_is_usable(cache))

    def test_job_precedence_and_cpu_fallback(self) -> None:
        self.assertEqual(
            agent_build.resolve_jobs(3, 6, environment={agent_build.JOBS_ENV_VAR: "4"}, cpu_count=20),
            3,
        )
        self.assertEqual(
            agent_build.resolve_jobs(None, 6, environment={agent_build.JOBS_ENV_VAR: "4"}, cpu_count=20),
            4,
        )
        self.assertEqual(agent_build.resolve_jobs(None, 6, environment={}, cpu_count=20), 6)
        self.assertEqual(agent_build.resolve_jobs(None, 0, environment={}, cpu_count=20), 18)
        self.assertEqual(agent_build.resolve_jobs(None, 0, environment={}, cpu_count=2), 1)

    def test_invalid_job_environment_value_is_rejected(self) -> None:
        with self.assertRaisesRegex(agent_build.AgentBuildError, agent_build.JOBS_ENV_VAR):
            agent_build.resolve_jobs(None, 0, environment={agent_build.JOBS_ENV_VAR: "many"})


class AgentBuildOperationTests(unittest.TestCase):
    def test_clean_and_rebuild_actions_are_parsed(self) -> None:
        clean = agent_build.parse_args(["Clean"])
        rebuild = agent_build.parse_args(["Rebuild"])
        self.assertEqual(clean.action, "Clean")
        self.assertEqual(rebuild.action, "Rebuild")
        self.assertEqual(rebuild.target, "")

    def test_rebuild_defaults_to_all_after_clean_and_fresh_configure(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            build_directory = Path(directory) / "Build" / "agent"
            build_directory.mkdir(parents=True)
            cache_file = build_directory / "CMakeCache.txt"
            cache_file.write_text("CMAKE_MAKE_PROGRAM:FILEPATH=C:/Ninja/ninja.exe\n", encoding="utf-8")
            args = argparse.Namespace(action="Rebuild", target="", filter="")
            with mock.patch.object(agent_build, "run_command") as run:
                agent_build.perform_action(
                    args,
                    cmake="cmake",
                    jobs=8,
                    environment={"PATH": ""},
                    build_directory=build_directory,
                    cache_file=cache_file,
                    preset="agent",
                    profile={},
                )

        self.assertEqual(
            [call.args[0] for call in run.call_args_list],
            [
                ["cmake", "--build", str(build_directory), "--target", "clean"],
                ["cmake", "--fresh", "--preset", "agent"],
                ["cmake", "--build", str(build_directory), "--target", "all", "-j", "8"],
            ],
        )

    def test_clean_is_a_noop_without_a_usable_cache(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            build_directory = Path(directory) / "Build" / "agent"
            args = argparse.Namespace(action="Clean", target="", filter="")
            with mock.patch.object(agent_build, "run_command") as run:
                agent_build.perform_action(
                    args,
                    cmake="cmake",
                    jobs=8,
                    environment={},
                    build_directory=build_directory,
                    cache_file=build_directory / "CMakeCache.txt",
                    preset="agent",
                    profile={},
                )
        run.assert_not_called()

    def test_same_profile_lock_is_exclusive_and_other_profiles_are_independent(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            first_path = agent_build.lock_file_path("profile-a", root)
            second_path = agent_build.lock_file_path("profile-b", root)
            metadata = {"pid": 1, "profile": "profile-a", "action": "Build"}
            with agent_build.AgentBuildLock(first_path, metadata):
                with self.assertRaisesRegex(agent_build.AgentBuildError, "already owns"):
                    with agent_build.AgentBuildLock(first_path, metadata):
                        pass
                with agent_build.AgentBuildLock(second_path, metadata):
                    pass

            with agent_build.AgentBuildLock(first_path, metadata):
                pass

    def test_interrupted_operation_blocks_build_until_rebuild_succeeds(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / "interrupted.json"
            metadata = {"pid": 1, "profile": "agent", "action": "Build"}

            def interrupt() -> None:
                raise agent_build.AgentBuildInterruptedError("interrupted")

            with self.assertRaises(agent_build.AgentBuildInterruptedError):
                agent_build.execute_with_recovery_marker(
                    action="Build", marker_file=marker, metadata=metadata, operation=interrupt
                )
            self.assertTrue(marker.is_file())

            with self.assertRaisesRegex(agent_build.AgentBuildError, "Rebuild --target all"):
                agent_build.execute_with_recovery_marker(
                    action="Build", marker_file=marker, metadata=metadata, operation=lambda: None
                )

            agent_build.execute_with_recovery_marker(
                action="Rebuild",
                marker_file=marker,
                metadata={**metadata, "action": "Rebuild"},
                operation=lambda: None,
            )
            self.assertFalse(marker.exists())

    def test_normal_command_failure_does_not_leave_an_interruption_marker(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / "interrupted.json"

            def fail() -> None:
                raise agent_build.AgentBuildError("compile failed")

            with self.assertRaisesRegex(agent_build.AgentBuildError, "compile failed"):
                agent_build.execute_with_recovery_marker(
                    action="Build",
                    marker_file=marker,
                    metadata={"pid": 1, "profile": "agent", "action": "Build"},
                    operation=fail,
                )
            self.assertFalse(marker.exists())

    def test_clean_does_not_clear_a_previous_interruption_marker(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / "interrupted.json"
            previous = {"pid": 1, "profile": "agent", "action": "Build"}
            marker.write_text(json.dumps(previous), encoding="utf-8")

            agent_build.execute_with_recovery_marker(
                action="Clean",
                marker_file=marker,
                metadata={**previous, "action": "Clean"},
                operation=lambda: None,
            )

            self.assertEqual(json.loads(marker.read_text(encoding="utf-8")), previous)

    def test_keyboard_interrupt_terminates_the_child_process_tree(self) -> None:
        process = mock.Mock()
        process.wait.side_effect = KeyboardInterrupt
        with mock.patch.object(agent_build.subprocess, "Popen", return_value=process), mock.patch.object(
            agent_build, "terminate_process_tree"
        ) as terminate:
            with self.assertRaises(agent_build.AgentBuildInterruptedError):
                agent_build.run_command(["cmake", "--version"], environment={})
        terminate.assert_called_once_with(process)


class EnvironmentProviderTests(unittest.TestCase):
    def test_windows_environment_collapses_case_insensitive_duplicates(self) -> None:
        environment = agent_build.parse_environment_output(
            "PATH=developer-path\nPath=parent-path\n", case_insensitive=True
        )
        self.assertEqual(environment, {"PATH": "developer-path"})

    def test_inherit_provider_preserves_environment(self) -> None:
        with mock.patch.dict(agent_build.os.environ, {"DURIN_TEST_ENV": "present"}, clear=True):
            result = agent_build.build_environment(
                {"environmentProvider": "inherit"},
                {"script": "", "arguments": []},
                current_host="linux",
            )
        self.assertEqual(result["DURIN_TEST_ENV"], "present")

    def test_visual_studio_provider_uses_detected_script(self) -> None:
        script = Path("VsDevCmd.bat")
        with mock.patch.object(agent_build, "find_vsdevcmd", return_value=script), mock.patch.object(
            agent_build, "capture_setup_environment", return_value={"INCLUDE": "detected"}
        ) as capture:
            result = agent_build.build_environment(
                {"environmentProvider": "visual-studio"},
                {"script": "", "arguments": []},
                current_host="windows",
            )
        self.assertEqual(result["INCLUDE"], "detected")
        capture.assert_called_once_with(script, ["-arch=x64", "-host_arch=x64"], current_host="windows")

    def test_script_provider_requires_script(self) -> None:
        with self.assertRaisesRegex(agent_build.AgentBuildError, "requires environmentSetup.script"):
            agent_build.build_environment(
                {"environmentProvider": "script"},
                {"script": "", "arguments": []},
                current_host="linux",
            )

    def test_windows_setup_script_is_passed_to_cmd_as_a_separate_argument(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            script = Path(directory) / "VS Tools" / "VsDevCmd.bat"
            script.parent.mkdir()
            script.touch()
            completed = mock.Mock(returncode=0, stdout="DURIN_ENV=ready\n", stderr="")
            with mock.patch.object(agent_build.subprocess, "run", return_value=completed) as run:
                environment = agent_build.capture_setup_environment(
                    script, ["-arch=x64"], current_host="windows"
                )

        command = run.call_args.args[0]
        self.assertEqual(command[4:7], ["call", str(script), "-arch=x64"])
        self.assertEqual(command[-3:], [">nul", "&&", "set"])
        self.assertEqual(environment["DURIN_ENV"], "ready")

    def test_visual_studio_profile_adds_bundled_ninja_to_path(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            visual_studio_root = Path(directory)
            ninja = visual_studio_root / "Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"
            ninja.parent.mkdir(parents=True)
            ninja.touch()
            environment = {"Path": "original", "VSINSTALLDIR": str(visual_studio_root)}
            with mock.patch.object(agent_build.shutil, "which", return_value=None):
                agent_build.ensure_required_commands(
                    {"environmentProvider": "visual-studio", "requiredCommands": ["ninja"]},
                    environment,
                )

        self.assertTrue(environment["Path"].startswith(str(ninja.parent)))

    def test_missing_required_command_is_rejected(self) -> None:
        with mock.patch.object(agent_build.shutil, "which", return_value=None):
            with self.assertRaisesRegex(agent_build.AgentBuildError, "Required command"):
                agent_build.ensure_required_commands(
                    {"environmentProvider": "inherit", "requiredCommands": ["missing-tool"]},
                    {"PATH": ""},
                )


class AgentConfigLifecycleTests(unittest.TestCase):
    @staticmethod
    def create_repo(root: Path) -> None:
        template = root / agent_config.TEMPLATE_RELATIVE_PATH
        template.parent.mkdir(parents=True)
        template.write_text('{"cmakeCommand": ""}\n', encoding="utf-8")

    def test_initialize_is_idempotent(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.create_repo(root)
            target = agent_config.ensure_agent_config(root)
            target.write_text("local edit\n", encoding="utf-8")
            agent_config.ensure_agent_config(root)
            self.assertEqual(target.read_text(encoding="utf-8"), "local edit\n")

    def test_sync_copies_source_and_skips_matching_content(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source"
            target = root / "target"
            self.create_repo(source)
            self.create_repo(target)
            source_config = agent_config.ensure_agent_config(source)
            source_config.write_text("source config\n", encoding="utf-8")

            target_config = agent_config.sync_agent_config(source, target)
            self.assertEqual(target_config.read_text(encoding="utf-8"), "source config\n")
            previous_timestamp = target_config.stat().st_mtime_ns
            agent_config.sync_agent_config(source, target)
            self.assertEqual(target_config.stat().st_mtime_ns, previous_timestamp)

    def test_missing_source_falls_back_to_target_template(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source"
            target = root / "target"
            source.mkdir()
            self.create_repo(target)
            target_config = agent_config.sync_agent_config(source, target)
            self.assertEqual(target_config.read_text(encoding="utf-8"), '{"cmakeCommand": ""}\n')

    def test_dry_run_does_not_create_config(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.create_repo(root)
            target = agent_config.ensure_agent_config(root, dry_run=True)
            self.assertFalse(target.exists())


if __name__ == "__main__":
    unittest.main()
