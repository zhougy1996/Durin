from __future__ import annotations

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
        self.assertEqual(config["environmentSetup"], {"script": "", "arguments": []})

    def test_valid_config_is_loaded(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "config.json"
            path.write_text(
                json.dumps(
                    {
                        "cmakeCommand": "custom-cmake",
                        "defaultBuildProfile": "windows-msvc-x64",
                        "environmentSetup": {"script": "setup.cmd", "arguments": ["x64"]},
                    }
                ),
                encoding="utf-8",
            )
            config = agent_build.load_local_config(path)

        self.assertEqual(config["cmakeCommand"], "custom-cmake")
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
