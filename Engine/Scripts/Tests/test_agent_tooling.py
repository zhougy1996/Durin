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


class WindowsBuildWrapperTests(unittest.TestCase):
    def test_wrapper_establishes_msvc_language_and_forwards_all_arguments(self) -> None:
        content = (REPO_ROOT / "BuildTool.bat").read_text(encoding="utf-8")

        self.assertIn('set "VSLANG=1033"', content)
        self.assertIn('.venv\\Scripts\\python.exe', content)
        self.assertIn('Engine\\Scripts\\Build\\agent_build.py" %*', content)
        self.assertIn("exit /b %ERRORLEVEL%", content)

    def test_setup_prepares_python_before_other_bootstrap_steps(self) -> None:
        content = (REPO_ROOT / "Setup.bat").read_text(encoding="utf-8")

        python_setup = content.index("SetupPython.bat")
        agent_config = content.index("InitializeAgentConfig.bat")
        third_party = content.index("Bootstrap.bat")
        self.assertLess(python_setup, agent_config)
        self.assertLess(agent_config, third_party)

    def test_python_requirements_pin_libclang(self) -> None:
        content = (REPO_ROOT / "requirements.txt").read_text(encoding="utf-8")

        self.assertRegex(content, r"(?m)^libclang==\d+\.\d+\.\d+$")


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
        with self.assertRaisesRegex(agent_build.AgentBuildError, "No Agent build profile"):
            agent_build.select_profile(self.profiles, environment={}, current_host="macos")

    def test_enabled_preset_is_selected_and_unknown_preset_is_rejected(self) -> None:
        profile = {
            "defaultPreset": "debug",
            "presets": ["debug", "release"],
        }
        presets = {"debug": {"name": "debug"}, "release": {"name": "release"}}

        name, _ = agent_build.select_preset(profile, presets)
        self.assertEqual(name, "debug")
        name, _ = agent_build.select_preset(profile, presets, requested="release")
        self.assertEqual(name, "release")
        with self.assertRaisesRegex(agent_build.AgentBuildError, "not enabled"):
            agent_build.select_preset(profile, presets, requested="shipping")

    def test_cmake_preset_inheritance_resolves_build_metadata(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "CMakePresets.json"
            path.write_text(
                json.dumps(
                    {
                        "configurePresets": [
                            {
                                "name": "base",
                                "hidden": True,
                                "binaryDir": "${sourceDir}/Build/${presetName}",
                                "cacheVariables": {"CMAKE_BUILD_TYPE": "Debug", "BUILD_TESTING": "OFF"},
                            },
                            {
                                "name": "tests",
                                "inherits": "base",
                                "cacheVariables": {
                                    "DURIN_PROFILE_NAME": "DurinEditor",
                                    "BUILD_TESTING": "ON",
                                },
                            },
                        ]
                    }
                ),
                encoding="utf-8",
            )

            presets = agent_build.load_configure_presets(path)

        self.assertEqual(agent_build.preset_cache_string(presets["tests"], "CMAKE_BUILD_TYPE"), "Debug")
        self.assertTrue(agent_build.preset_cache_bool(presets["tests"], "BUILD_TESTING"))

    def test_repository_profile_only_enables_existing_presets(self) -> None:
        profiles = agent_build.load_profiles()
        presets = agent_build.load_configure_presets()

        for profile in profiles.values():
            self.assertIn(profile["defaultPreset"], profile["presets"])
            self.assertTrue(set(profile["presets"]).issubset(presets))

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
        self.assertEqual(clean.action, "clean")
        self.assertEqual(rebuild.action, "rebuild")
        self.assertEqual(rebuild.target, "")

    def test_no_arguments_open_the_shell_and_lowercase_commands_are_canonical(self) -> None:
        self.assertEqual(agent_build.parse_args([]).action, "shell")
        self.assertEqual(agent_build.parse_args(["build", "--target", "all"]).action, "build")
        self.assertEqual(agent_build.parse_args(["Shell"]).action, "shell")
        purge = agent_build.parse_args(["Purge", "--all-presets", "--yes"])
        self.assertEqual(purge.action, "purge")
        self.assertTrue(purge.all_presets)
        self.assertTrue(purge.yes)

    def test_shell_preset_selection_is_forwarded_to_build(self) -> None:
        args = agent_build.parse_args(["shell"])
        with mock.patch(
            "builtins.input", side_effect=["/presets", "4", "/build", "/exit"]
        ) as shell_input, mock.patch("builtins.print") as shell_print, mock.patch.object(
            agent_build, "execute"
        ) as execute:
            agent_build.run_shell(args)

        request = execute.call_args.args[0]
        self.assertEqual(request.action, "build")
        self.assertEqual(request.target, "all")
        self.assertEqual(request.preset, "Win64-Release-DurinEditor")
        self.assertIn(mock.call("BuildTool> "), shell_input.call_args_list)
        self.assertNotIn(mock.call("Preset number> "), shell_input.call_args_list)
        self.assertIn(
            mock.call("Enter a preset number, or press Enter to keep the current preset."),
            shell_print.call_args_list,
        )

    def test_shell_purge_forwards_scope_and_confirmation_options(self) -> None:
        args = agent_build.parse_args(["shell"])
        with mock.patch("builtins.input", side_effect=["/purge --all-presets --yes", "/exit"]), mock.patch.object(
            agent_build, "execute"
        ) as execute:
            agent_build.run_shell(args)

        request = execute.call_args.args[0]
        self.assertEqual(request.action, "purge")
        self.assertTrue(request.all_presets)
        self.assertTrue(request.yes)

    def test_preset_command_uses_full_names_and_no_value_shows_current_preset(self) -> None:
        profile = {
            "defaultPreset": "Win64-Debug-DurinEditor-Tests",
            "presets": ["Win64-Debug-DurinEditor-Tests", "Win64-Release-DurinEditor"],
        }
        self.assertEqual(
            agent_build.resolve_shell_preset("Win64-Release-DurinEditor", profile),
            "Win64-Release-DurinEditor",
        )
        with self.assertRaisesRegex(agent_build.AgentBuildError, "full name"):
            agent_build.resolve_shell_preset("2", profile)

        args = agent_build.parse_args(["shell"])
        with mock.patch("builtins.input", side_effect=["/preset", "/exit"]), mock.patch.object(
            agent_build, "execute"
        ) as execute:
            agent_build.run_shell(args)
        execute.assert_not_called()

    def test_test_action_rejects_non_test_preset_before_building(self) -> None:
        args = argparse.Namespace(action="test", target="CoreTests", filter="")
        with mock.patch.object(agent_build, "run_command") as run:
            with self.assertRaisesRegex(agent_build.AgentBuildError, "does not enable BUILD_TESTING"):
                agent_build.perform_action(
                    args,
                    cmake="cmake",
                    jobs=8,
                    environment={},
                    build_directory=Path("Build/release"),
                    cache_file=Path("Build/release/CMakeCache.txt"),
                    preset="release",
                    profile={"platform": "Win64", "testExecutableSuffix": ".exe"},
                    preset_metadata={"name": "release", "cacheVariables": {"BUILD_TESTING": "OFF"}},
                )
        run.assert_not_called()

    def test_purge_paths_cover_preset_tree_project_outputs_and_intermediate_metadata(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for project_name in ("Engine", "SandBox"):
                project_root = root / project_name
                project_root.mkdir()
                (project_root / f"{project_name}.dproject").touch()
            preset = {
                "name": "Win64-Debug-DurinEditor",
                "binaryDir": "${sourceDir}/Build/${presetName}",
                "cacheVariables": {
                    "CMAKE_BUILD_TYPE": "Debug",
                    "CMAKE_INSTALL_PREFIX": "${sourceDir}/Install/${presetName}",
                    "DURIN_PROFILE_NAME": "DurinEditor",
                },
            }

            paths = set(agent_build.collect_purge_paths({"platform": "Win64"}, [preset], root=root))

            self.assertIn(root / "Build/Win64-Debug-DurinEditor", paths)
            self.assertIn(root / "Install/Win64-Debug-DurinEditor", paths)
            self.assertIn(root / "Engine/Binaries/Win64/Debug", paths)
            self.assertIn(root / "SandBox/Binaries/Win64/Debug", paths)
            self.assertIn(root / "Engine/Intermediate/Build/Win64/DurinEditor", paths)
            self.assertIn(root / "SandBox/Intermediate/Build/Win64/DurinEditor", paths)

    def test_purge_removes_only_validated_paths_inside_the_checkout(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            artifact = root / "Build" / "preset"
            artifact.mkdir(parents=True)
            (artifact / "output.obj").touch()
            preserved_dependency = root / "Build" / "ThirdParty" / "dependency.lib"
            preserved_dependency.parent.mkdir(parents=True)
            preserved_dependency.touch()

            agent_build.remove_purge_paths([artifact], root=root)

            self.assertFalse(artifact.exists())
            self.assertTrue(preserved_dependency.exists())
            with self.assertRaisesRegex(agent_build.AgentBuildError, "checkout root"):
                agent_build.remove_purge_paths([root], root=root)

    def test_purge_confirmation_uses_a_stronger_phrase_for_all_presets(self) -> None:
        artifact = Path("Build/preset")
        self.assertTrue(
            agent_build.confirm_purge([artifact], all_presets=False, input_fn=lambda prompt: "PURGE")
        )
        self.assertFalse(
            agent_build.confirm_purge([artifact], all_presets=True, input_fn=lambda prompt: "PURGE")
        )
        self.assertTrue(
            agent_build.confirm_purge([artifact], all_presets=True, input_fn=lambda prompt: "PURGE ALL")
        )

    def test_rebuild_defaults_to_all_after_clean_and_fresh_configure(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            build_directory = Path(directory) / "Build" / "agent"
            build_directory.mkdir(parents=True)
            cache_file = build_directory / "CMakeCache.txt"
            cache_file.write_text("CMAKE_MAKE_PROGRAM:FILEPATH=C:/Ninja/ninja.exe\n", encoding="utf-8")
            args = argparse.Namespace(action="rebuild", target="", filter="")
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
                    preset_metadata={},
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
            args = argparse.Namespace(action="clean", target="", filter="")
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
                    preset_metadata={},
                )
        run.assert_not_called()

    def test_checkout_lock_is_exclusive_across_presets(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            first_path = agent_build.lock_file_path(root)
            second_path = agent_build.lock_file_path(root)
            metadata = {"pid": 1, "profile": "profile-a", "action": "Build"}
            with agent_build.AgentBuildLock(first_path, metadata):
                with self.assertRaisesRegex(agent_build.AgentBuildError, "already owns"):
                    with agent_build.AgentBuildLock(first_path, metadata):
                        pass
                with self.assertRaisesRegex(agent_build.AgentBuildError, "already owns"):
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
                    action="build", marker_file=marker, metadata=metadata, operation=interrupt
                )
            self.assertTrue(marker.is_file())

            with self.assertRaisesRegex(agent_build.AgentBuildError, "rebuild --target all"):
                agent_build.execute_with_recovery_marker(
                    action="build", marker_file=marker, metadata=metadata, operation=lambda: None
                )

            agent_build.execute_with_recovery_marker(
                action="rebuild",
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
                    action="build",
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
                action="clean",
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
