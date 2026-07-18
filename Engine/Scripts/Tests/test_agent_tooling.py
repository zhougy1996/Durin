from __future__ import annotations

import io
import json
import os
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[3]
BUILD_SCRIPT_DIR = REPO_ROOT / "Engine" / "Scripts" / "Build"
if str(BUILD_SCRIPT_DIR) not in os.sys.path:
    os.sys.path.insert(0, str(BUILD_SCRIPT_DIR))

from durin_build_tool import cli as build_cli
from durin_build_tool import config as build_config
from durin_build_tool import core as build_core
from durin_build_tool.output import BuildOutput

from Engine.Scripts.Bootstrap import agent_config


class BuildConfigTests(unittest.TestCase):
    def test_missing_config_uses_empty_overrides(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            config = build_config.load_local_config(Path(directory) / "missing.json")
        self.assertEqual(config, build_config.LocalConfig())

    def test_valid_config_uses_typed_models(self) -> None:
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
            config = build_config.load_local_config(path)
        self.assertEqual(config.cmake_command, "custom-cmake")
        self.assertEqual(config.jobs, 8)
        self.assertEqual(config.environment_setup.arguments, ("x64",))

    def test_invalid_json_and_field_types_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "config.json"
            path.write_text("{", encoding="utf-8")
            with self.assertRaisesRegex(build_config.BuildToolError, "invalid JSON"):
                build_config.load_local_config(path)
            path.write_text(json.dumps({"cmakeCommand": 42}), encoding="utf-8")
            with self.assertRaisesRegex(build_config.BuildToolError, "must be a string"):
                build_config.load_local_config(path)
            path.write_text(json.dumps({"jobs": 257}), encoding="utf-8")
            with self.assertRaisesRegex(build_config.BuildToolError, "integer from 0 to 256"):
                build_config.load_local_config(path)

    def test_repository_profiles_reference_existing_presets(self) -> None:
        profiles = build_config.load_profiles()
        presets = build_config.load_configure_presets()
        for profile in profiles.values():
            self.assertIn(profile.default_preset, profile.presets)
            self.assertTrue(set(profile.presets).issubset(presets))

    def test_cmake_preset_inheritance_is_resolved(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "CMakePresets.json"
            path.write_text(
                json.dumps(
                    {
                        "configurePresets": [
                            {
                                "name": "base",
                                "binaryDir": "${sourceDir}/Build/${presetName}",
                                "cacheVariables": {"CMAKE_BUILD_TYPE": "Debug", "BUILD_TESTING": "OFF"},
                            },
                            {
                                "name": "tests",
                                "inherits": "base",
                                "cacheVariables": {"BUILD_TESTING": "ON"},
                            },
                        ]
                    }
                ),
                encoding="utf-8",
            )
            presets = build_config.load_configure_presets(path)
        self.assertEqual(build_config.preset_cache_string(presets["tests"], "CMAKE_BUILD_TYPE"), "Debug")
        self.assertTrue(build_config.preset_cache_bool(presets["tests"], "BUILD_TESTING"))

    def test_profile_precedence_and_host_validation(self) -> None:
        profiles = {
            "default": build_config.BuildProfile(
                "default",
                "windows",
                "debug",
                ("debug",),
                build_config.EnvironmentProvider.INHERIT,
                "Win64",
                ".exe",
                True,
                (),
            ),
            "other": build_config.BuildProfile(
                "other",
                "windows",
                "debug",
                ("debug",),
                build_config.EnvironmentProvider.INHERIT,
                "Win64",
                ".exe",
                False,
                (),
            ),
        }
        selected = build_config.select_profile(
            profiles,
            requested="other",
            environment={build_config.PROFILE_ENV_VAR: "default"},
            current_host="windows",
        )
        self.assertEqual(selected.name, "other")
        with self.assertRaisesRegex(build_config.BuildToolError, "current host"):
            build_config.select_profile(profiles, requested="other", current_host="linux")

    def test_job_precedence_and_cpu_fallback(self) -> None:
        self.assertEqual(build_config.resolve_jobs(3, 6, environment={}, cpu_count=20), 3)
        self.assertEqual(
            build_config.resolve_jobs(
                None,
                6,
                environment={build_config.JOBS_ENV_VAR: "4"},
                cpu_count=20,
            ),
            4,
        )
        self.assertEqual(build_config.resolve_jobs(None, 6, environment={}, cpu_count=20), 6)
        self.assertEqual(build_config.resolve_jobs(None, 0, environment={}, cpu_count=20), 18)

    def test_invalid_job_environment_is_rejected(self) -> None:
        with self.assertRaisesRegex(build_config.BuildToolError, build_config.JOBS_ENV_VAR):
            build_config.resolve_jobs(
                None,
                0,
                environment={build_config.JOBS_ENV_VAR: "many"},
            )

    def test_unknown_preset_is_rejected_with_available_values(self) -> None:
        profile = next(iter(build_config.load_profiles().values()))
        presets = build_config.load_configure_presets()
        with self.assertRaisesRegex(build_config.BuildToolError, "Available presets"):
            build_config.select_preset(profile, presets, requested="missing")

    def test_output_configuration_appends_build_identifier(self) -> None:
        preset = build_config.ConfigurePreset(
            "debug",
            {
                "cacheVariables": {
                    "CMAKE_BUILD_TYPE": "Debug",
                    "DURIN_BUILD_IDENTIFIER": "Agent",
                }
            },
        )
        self.assertEqual(build_config.preset_output_configuration(preset), "Debug-Agent")

    def test_explicit_cmake_path_takes_precedence(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            requested = Path(directory) / "cmake.exe"
            requested.touch()
            resolved = build_config.resolve_cmake_command(
                str(requested),
                "configured",
                environment={"DURIN_CMAKE_COMMAND": "environment"},
            )
        self.assertEqual(Path(resolved), requested.resolve())

    def test_preset_build_path_cannot_escape_checkout(self) -> None:
        preset = build_config.ConfigurePreset("escape", {"binaryDir": "${sourceDir}/../outside"})
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(build_config.BuildToolError, "inside the checkout"):
                build_config.preset_build_directory(preset, root=Path(directory))


class CliTests(unittest.TestCase):
    def test_no_arguments_open_shell_and_actions_are_case_insensitive(self) -> None:
        self.assertIs(build_cli.parse_args([]).action, build_config.Action.SHELL)
        request = build_cli.parse_args(["Build", "--target", "all"])
        self.assertIs(request.action, build_config.Action.BUILD)
        self.assertEqual(request.target, "all")

    def test_command_specific_options_are_parsed(self) -> None:
        purge = build_cli.parse_args(["purge", "--all-presets", "--yes"])
        self.assertTrue(purge.all_presets)
        self.assertTrue(purge.yes)
        run = build_cli.parse_args(["run", "--preset", "game", "--args", "-log"])
        self.assertEqual(run.run_arguments, ("-log",))
        test = build_cli.parse_args(["test", "--target", "CoreTests", "--filter", "Core.*"])
        self.assertEqual(test.test_filter, "Core.*")

    def test_build_requires_target_with_command_specific_help(self) -> None:
        with self.assertRaisesRegex(build_config.BuildToolError, "BuildTool build --help"):
            build_cli.parse_args(["build"])

    def test_build_help_does_not_show_purge_options(self) -> None:
        parser = build_cli.make_parser()
        build_parser = parser._subparsers._group_actions[0].choices["build"]
        help_text = build_parser.format_help()
        self.assertIn("--target", help_text)
        self.assertNotIn("--all-presets", help_text)

    def test_plain_option_is_available_after_command(self) -> None:
        request = build_cli.parse_args(["configure", "--plain"])
        self.assertTrue(request.plain)

    def test_global_options_before_uppercase_command_are_preserved(self) -> None:
        request = build_cli.parse_args(["--plain", "Build", "--target", "all"])
        self.assertIs(request.action, build_config.Action.BUILD)
        self.assertTrue(request.plain)

    def test_rebuild_defaults_to_all(self) -> None:
        self.assertEqual(build_cli.parse_args(["rebuild"]).target, "all")

    def test_wrapper_uses_new_entrypoint_and_forwards_arguments(self) -> None:
        content = (REPO_ROOT / "BuildTool.bat").read_text(encoding="utf-8")
        self.assertIn('set "VSLANG=1033"', content)
        self.assertIn('Engine\\Scripts\\Build\\durin_build_tool\\__main__.py" %*', content)
        self.assertNotIn("agent_build.py", content)

    def test_requirements_pin_rich_and_libclang(self) -> None:
        content = (REPO_ROOT / "requirements.txt").read_text(encoding="utf-8")
        self.assertRegex(content, r"(?m)^libclang==\d+\.\d+\.\d+$")
        self.assertRegex(content, r"(?m)^rich==\d+\.\d+\.\d+$")


class OutputTests(unittest.TestCase):
    def test_plain_output_has_no_ansi_sequences(self) -> None:
        stdout = io.StringIO()
        stderr = io.StringIO()
        output = BuildOutput(plain=True, stdout=stdout, stderr=stderr, force_terminal=True)
        output.success("done")
        output.failure(build_config.BuildToolError("failed"), None, 1.0)
        self.assertNotIn("\x1b[", stdout.getvalue() + stderr.getvalue())

    def test_non_tty_output_automatically_uses_plain_mode(self) -> None:
        output = BuildOutput(stdout=io.StringIO(), stderr=io.StringIO())
        self.assertTrue(output.plain)

    def test_rich_tty_output_contains_ansi_and_semantic_status(self) -> None:
        stdout = io.StringIO()
        output = BuildOutput(stdout=stdout, stderr=io.StringIO(), force_terminal=True)
        output.success("done")
        self.assertIn("\x1b[", stdout.getvalue())
        self.assertIn("success", stdout.getvalue())

    def test_failure_summary_contains_command_exit_code_and_recovery(self) -> None:
        stderr = io.StringIO()
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=stderr)
        error = build_config.BuildToolError(
            "compile failed",
            command=["cmake", "--build", "Build"],
            exit_code=1,
            recovery="fix the compiler error",
        )
        output.failure(error, None, 2.5)
        text = stderr.getvalue()
        self.assertIn("cmake --build Build", text)
        self.assertIn("Exit code: 1", text)
        self.assertIn("fix the compiler error", text)

    def test_no_color_environment_forces_plain_output(self) -> None:
        with mock.patch.dict(os.environ, {"NO_COLOR": "1"}):
            output = BuildOutput(
                stdout=io.StringIO(),
                stderr=io.StringIO(),
                force_terminal=True,
            )
        self.assertTrue(output.plain)

    def test_plain_stage_uses_ascii_boundary(self) -> None:
        stdout = io.StringIO()
        output = BuildOutput(plain=True, stdout=stdout, stderr=io.StringIO())
        with output.stage("Build"):
            pass
        self.assertIn("== Build ==", stdout.getvalue())


class CoreTests(unittest.TestCase):
    def make_profile(self) -> build_config.BuildProfile:
        return build_config.BuildProfile(
            "test-profile",
            "windows",
            "debug",
            ("debug", "release"),
            build_config.EnvironmentProvider.INHERIT,
            "Win64",
            ".exe",
            True,
            (),
        )

    def make_preset(self, name: str = "debug", testing: str = "ON") -> build_config.ConfigurePreset:
        return build_config.ConfigurePreset(
            name,
            {
                "name": name,
                "binaryDir": "${sourceDir}/Build/${presetName}",
                "cacheVariables": {
                    "CMAKE_BUILD_TYPE": "Debug",
                    "DURIN_PROFILE_NAME": "DurinEditor",
                    "BUILD_TESTING": testing,
                },
            },
        )

    def test_environment_output_collapses_windows_case_duplicates(self) -> None:
        environment = build_core.parse_environment_output(
            "PATH=developer\nPath=parent\n",
            case_insensitive=True,
        )
        self.assertEqual(environment, {"PATH": "developer"})

    def test_inherit_provider_preserves_environment(self) -> None:
        with mock.patch.dict(os.environ, {"DURIN_TEST_ENV": "present"}, clear=True):
            environment = build_core.build_environment(
                self.make_profile(),
                build_config.EnvironmentSetup(),
                current_host="windows",
            )
        self.assertEqual(environment["DURIN_TEST_ENV"], "present")

    def test_visual_studio_environment_is_captured_once(self) -> None:
        profile = replace(
            self.make_profile(),
            environment_provider=build_config.EnvironmentProvider.VISUAL_STUDIO,
        )
        with mock.patch.object(build_core, "find_vsdevcmd", return_value=Path("VsDevCmd.bat")), mock.patch.object(
            build_core,
            "capture_setup_environment",
            return_value={"PATH": "ready", "VSLANG": "2052"},
        ) as capture, mock.patch.object(
            build_core,
            "detect_msvc_showincludes_prefix",
            return_value="Note: including file:  ",
        ) as detect_prefix:
            environment = build_core.build_environment(
                profile,
                build_config.EnvironmentSetup(),
                current_host="windows",
            )
        self.assertEqual(environment["PATH"], "ready")
        self.assertEqual(environment["VSLANG"], "1033")
        capture.assert_called_once()
        detect_prefix.assert_called_once_with(environment)

    def test_visual_studio_environment_rejects_localized_compiler_output(self) -> None:
        profile = replace(
            self.make_profile(),
            environment_provider=build_config.EnvironmentProvider.VISUAL_STUDIO,
        )
        with mock.patch.object(build_core, "find_vsdevcmd", return_value=Path("VsDevCmd.bat")), mock.patch.object(
            build_core,
            "capture_setup_environment",
            return_value={"PATH": "ready"},
        ), mock.patch.object(
            build_core,
            "detect_msvc_showincludes_prefix",
            return_value="注意: 包含文件:  ",
        ), self.assertRaisesRegex(build_config.BuildToolError, "English language pack"):
            build_core.build_environment(
                profile,
                build_config.EnvironmentSetup(),
                current_host="windows",
            )

    def test_windows_setup_script_is_passed_as_separate_argument(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            script = Path(directory) / "VS Tools" / "VsDevCmd.bat"
            script.parent.mkdir()
            script.touch()
            completed = mock.Mock(returncode=0, stdout="DURIN_ENV=ready\n", stderr="")
            with mock.patch.object(build_core.subprocess, "run", return_value=completed) as run:
                environment = build_core.capture_setup_environment(
                    script,
                    ["-arch=x64"],
                    current_host="windows",
                )
        command = run.call_args.args[0]
        self.assertEqual(command[4:7], ["call", str(script), "-arch=x64"])
        self.assertEqual(environment["DURIN_ENV"], "ready")

    def test_visual_studio_profile_adds_bundled_ninja(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            ninja = root / "Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"
            ninja.parent.mkdir(parents=True)
            ninja.touch()
            profile = replace(
                self.make_profile(),
                environment_provider=build_config.EnvironmentProvider.VISUAL_STUDIO,
                required_commands=("ninja",),
            )
            environment = {"Path": "original", "VSINSTALLDIR": str(root)}
            with mock.patch.object(build_core.shutil, "which", return_value=None):
                build_core.ensure_required_commands(profile, environment)
        self.assertTrue(environment["Path"].startswith(str(ninja.parent)))

    def test_derive_context_reuses_toolchain_environment(self) -> None:
        profile = self.make_profile()
        presets = {"debug": self.make_preset(), "release": self.make_preset("release")}
        request = build_config.CommandRequest(build_config.Action.SHELL, preset="debug")
        environment = {"PATH": "cached"}
        context = build_config.BuildContext(
            request,
            build_config.LocalConfig(),
            profile,
            presets,
            presets["debug"],
            "windows",
            cmake="cmake",
            jobs=8,
            environment=environment,
        )
        child = build_core.derive_context(
            context,
            build_config.CommandRequest(build_config.Action.BUILD, target="all", preset="release"),
        )
        self.assertIs(child.environment, environment)
        self.assertEqual(child.preset.name, "release")

    def test_runtime_path_uses_profile_and_build_identifier(self) -> None:
        preset = self.make_preset()
        values = dict(preset.values)
        cache = dict(values["cacheVariables"])
        cache["DURIN_BUILD_IDENTIFIER"] = "Agent"
        preset = build_config.ConfigurePreset("debug", {**values, "cacheVariables": cache})
        path = build_core.runtime_executable_path(self.make_profile(), preset, root=Path("repo"))
        self.assertEqual(
            path,
            Path("repo/Engine/Binaries/Win64/Debug-Agent/Runtime/DurinEditor/DurinEditor.exe"),
        )

    def test_run_application_reports_how_to_build_missing_runtime(self) -> None:
        preset = self.make_preset()
        request = build_config.CommandRequest(build_config.Action.RUN)
        context = build_config.BuildContext(
            request,
            build_config.LocalConfig(),
            self.make_profile(),
            {"debug": preset},
            preset,
            "windows",
        )
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        with mock.patch.object(
            build_core,
            "runtime_executable_path",
            return_value=Path("missing/DurinEditor.exe"),
        ), self.assertRaisesRegex(build_config.BuildToolError, "was not found"):
            build_core.run_application(context, output)

    def test_run_application_waits_for_relaunched_descendants(self) -> None:
        preset = self.make_preset()
        context = build_config.BuildContext(
            build_config.CommandRequest(build_config.Action.RUN),
            build_config.LocalConfig(),
            self.make_profile(),
            {"debug": preset},
            preset,
            "windows",
        )
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(
            build_core,
            "runtime_executable_path",
            return_value=Path(directory) / "DurinEditor.exe",
        ) as runtime_path, mock.patch.object(build_core, "run_command") as run:
            runtime_path.return_value.touch()
            build_core.run_application(context, output)
        self.assertTrue(run.call_args.kwargs["wait_for_descendants"])

    def test_run_command_waits_for_windows_process_job(self) -> None:
        process = mock.Mock(pid=42, returncode=0)
        process.wait.return_value = 0
        process_job = mock.Mock()
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        with mock.patch.object(build_core.subprocess, "Popen", return_value=process), mock.patch.object(
            build_core,
            "WindowsProcessJob",
            return_value=process_job,
        ):
            build_core.run_command(
                ["DurinEditor.exe"],
                environment={},
                output=output,
                wait_for_descendants=True,
            )
        process_job.assign.assert_called_once_with(process)
        process_job.wait.assert_called_once_with()
        process_job.close.assert_called_once_with()

    def test_interrupt_terminates_relaunched_windows_process_job(self) -> None:
        process = mock.Mock(pid=42, returncode=0)
        process.wait.return_value = 0
        process.poll.return_value = 0
        process_job = mock.Mock()
        process_job.wait.side_effect = KeyboardInterrupt
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        with mock.patch.object(build_core.subprocess, "Popen", return_value=process), mock.patch.object(
            build_core,
            "WindowsProcessJob",
            return_value=process_job,
        ), self.assertRaisesRegex(build_config.BuildToolError, "Application run was interrupted"):
            build_core.run_command(
                ["DurinEditor.exe"],
                environment={},
                output=output,
                recovery_required_on_interrupt=False,
                wait_for_descendants=True,
            )
        process_job.terminate.assert_called_once_with()
        process_job.close.assert_called_once_with()

    def test_open_runtime_directory_uses_selected_preset_directory(self) -> None:
        preset = self.make_preset()
        request = build_config.CommandRequest(build_config.Action.SHELL)
        context = build_config.BuildContext(
            request,
            build_config.LocalConfig(),
            self.make_profile(),
            {"debug": preset},
            preset,
            "windows",
        )
        stdout = io.StringIO()
        output = BuildOutput(plain=True, stdout=stdout, stderr=io.StringIO())
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(
            build_core,
            "runtime_executable_path",
            return_value=Path(directory) / "DurinEditor.exe",
        ), mock.patch.object(build_core.os, "startfile", create=True) as startfile:
            build_core.open_runtime_directory(context, output)
        startfile.assert_called_once_with(Path(directory))
        self.assertIn("Opened runtime directory", stdout.getvalue())

    def test_test_action_rejects_non_test_preset(self) -> None:
        request = build_config.CommandRequest(build_config.Action.TEST, target="CoreTests")
        with self.assertRaisesRegex(build_config.BuildToolError, "does not enable BUILD_TESTING"):
            build_core.validate_request(request, self.make_preset(testing="OFF"))

    def test_failed_generator_cache_is_not_reused(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory) / "CMakeCache.txt"
            cache.write_text("CMAKE_MAKE_PROGRAM:FILEPATH=CMAKE_MAKE_PROGRAM-NOTFOUND\n", encoding="utf-8")
            self.assertFalse(build_core.cache_is_usable(cache))
            cache.write_text("CMAKE_MAKE_PROGRAM:FILEPATH=ninja\n", encoding="utf-8")
            self.assertTrue(build_core.cache_is_usable(cache))

    def test_ninja_msvc_prefix_requires_english(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            build_directory = Path(directory)
            rules = build_directory / "CMakeFiles" / "rules.ninja"
            rules.parent.mkdir()
            rules.write_text("msvc_deps_prefix = 注意: 包含文件:  \n", encoding="utf-8")
            self.assertFalse(build_core.ninja_uses_english_msvc_prefix(build_directory))
            rules.write_text("msvc_deps_prefix = Note: including file:  \n", encoding="utf-8")
            self.assertTrue(build_core.ninja_uses_english_msvc_prefix(build_directory))

    def test_checkout_lock_is_exclusive_across_presets(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = build_core.lock_file_path(Path(directory))
            with build_core.BuildToolLock(path, {"pid": 1}):
                with self.assertRaisesRegex(build_config.BuildToolError, "already owns"):
                    with build_core.BuildToolLock(path, {"pid": 2}):
                        pass

    def test_interruption_marker_requires_rebuild(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / "interrupted.json"

            def interrupt() -> None:
                raise build_config.BuildToolInterruptedError("interrupted")

            with self.assertRaises(build_config.BuildToolInterruptedError):
                build_core.execute_with_recovery_marker(
                    action=build_config.Action.BUILD,
                    marker_file=marker,
                    metadata={"pid": 1},
                    operation=interrupt,
                )
            with self.assertRaisesRegex(build_config.BuildToolError, "did not return normally"):
                build_core.execute_with_recovery_marker(
                    action=build_config.Action.BUILD,
                    marker_file=marker,
                    metadata={"pid": 1},
                    operation=lambda: None,
                )
            build_core.execute_with_recovery_marker(
                action=build_config.Action.REBUILD,
                marker_file=marker,
                metadata={"pid": 1},
                operation=lambda: None,
            )
            self.assertFalse(marker.exists())

    def test_normal_command_failure_restores_marker(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / "interrupted.json"
            with self.assertRaisesRegex(build_config.BuildToolError, "failed"):
                build_core.execute_with_recovery_marker(
                    action=build_config.Action.BUILD,
                    marker_file=marker,
                    metadata={"pid": 1},
                    operation=lambda: (_ for _ in ()).throw(build_config.BuildToolError("failed")),
                )
            self.assertFalse(marker.exists())

    def test_keyboard_interrupt_terminates_child_process_tree(self) -> None:
        process = mock.Mock()
        process.wait.side_effect = KeyboardInterrupt
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        with mock.patch.object(build_core.subprocess, "Popen", return_value=process), mock.patch.object(
            build_core,
            "terminate_process_tree",
        ) as terminate:
            with self.assertRaises(build_config.BuildToolInterruptedError):
                build_core.run_command(["cmake", "--version"], environment={}, output=output)
        terminate.assert_called_once_with(process)

    def test_purge_paths_cover_build_outputs_and_metadata(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            project = root / "Engine"
            project.mkdir()
            (project / "Engine.dproject").touch()
            paths = set(build_core.collect_purge_paths(self.make_profile(), [self.make_preset()], root=root))
            self.assertIn(root / "Build/debug", paths)
            self.assertIn(root / "Engine/Binaries/Win64/Debug", paths)
            self.assertIn(root / "Engine/Intermediate/Build/Win64/DurinEditor", paths)

    def test_purge_rejects_checkout_root(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaisesRegex(build_config.BuildToolError, "checkout root"):
                build_core.remove_purge_paths([root], root=root)

    def test_purge_removes_only_selected_artifact(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            artifact = root / "Build" / "debug"
            artifact.mkdir(parents=True)
            preserved = root / "Build" / "ThirdParty" / "library.lib"
            preserved.parent.mkdir(parents=True)
            preserved.touch()
            build_core.remove_purge_paths([artifact], root=root)
            self.assertFalse(artifact.exists())
            self.assertTrue(preserved.exists())


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
            previous = target_config.stat().st_mtime_ns
            agent_config.sync_agent_config(source, target)
            self.assertEqual(target_config.stat().st_mtime_ns, previous)

    def test_dry_run_does_not_create_config(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.create_repo(root)
            target = agent_config.ensure_agent_config(root, dry_run=True)
            self.assertFalse(target.exists())


if __name__ == "__main__":
    unittest.main()
