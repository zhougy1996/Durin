from __future__ import annotations

import io
import json
import os
import shutil
import subprocess
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
from Engine.Scripts.Bootstrap import prepare_worktree
from Engine.Scripts.Bootstrap import setup_preflight


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

    def test_fast_configure_is_code_model_only_and_not_buildtool_owned(self) -> None:
        profiles = build_config.load_profiles()
        presets = build_config.load_configure_presets()
        preset_name = "Win64-Debug-DurinEditor-FastConfigure"
        self.assertTrue(
            build_config.preset_cache_bool(presets[preset_name], "DURIN_IDE_CODE_MODEL_ONLY")
        )
        for profile in profiles.values():
            self.assertNotIn(preset_name, profile.presets)

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


class CMakeCodeModelGuardTests(unittest.TestCase):
    def test_code_model_guard_fails_before_target_command_runs(self) -> None:
        local_config = build_config.load_local_config()
        cmake = local_config.cmake_command or shutil.which("cmake")
        if not cmake:
            self.skipTest("CMake is not available")
        ninja = shutil.which("ninja")
        if not ninja and os.name == "nt":
            for parent in Path(cmake).resolve().parents:
                bundled_ninja = parent / "ninja" / "win" / "x64" / "ninja.exe"
                if bundled_ninja.is_file():
                    ninja = str(bundled_ninja)
                    break
        if not ninja:
            self.skipTest("Ninja is not available")

        build_options = (REPO_ROOT / "CMake" / "Config" / "BuildOptions.cmake").as_posix()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source"
            build = root / "build"
            module = source / "Module"
            module.mkdir(parents=True)
            (source / "CMakeLists.txt").write_text(
                "\n".join(
                    [
                        "cmake_minimum_required(VERSION 3.24)",
                        "project(CodeModelGuard NONE)",
                        f'include("{build_options}")',
                        "add_subdirectory(Module)",
                        "durin_enforce_code_model_only_build()",
                    ]
                ),
                encoding="utf-8",
            )
            (module / "CMakeLists.txt").write_text(
                "\n".join(
                    [
                        "add_custom_target(WouldBuild",
                        '  COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_BINARY_DIR}/target-ran"',
                        ")",
                    ]
                ),
                encoding="utf-8",
            )
            configure = subprocess.run(
                [
                    cmake,
                    "-S",
                    str(source),
                    "-B",
                    str(build),
                    "-G",
                    "Ninja",
                    f"-DCMAKE_MAKE_PROGRAM={Path(ninja).as_posix()}",
                    "-DDURIN_IDE_CODE_MODEL_ONLY=ON",
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(configure.returncode, 0, configure.stdout + configure.stderr)

            guarded_build = subprocess.run(
                [cmake, "--build", str(build), "--target", "WouldBuild"],
                capture_output=True,
                text=True,
                check=False,
            )
            output = guarded_build.stdout + guarded_build.stderr
            self.assertNotEqual(guarded_build.returncode, 0, output)
            self.assertIn("This IDE preset is code-model-only and cannot build", output)
            self.assertFalse((build / "target-ran").exists())


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
        test = build_cli.parse_args(
            ["test", "--target", "CoreTests", "--filter", "Core.*", "--timeout", "45"]
        )
        self.assertEqual(test.test_filter, "Core.*")
        self.assertEqual(test.test_timeout_seconds, 45)
        self.assertEqual(build_cli.parse_args(["test", "--target", "CoreTests"]).test_timeout_seconds, 300)

    def test_build_defaults_to_all_and_test_requires_target(self) -> None:
        self.assertEqual(build_cli.parse_args(["build"]).target, "all")
        with self.assertRaisesRegex(build_config.BuildToolError, "BuildTool test --help"):
            build_cli.parse_args(["test"])

    def test_build_help_does_not_show_purge_options(self) -> None:
        parser = build_cli.make_parser()
        build_parser = parser._subparsers._group_actions[0].choices["build"]
        help_text = build_parser.format_help()
        self.assertIn("--target", help_text)
        self.assertNotIn("--all-presets", help_text)

    def test_every_direct_command_and_help_come_from_shared_specs(self) -> None:
        parser = build_cli.make_parser()
        subparsers = parser._subparsers._group_actions[0].choices
        self.assertEqual(set(subparsers), {spec.name for spec in build_cli.COMMAND_SPECS})
        top_level_help = parser.format_help()
        for spec in build_cli.COMMAND_SPECS:
            self.assertIn(spec.name, top_level_help)
            self.assertIn(spec.summary, top_level_help)
            command_help = subparsers[spec.name].format_help()
            for argument in spec.arguments:
                self.assertIn(argument.flags[0], command_help)

    def test_shell_startup_and_interactive_help_share_command_descriptions(self) -> None:
        parser = build_cli.make_parser()
        shell_parser = parser._subparsers._group_actions[0].choices["shell"]
        shared_help = build_cli.shell_command_help()
        self.assertIn(shared_help, shell_parser.format_help())
        stdout = io.StringIO()
        build_cli.print_shell_help(BuildOutput(plain=True, stdout=stdout, stderr=io.StringIO()))
        self.assertEqual(stdout.getvalue().strip(), shared_help)
        styled_stdout = io.StringIO()
        build_cli.print_shell_help(
            BuildOutput(
                stdout=styled_stdout,
                stderr=io.StringIO(),
                force_terminal=True,
            )
        )
        self.assertIn("configure [--fresh]", styled_stdout.getvalue())
        self.assertIn("preset [full-name]", styled_stdout.getvalue())

    def test_options_with_no_effect_are_rejected(self) -> None:
        invalid = [
            ["purge", "--jobs", "2"],
            ["--jobs", "2", "purge"],
            ["run", "--cmake", "cmake"],
            ["--environment-setup", "setup.bat", "open-runtime"],
            ["stop", "--preset", "debug"],
        ]
        for argv in invalid:
            with self.subTest(argv=argv), self.assertRaises(build_config.BuildToolError):
                build_cli.parse_args(argv)

    def test_direct_read_only_commands_are_parsed_and_dispatched(self) -> None:
        self.assertIs(build_cli.parse_args(["presets"]).action, build_config.Action.PRESETS)
        self.assertIs(build_cli.parse_args(["status"]).action, build_config.Action.STATUS)
        self.assertIs(build_cli.parse_args(["open-runtime"]).action, build_config.Action.OPEN_RUNTIME)
        context = mock.Mock()
        context.preset.name = "debug"
        with mock.patch.object(build_cli, "create_context", return_value=context) as create, mock.patch.object(
            build_cli, "show_presets"
        ) as presets, mock.patch.object(build_cli, "execute_context") as execute:
            self.assertEqual(build_cli.main(["presets", "--plain"]), 0)
        create.assert_called_once_with(mock.ANY, prepare_tools=False)
        presets.assert_called_once()
        execute.assert_not_called()
        with mock.patch.object(build_cli, "create_context", return_value=context), mock.patch.object(
            build_cli, "show_status"
        ) as status, mock.patch.object(build_cli, "execute_context") as execute:
            self.assertEqual(build_cli.main(["status", "--plain"]), 0)
        status.assert_called_once()
        execute.assert_not_called()
        with mock.patch.object(build_cli, "create_context", return_value=context) as create, mock.patch.object(
            build_cli, "open_runtime_directory"
        ) as open_runtime, mock.patch.object(build_cli, "execute_context") as execute:
            self.assertEqual(build_cli.main(["open-runtime", "--plain"]), 0)
        create.assert_called_once_with(mock.ANY, prepare_tools=False)
        open_runtime.assert_called_once()
        execute.assert_not_called()

    def test_plain_preset_listing_preserves_state_markers(self) -> None:
        context = mock.Mock()
        context.profile.presets = ("debug", "release")
        context.profile.default_preset = "debug"
        stdout = io.StringIO()
        build_cli.show_presets(
            BuildOutput(plain=True, stdout=stdout, stderr=io.StringIO()),
            context,
            "debug",
        )
        self.assertIn("debug [default, current]", stdout.getvalue())

    def test_canonical_direct_and_shell_commands_produce_equal_requests(self) -> None:
        cases = [
            (["stop", "--plain"], ["stop", "--plain"]),
            (["presets", "--preset", "debug", "--plain"], ["presets", "--preset", "debug", "--plain"]),
            (["status", "--preset", "debug", "--jobs", "3"], ["status", "--preset", "debug", "--jobs", "3"]),
            (["open-runtime", "--preset", "debug"], ["open-runtime", "--preset", "debug"]),
            (["configure", "--preset", "debug", "--fresh"], ["configure", "--preset", "debug", "--fresh"]),
            (["build", "--preset", "debug"], ["build", "--preset", "debug"]),
            (["clean", "--preset", "debug"], ["clean", "--preset", "debug"]),
            (
                ["purge", "--preset", "debug", "--all-presets", "--yes"],
                ["purge", "--preset", "debug", "--all-presets", "--yes"],
            ),
            (["rebuild", "--preset", "debug", "--target", "Core"], ["rebuild", "--preset", "debug", "--target", "Core"]),
            (
                [
                    "test",
                    "--preset",
                    "debug",
                    "--target",
                    "CoreTests",
                    "--filter",
                    "Core.*",
                    "--timeout",
                    "45",
                ],
                [
                    "test",
                    "--preset",
                    "debug",
                    "--target",
                    "CoreTests",
                    "--filter",
                    "Core.*",
                    "--timeout",
                    "45",
                ],
            ),
            (
                ["run", "--preset", "debug", "--args", "--hidden-window"],
                ["run", "--preset", "debug", "--args", "--hidden-window"],
            ),
        ]
        session = build_config.CommandRequest(build_config.Action.SHELL)
        for direct_argv, shell_parts in cases:
            with self.subTest(command=direct_argv[0]):
                expected = build_cli.parse_args(direct_argv)
                self.assertEqual(
                    build_cli.parse_shell_request(shell_parts, session, current_preset="debug"),
                    expected,
                )
                slash_alias = [f"/{shell_parts[0]}", *shell_parts[1:]]
                self.assertEqual(
                    build_cli.parse_shell_request(slash_alias, session, current_preset="debug"),
                    expected,
                )

    def test_shell_compact_forms_match_canonical_direct_requests(self) -> None:
        cases = [
            (["/build", "Core"], ["build", "--preset", "debug", "--target", "Core"]),
            (["rebuild"], ["rebuild", "--preset", "debug"]),
            (
                ["test", "CoreTests", "FJsonDocumentTests.*", "--timeout", "45"],
                [
                    "test",
                    "--preset",
                    "debug",
                    "--target",
                    "CoreTests",
                    "--filter",
                    "FJsonDocumentTests.*",
                    "--timeout",
                    "45",
                ],
            ),
            (
                ["run", "--hidden-window", "--project", "Example"],
                [
                    "run",
                    "--preset",
                    "debug",
                    "--args",
                    "--hidden-window",
                    "--project",
                    "Example",
                ],
            ),
        ]
        session = build_config.CommandRequest(build_config.Action.SHELL)
        for shell_parts, direct_argv in cases:
            with self.subTest(command=shell_parts[0]):
                self.assertEqual(
                    build_cli.parse_shell_request(shell_parts, session, current_preset="debug"),
                    build_cli.parse_args(direct_argv),
                )

    def test_shell_session_defaults_and_named_overrides_are_preserved(self) -> None:
        session = build_config.CommandRequest(
            build_config.Action.SHELL,
            jobs=7,
            profile="profile",
            preset="initial",
            cmake="custom-cmake",
            environment_setup="setup.bat",
            plain=True,
        )
        shell_request = build_cli.parse_shell_request(
            ["build", "--target", "Core", "--jobs", "3"],
            session,
            current_preset="debug",
        )
        direct_request = build_cli.parse_args(
            [
                "build",
                "--target",
                "Core",
                "--jobs",
                "3",
                "--profile",
                "profile",
                "--preset",
                "debug",
                "--cmake",
                "custom-cmake",
                "--environment-setup",
                "setup.bat",
                "--plain",
            ]
        )
        self.assertEqual(shell_request, direct_request)

    def test_shell_commands_reject_invalid_operands_from_shared_model(self) -> None:
        invalid = [
            ["stop", "extra"],
            ["presets", "extra"],
            ["status", "extra"],
            ["open-runtime", "extra"],
            ["configure", "extra"],
            ["build", "Core", "Extra"],
            ["clean", "extra"],
            ["purge", "extra"],
            ["rebuild", "Core", "Extra"],
            ["test", "CoreTests", "Core.*", "extra"],
            ["run", "--preset"],
        ]
        session = build_config.CommandRequest(build_config.Action.SHELL)
        for parts in invalid:
            with self.subTest(command=parts[0]), self.assertRaises(build_config.BuildToolError):
                build_cli.parse_shell_request(parts, session, current_preset="debug")

    def test_plain_option_is_available_after_command(self) -> None:
        request = build_cli.parse_args(["configure", "--plain"])
        self.assertTrue(request.plain)

    def test_configure_fresh_option_is_explicit(self) -> None:
        self.assertFalse(build_cli.parse_args(["configure"]).fresh)
        self.assertTrue(build_cli.parse_args(["configure", "--fresh"]).fresh)

    def test_global_options_before_uppercase_command_are_preserved(self) -> None:
        request = build_cli.parse_args(["--plain", "Build", "--target", "all"])
        self.assertIs(request.action, build_config.Action.BUILD)
        self.assertTrue(request.plain)

    def test_command_like_option_values_do_not_hide_uppercase_action(self) -> None:
        request = build_cli.parse_args(
            ["--cmake", "Build", "--profile=Run", "BUILD", "--target", "all"]
        )
        self.assertIs(request.action, build_config.Action.BUILD)
        self.assertEqual(request.cmake, "Build")
        self.assertEqual(request.profile, "Run")

    def test_windows_shell_split_preserves_paths_and_runtime_arguments(self) -> None:
        self.assertEqual(
            build_cli.split_shell_command(
                'run C:\\Temp\\foo "\\\\server\\share\\My File.txt" "" '
                '"C:\\Temp\\trailing\\\\" --define=a\\b --gtest_filter=Core.*',
                current_host="windows",
            ),
            [
                "run",
                "C:\\Temp\\foo",
                "\\\\server\\share\\My File.txt",
                "",
                "C:\\Temp\\trailing\\",
                "--define=a\\b",
                "--gtest_filter=Core.*",
            ],
        )

    def test_windows_shell_split_reverses_python_command_line_quoting(self) -> None:
        arguments = [
            "run",
            "C:\\Temp\\foo",
            "\\\\server\\share\\My File.txt",
            "",
            'quote"value',
            "C:\\Temp\\trailing\\",
            "--flag",
        ]
        self.assertEqual(
            build_cli.split_shell_command(
                subprocess.list2cmdline(arguments),
                current_host="windows",
            ),
            arguments,
        )

    def test_posix_shell_split_preserves_quoted_and_escaped_arguments(self) -> None:
        self.assertEqual(
            build_cli.split_shell_command(
                r"""run 'path with spaces' "" path\ with\ spaces --gtest_filter=Core.*""",
                current_host="linux",
            ),
            ["run", "path with spaces", "", "path with spaces", "--gtest_filter=Core.*"],
        )

    def test_shell_split_reports_malformed_quotes(self) -> None:
        with self.assertRaisesRegex(build_config.BuildToolError, "unmatched double quote"):
            build_cli.split_shell_command('run "unterminated', current_host="windows")
        with self.assertRaisesRegex(build_config.BuildToolError, "No closing quotation"):
            build_cli.split_shell_command("run 'unterminated", current_host="linux")

    def test_windows_shell_dispatch_preserves_runtime_arguments(self) -> None:
        base = mock.Mock()
        base.preset.name = "debug"
        base.current_host = "windows"
        child_context = mock.Mock()
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        with mock.patch.object(build_cli, "create_context", return_value=base), mock.patch.object(
            build_cli, "show_status"
        ), mock.patch.object(
            build_cli, "derive_context", return_value=child_context
        ) as derive, mock.patch.object(build_cli, "execute_context"), mock.patch(
            "builtins.input",
            side_effect=['run C:\\Temp\\foo "" --define=a\\b', "exit"],
        ):
            build_cli.run_shell(build_config.CommandRequest(build_config.Action.SHELL), output)
        child_request = derive.call_args.args[1]
        self.assertIs(child_request.action, build_config.Action.RUN)
        self.assertEqual(child_request.run_arguments, ("C:\\Temp\\foo", "", "--define=a\\b"))

    def test_malformed_shell_quoting_reports_error_and_keeps_session_open(self) -> None:
        base = mock.Mock()
        base.preset.name = "debug"
        base.current_host = "windows"
        stderr = io.StringIO()
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=stderr)
        with mock.patch.object(build_cli, "create_context", return_value=base), mock.patch.object(
            build_cli, "show_status"
        ), mock.patch.object(
            build_cli, "perf_counter", side_effect=[10.0, 10.25, 11.0]
        ), mock.patch("builtins.input", side_effect=['run "unterminated', "exit"]) as prompt:
            build_cli.run_shell(build_config.CommandRequest(build_config.Action.SHELL), output)
        self.assertEqual(prompt.call_count, 2)
        failure = stderr.getvalue()
        self.assertIn("ERROR: Command failed: Invalid shell command: unmatched double quote.", failure)
        self.assertIn("Elapsed: 0.25s", failure)
        self.assertNotIn("Action:", failure)

    def test_shell_validation_failure_has_action_context_and_accepts_slash_alias(self) -> None:
        base = mock.Mock()
        base.preset.name = "debug"
        base.current_host = "windows"
        stderr = io.StringIO()
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=stderr)
        with mock.patch.object(build_cli, "create_context", return_value=base), mock.patch.object(
            build_cli, "show_status"
        ), mock.patch.object(
            build_cli, "derive_context"
        ) as derive, mock.patch.object(
            build_cli, "perf_counter", side_effect=[20.0, 21.5, 22.0]
        ), mock.patch("builtins.input", side_effect=["/build Core Extra", "exit"]) as prompt:
            build_cli.run_shell(build_config.CommandRequest(build_config.Action.SHELL), output)
        failure = stderr.getvalue()
        self.assertEqual(prompt.call_count, 2)
        derive.assert_not_called()
        self.assertIn("ERROR: Build failed: build accepts at most one target.", failure)
        self.assertNotIn("/build accepts", failure)
        self.assertIn("Action: build", failure)
        self.assertIn("Preset: debug", failure)
        self.assertIn("Target: —", failure)
        self.assertIn("Elapsed: 1.50s", failure)

    def test_shell_child_failure_retains_context_command_and_elapsed_time(self) -> None:
        base = mock.Mock()
        base.preset.name = "debug"
        base.current_host = "windows"
        stderr = io.StringIO()
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=stderr)

        def derive(_base: object, child_request: build_config.CommandRequest) -> mock.Mock:
            child_context = mock.Mock()
            child_context.request = child_request
            child_context.preset.name = child_request.preset
            child_context.target = child_request.target
            return child_context

        error = build_config.BuildToolError(
            "compiler failed",
            command=["cmake", "--build", "Build/debug", "--target", "Core"],
            exit_code=2,
            recovery="Inspect the command output above, then rerun the same command.",
        )
        with mock.patch.object(build_cli, "create_context", return_value=base), mock.patch.object(
            build_cli, "show_status"
        ), mock.patch.object(
            build_cli, "derive_context", side_effect=derive
        ), mock.patch.object(
            build_cli, "execute_context", side_effect=error
        ) as execute, mock.patch.object(
            build_cli, "perf_counter", side_effect=[30.0, 34.75, 35.0]
        ), mock.patch("builtins.input", side_effect=["build Core", "exit"]) as prompt:
            build_cli.run_shell(build_config.CommandRequest(build_config.Action.SHELL), output)
        failure = stderr.getvalue()
        self.assertEqual(prompt.call_count, 2)
        execute.assert_called_once()
        self.assertIn("ERROR: Build failed: compiler failed", failure)
        self.assertIn("Action: build", failure)
        self.assertIn("Preset: debug", failure)
        self.assertIn("Target: Core", failure)
        self.assertIn("Command: cmake --build Build/debug --target Core", failure)
        self.assertIn("Exit code: 2", failure)
        self.assertIn("Elapsed: 4.75s", failure)
        self.assertIn("Inspect the command output above, then rerun the same command.", failure)

    def test_interrupted_shell_operation_reports_recovery_and_keeps_session_open(self) -> None:
        base = mock.Mock()
        base.preset.name = "debug"
        base.current_host = "windows"
        child_context = mock.Mock()
        child_context.request = build_config.CommandRequest(
            build_config.Action.REBUILD,
            preset="debug",
            target="all",
        )
        child_context.preset.name = "debug"
        child_context.target = "all"
        stderr = io.StringIO()
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=stderr)
        interruption = build_config.BuildToolInterruptedError(
            "Durin BuildTool was interrupted.",
            command=["cmake", "--build", "Build/debug"],
            recovery="Confirm the old process tree exited, then run rebuild --target all.",
        )
        with mock.patch.object(build_cli, "create_context", return_value=base), mock.patch.object(
            build_cli, "show_status"
        ), mock.patch.object(
            build_cli, "derive_context", return_value=child_context
        ), mock.patch.object(
            build_cli, "execute_context", side_effect=interruption
        ), mock.patch.object(
            build_cli, "perf_counter", side_effect=[40.0, 42.0, 43.0]
        ), mock.patch("builtins.input", side_effect=["rebuild", "exit"]) as prompt:
            build_cli.run_shell(build_config.CommandRequest(build_config.Action.SHELL), output)
        failure = stderr.getvalue()
        self.assertEqual(prompt.call_count, 2)
        self.assertIn("ERROR: Rebuild failed: Durin BuildTool was interrupted.", failure)
        self.assertIn("Action: rebuild", failure)
        self.assertIn("Elapsed: 2.00s", failure)
        self.assertIn("Confirm the old process tree exited, then run rebuild --target all.", failure)

    def test_rebuild_defaults_to_all(self) -> None:
        self.assertEqual(build_cli.parse_args(["rebuild"]).target, "all")

    def test_shell_stop_accepts_bare_command(self) -> None:
        base = mock.Mock()
        base.preset.name = "debug"
        stdout = io.StringIO()
        output = BuildOutput(plain=True, stdout=stdout, stderr=io.StringIO())
        with mock.patch.object(build_cli, "create_context", return_value=base), mock.patch.object(
            build_cli, "show_status"
        ), mock.patch.object(build_cli, "stop_active_operation", return_value=True) as stop, mock.patch(
            "builtins.input", side_effect=["stop", "exit"]
        ):
            build_cli.run_shell(build_config.CommandRequest(build_config.Action.SHELL), output)
        stop.assert_called_once_with()
        self.assertIn("Stopped the active BuildTool operation.", stdout.getvalue())

    def test_shell_help_prefers_commands_without_slashes(self) -> None:
        stdout = io.StringIO()
        build_cli.print_shell_help(BuildOutput(plain=True, stdout=stdout, stderr=io.StringIO()))
        help_text = stdout.getvalue()
        self.assertIn("  stop ", help_text)
        self.assertIn("  build ", help_text)
        self.assertNotIn("  /build ", help_text)

    def test_wrapper_uses_new_entrypoint_and_forwards_arguments(self) -> None:
        content = (REPO_ROOT / "BuildTool.bat").read_text(encoding="utf-8")
        self.assertIn('set "VSLANG=1033"', content)
        self.assertIn('Engine\\Scripts\\Build\\durin_build_tool\\__main__.py" %*', content)
        self.assertNotIn("agent_build.py", content)

    def test_requirements_pin_rich_and_libclang(self) -> None:
        content = (REPO_ROOT / "requirements.txt").read_text(encoding="utf-8")
        self.assertRegex(content, r"(?m)^libclang==\d+\.\d+\.\d+$")
        self.assertRegex(content, r"(?m)^rich==\d+\.\d+\.\d+$")

    def test_setup_prepares_configuration_before_each_preflight(self) -> None:
        content = (REPO_ROOT / "Setup.bat").read_text(encoding="utf-8")
        bootstrap = content[content.index(":bootstrap") : content.index(":prepare_worktree")]
        linked = content[content.index(":prepare_worktree") : content.index(":end")]
        self.assertLess(bootstrap.index("InitializeAgentConfig.bat"), bootstrap.index("Preflight.bat"))
        self.assertLess(bootstrap.index("Preflight.bat"), bootstrap.index("SetupPython.bat"))
        self.assertLess(linked.index("PrepareWorktree.bat"), linked.index("Preflight.bat"))
        self.assertEqual(content.count("InitializeAgentConfig.bat"), 1)

    def test_agent_config_initializer_supports_python_launcher_before_venv_exists(self) -> None:
        content = (REPO_ROOT / "Engine/Scripts/Bootstrap/InitializeAgentConfig.bat").read_text(
            encoding="utf-8"
        )
        self.assertIn("where py", content)
        self.assertIn('py -3 "%SCRIPT_DIR%initialize_agent_config.py" %*', content)

    def test_worktree_preparer_supports_python_launcher_before_venv_is_linked(self) -> None:
        content = (REPO_ROOT / "Engine/Scripts/Bootstrap/PrepareWorktree.bat").read_text(
            encoding="utf-8"
        )
        self.assertIn("where py", content)
        self.assertIn('py -3 "%SCRIPT_DIR%prepare_worktree.py" %*', content)

    def test_worktree_preparer_links_complete_agent_directory(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source"
            target = root / "target"
            (source / ".agents").mkdir(parents=True)
            (target / ".agents").mkdir(parents=True)
            (target / ".agents" / "build-config.json").write_text("local", encoding="utf-8")
            with mock.patch.object(prepare_worktree, "REPO_ROOT", target), mock.patch.object(
                prepare_worktree, "create_link"
            ) as create:
                prepare_worktree.prepare_agent_link(source, link_type="symlink", dry_run=False)
            self.assertFalse((target / ".agents").exists())
            self.assertEqual(
                (target / ".agents.pre-link-backup" / "build-config.json").read_text(encoding="utf-8"),
                "local",
            )
            create.assert_called_once_with(
                (source / ".agents").resolve(),
                (target / ".agents").absolute(),
                link_type="symlink",
                target_is_directory=True,
                dry_run=False,
            )


class SetupPreflightTests(unittest.TestCase):
    def test_cmake_minimum_version_is_checked(self) -> None:
        completed = subprocess.CompletedProcess(["cmake", "--version"], 0, "cmake version 3.23.5\n", "")
        with mock.patch.object(setup_preflight, "command_path", return_value="cmake"), mock.patch.object(
            setup_preflight.subprocess, "run", return_value=completed
        ):
            error = setup_preflight.check_cmake()
        self.assertIn("requires 3.24 or newer", error or "")

    def test_visual_studio_environment_override_is_loaded_from_agent_config(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            config = root / ".agents" / "build-config.json"
            config.parent.mkdir(parents=True)
            config.write_text(
                json.dumps(
                    {
                        "environmentSetup": {
                            "script": str(root / "toolchain/VsDevCmd.bat"),
                            "arguments": ["-arch=x64", "-host_arch=x64"],
                        }
                    }
                ),
                encoding="utf-8",
            )
            with mock.patch.object(setup_preflight, "REPO_ROOT", root):
                script, arguments = setup_preflight.configured_visual_studio_environment()
        self.assertEqual(script, (root / "toolchain/VsDevCmd.bat").resolve())
        self.assertEqual(arguments, ["-arch=x64", "-host_arch=x64"])

    def test_vulkan_sdk_check_reports_every_required_file(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            error = setup_preflight.check_vulkan_sdk({"VULKAN_SDK": directory})
        self.assertIn("vulkan.h", error or "")
        self.assertIn("vk_mem_alloc.h", error or "")
        self.assertIn("vulkan-1.lib", error or "")

    def test_old_msvc_toolset_has_actionable_version_error(self) -> None:
        environment = {"PATH": "tools", "VCTOOLSVERSION": "14.43.34808"}
        with mock.patch.object(setup_preflight, "command_path", return_value="cl.exe"):
            error = setup_preflight.check_msvc_version(environment)
        self.assertIn("requires 14.44 or newer", error or "")
        self.assertIn("std::format_string", error or "")


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

    def test_failure_without_derived_context_uses_available_request_details(self) -> None:
        stderr = io.StringIO()
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=stderr)
        request = build_config.CommandRequest(
            build_config.Action.TEST,
            target="CoreTests",
            preset="debug",
        )
        output.failure(build_config.BuildToolError("validation failed"), None, 0.5, request=request)
        text = stderr.getvalue()
        self.assertIn("ERROR: Test failed: validation failed", text)
        self.assertIn("Action: test", text)
        self.assertIn("Preset: debug", text)
        self.assertIn("Target: CoreTests", text)

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
        with mock.patch.object(build_core, "load_visual_studio_environment_cache", return_value=None), mock.patch.object(
            build_core, "write_visual_studio_environment_cache"
        ), mock.patch.object(build_core, "find_vsdevcmd", return_value=Path("VsDevCmd.bat")), mock.patch.object(
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

    def test_visual_studio_environment_cache_reuses_delta_and_invalidates_for_compiler_change(self) -> None:
        profile = replace(
            self.make_profile(),
            environment_provider=build_config.EnvironmentProvider.VISUAL_STUDIO,
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            script = root / "VsDevCmd.bat"
            compiler = root / "cl.exe"
            cache = root / "environment.json"
            script.touch()
            compiler.touch()
            captured = {
                "PATH": str(root) + os.pathsep + "original-path",
                "VSLANG": "1033",
                "VSINSTALLDIR": str(root),
                "DURIN_LIVE_VALUE": "first",
            }
            with mock.patch.object(build_core, "find_vsdevcmd", return_value=script), mock.patch.object(
                build_core, "visual_studio_environment_cache_path", return_value=cache
            ), mock.patch.object(
                build_core, "capture_setup_environment", return_value=captured
            ) as capture, mock.patch.object(
                build_core, "detect_msvc_showincludes_prefix", return_value="Note: including file:  "
            ) as detect_prefix, mock.patch.object(
                build_core.shutil, "which", return_value=str(compiler)
            ), mock.patch.dict(
                os.environ,
                {"DURIN_LIVE_VALUE": "first", "PATH": "original-path"},
                clear=True,
            ):
                first = build_core.build_environment(
                    profile,
                    build_config.EnvironmentSetup(),
                    current_host="windows",
                )
                os.environ["DURIN_LIVE_VALUE"] = "second"
                os.environ["PATH"] = "new-path"
                second = build_core.build_environment(
                    profile,
                    build_config.EnvironmentSetup(),
                    current_host="windows",
                )
                compiler.write_text("updated", encoding="utf-8")
                build_core.build_environment(
                    profile,
                    build_config.EnvironmentSetup(),
                    current_host="windows",
                )
        self.assertEqual(first["PATH"], str(root) + os.pathsep + "original-path")
        self.assertEqual(second["DURIN_LIVE_VALUE"], "second")
        self.assertEqual(second["PATH"], str(root) + os.pathsep + "new-path")
        self.assertEqual(capture.call_count, 2)
        self.assertEqual(detect_prefix.call_count, 2)

    def test_visual_studio_environment_rejects_localized_compiler_output(self) -> None:
        profile = replace(
            self.make_profile(),
            environment_provider=build_config.EnvironmentProvider.VISUAL_STUDIO,
        )
        with mock.patch.object(build_core, "load_visual_studio_environment_cache", return_value=None), mock.patch.object(
            build_core, "find_vsdevcmd", return_value=Path("VsDevCmd.bat")
        ), mock.patch.object(
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
        self.assertFalse(run.call_args.kwargs["show_heartbeat"])

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

    def test_configure_preserves_cache_unless_fresh_is_requested(self) -> None:
        preset = self.make_preset()
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(
            build_core, "preset_build_directory", return_value=Path(directory)
        ), mock.patch.object(build_core, "require_english_msvc_ninja_prefix"), mock.patch.object(
            build_core, "run_command"
        ) as run:
            context = build_config.BuildContext(
                build_config.CommandRequest(build_config.Action.CONFIGURE),
                build_config.LocalConfig(),
                self.make_profile(),
                {"debug": preset},
                preset,
                "windows",
                cmake="cmake",
                environment={},
            )
            build_core.perform_action(context, output)
            self.assertEqual(run.call_args.args[0], ["cmake", "--preset", "debug"])

            context.request = replace(context.request, fresh=True)
            build_core.perform_action(context, output)
            self.assertEqual(run.call_args.args[0], ["cmake", "--fresh", "--preset", "debug"])

    def test_configure_recovers_an_unusable_existing_cache_with_fresh(self) -> None:
        preset = self.make_preset()
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory) / "CMakeCache.txt"
            cache.write_text("CMAKE_MAKE_PROGRAM:FILEPATH=CMAKE_MAKE_PROGRAM-NOTFOUND\n", encoding="utf-8")
            with mock.patch.object(
                build_core, "preset_build_directory", return_value=Path(directory)
            ), mock.patch.object(build_core, "require_english_msvc_ninja_prefix"), mock.patch.object(
                build_core, "run_command"
            ) as run:
                context = build_config.BuildContext(
                    build_config.CommandRequest(build_config.Action.CONFIGURE),
                    build_config.LocalConfig(),
                    self.make_profile(),
                    {"debug": preset},
                    preset,
                    "windows",
                    cmake="cmake",
                    environment={},
                )
                build_core.perform_action(context, output)
        self.assertEqual(run.call_args.args[0], ["cmake", "--fresh", "--preset", "debug"])

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

    def test_inaccessible_lock_reports_acl_recovery(self) -> None:
        path = Path("checkout.lock")
        denied = PermissionError(13, "Permission denied", str(path))
        with mock.patch.object(Path, "open", side_effect=denied), mock.patch.object(
            build_core, "recover_inaccessible_windows_lock", return_value=False
        ), self.assertRaises(build_config.BuildToolError) as raised:
            build_core.open_checkout_lock(path)
        self.assertIn("file-permission problem", str(raised.exception))
        self.assertIn("icacls", raised.exception.recovery)
        self.assertIn("Remove-Item", raised.exception.recovery)

    def test_inaccessible_windows_lock_is_reopened_after_stale_recovery(self) -> None:
        path = Path("checkout.lock")
        handle = mock.Mock()
        denied = PermissionError(13, "Permission denied", str(path))
        with mock.patch.object(Path, "open", side_effect=[denied, handle]), mock.patch.object(
            build_core, "recover_inaccessible_windows_lock", return_value=True
        ):
            self.assertIs(build_core.open_checkout_lock(path), handle)

    def test_windows_lock_acl_is_reset_to_directory_inheritance(self) -> None:
        result = mock.Mock(returncode=0)
        with mock.patch.object(build_core.os, "name", "nt"), mock.patch.object(
            build_core.subprocess, "run", return_value=result
        ) as run:
            self.assertTrue(build_core.normalize_windows_lock_acl(Path("checkout.lock")))
        self.assertEqual(run.call_args.args[0], ["icacls", "checkout.lock", "/reset", "/q"])

    def test_windows_lock_acl_reset_is_best_effort(self) -> None:
        with mock.patch.object(build_core.os, "name", "nt"), mock.patch.object(
            build_core.subprocess, "run", return_value=mock.Mock(returncode=5)
        ):
            self.assertFalse(build_core.normalize_windows_lock_acl(Path("checkout.lock")))

    def test_stop_ignores_stale_unowned_lock(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "checkout.lock"
            path.write_text(json.dumps({"pid": 424242}), encoding="utf-8")
            with mock.patch.object(build_core, "lock_file_path", return_value=path), mock.patch.object(
                build_core.subprocess, "run"
            ) as run, mock.patch.object(build_core.os, "killpg", create=True) as killpg:
                self.assertFalse(build_core.stop_active_operation())
            run.assert_not_called()
            killpg.assert_not_called()

    def test_stop_terminates_process_recorded_by_owned_lock(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "checkout.lock"
            with build_core.BuildToolLock(path, {"pid": 424242}), mock.patch.object(
                build_core, "lock_file_path", return_value=path
            ):
                if os.name == "nt":
                    result = mock.Mock(returncode=0)
                    with mock.patch.object(build_core.subprocess, "run", return_value=result) as run:
                        self.assertTrue(build_core.stop_active_operation())
                    self.assertEqual(run.call_args.args[0][:3], ["taskkill", "/PID", "424242"])
                else:
                    with mock.patch.object(build_core.os, "killpg") as killpg:
                        self.assertTrue(build_core.stop_active_operation())
                    killpg.assert_called_once_with(424242, build_core.signal.SIGTERM)

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

    def test_run_command_does_not_inherit_buildtool_handles(self) -> None:
        process = mock.Mock()
        process.wait.return_value = 0
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        with mock.patch.object(build_core.subprocess, "Popen", return_value=process) as popen:
            build_core.run_command(["cmake", "--version"], environment={}, output=output)
        self.assertTrue(popen.call_args.kwargs["close_fds"])

    def test_command_timeout_terminates_child_process_tree(self) -> None:
        process = mock.Mock()
        process.wait.side_effect = build_core.subprocess.TimeoutExpired(["CoreTests"], 0)
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        with mock.patch.object(build_core.subprocess, "Popen", return_value=process), mock.patch.object(
            build_core,
            "terminate_process_tree",
        ) as terminate, self.assertRaisesRegex(build_config.BuildToolError, "timed out"):
            build_core.run_command(
                ["CoreTests"],
                environment={},
                output=output,
                timeout_seconds=0.001,
            )
        terminate.assert_called_once_with(process)

    def test_native_test_failure_does_not_leave_recovery_marker(self) -> None:
        preset = self.make_preset()
        context = build_config.BuildContext(
            build_config.CommandRequest(build_config.Action.TEST, target="CoreTests"),
            build_config.LocalConfig(),
            self.make_profile(),
            {"debug": preset},
            preset,
            "windows",
            cmake="cmake",
            jobs=1,
            environment={},
        )
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            marker = root / "interrupted.json"
            lock = root / "checkout.lock"
            with mock.patch.object(build_core, "interruption_marker_path", return_value=marker), mock.patch.object(
                build_core, "lock_file_path", return_value=lock
            ), mock.patch.object(build_core, "perform_action"), mock.patch.object(
                build_core,
                "run_native_test",
                side_effect=build_config.BuildToolError("test failed"),
            ), self.assertRaisesRegex(build_config.BuildToolError, "test failed"):
                build_core.execute_context(context, output, confirm_purge=lambda _paths, _all: False)
            self.assertFalse(marker.exists())

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

    def test_dry_run_does_not_create_config(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.create_repo(root)
            target = agent_config.ensure_agent_config(root, dry_run=True)
            self.assertFalse(target.exists())


if __name__ == "__main__":
    unittest.main()
