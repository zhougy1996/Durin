from __future__ import annotations

import argparse
import io
import json
import os
import shutil
import subprocess
import tempfile
import unittest
import zipfile
from dataclasses import replace
from pathlib import Path
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[3]
DEV_TOOL_DIR = REPO_ROOT / "Tools" / "DurinDevTool"
if str(DEV_TOOL_DIR) not in os.sys.path:
    os.sys.path.insert(0, str(DEV_TOOL_DIR))

from durin_dev_tool.build import operations as build_cli
from durin_dev_tool.build import config as build_config
from durin_dev_tool.build import core as build_core
from durin_dev_tool.build import descriptors as build_descriptors
from durin_dev_tool.build import scaffolding as build_scaffolding
from durin_dev_tool.build.handler import request_from_namespace
from durin_dev_tool.build.output import BuildOutput
from durin_dev_tool.registry import CommandRegistry

def parse_build_request(arguments: list[str]) -> build_config.CommandRequest:
    _spec, namespace = CommandRegistry().parse(arguments)
    if getattr(namespace, "selected_preset", ""):
        namespace.preset = namespace.selected_preset
    return request_from_namespace(namespace)


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

    def test_repository_profile_orders_presets_for_interactive_selection(self) -> None:
        profile = build_config.load_profiles()["windows-msvc-x64"]
        self.assertEqual(
            profile.presets,
            (
                "Win64-Debug-DurinEditor-Tests",
                "Win64-Debug-DurinEditor",
                "Win64-Release-DurinEditor",
                "Win64-Release-DurinEditor-Profiling",
                "Win64-Debug-DurinGame",
                "Win64-Release-DurinGame",
                "Win64-Release-DurinGame-Profiling",
                "Win64-Shipping-DurinGame",
            ),
        )

    def test_profiling_presets_are_release_isolated_and_enable_tracy(self) -> None:
        presets = build_config.load_configure_presets()
        for runtime_variant in ("DurinEditor", "DurinGame"):
            preset = presets[f"Win64-Release-{runtime_variant}-Profiling"]
            self.assertEqual(
                build_config.preset_cache_string(preset, "CMAKE_BUILD_TYPE"),
                "Release",
            )
            self.assertEqual(
                build_config.preset_cache_string(preset, "DURIN_RUNTIME_VARIANT"),
                runtime_variant,
            )
            self.assertEqual(
                build_config.preset_cache_string(preset, "DURIN_PRESET_ROLE"),
                "Profiling",
            )
            self.assertTrue(
                build_config.preset_cache_bool(preset, "DURIN_ENABLE_TRACY")
            )
            self.assertEqual(
                build_config.preset_output_configuration(preset),
                "Release-Profiling",
            )

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

    def test_output_configuration_uses_preset_role(self) -> None:
        standard = build_config.ConfigurePreset(
            "debug",
            {"cacheVariables": {"CMAKE_BUILD_TYPE": "Debug"}},
        )
        profiling = build_config.ConfigurePreset(
            "profiling",
            {
                "cacheVariables": {
                    "CMAKE_BUILD_TYPE": "Release",
                    "DURIN_PRESET_ROLE": "Profiling",
                }
            },
        )
        self.assertEqual(build_config.preset_output_configuration(standard), "Debug")
        self.assertEqual(build_config.preset_output_configuration(profiling), "Release-Profiling")

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
                bundled_ninja = parent / "Ninja" / "ninja.exe"
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


class ScaffoldingInfrastructureTests(unittest.TestCase):
    @staticmethod
    def write_project(path: Path, name: str) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(
            json.dumps({"ProjectName": name, "ModuleDirs": {}, "BaseModules": []}, indent=4)
            + "\n",
            encoding="utf-8",
        )

    @classmethod
    def create_discovery_workspace(cls, root: Path) -> None:
        cls.write_project(root / "Engine" / "Engine.dproject", "Engine")
        cls.write_project(root / "Sandbox" / "Sandbox.dproject", "Sandbox")
        (root / "CMakeLists.txt").write_text(
            "add_subdirectory(Engine)\nadd_subdirectory(\"Sandbox\")\n",
            encoding="utf-8",
        )
        module_dir = root / "Engine" / "Source" / "Runtime" / "Core"
        module_dir.mkdir(parents=True)
        (module_dir / "CMakeLists.txt").write_text(
            "add_durin_module(Core)\n",
            encoding="utf-8",
        )

    @staticmethod
    def snapshot(root: Path) -> tuple[tuple[str, ...], dict[str, bytes]]:
        directories = tuple(
            sorted(
                path.relative_to(root).as_posix()
                for path in root.rglob("*")
                if path.is_dir()
            )
        )
        files = {
            path.relative_to(root).as_posix(): path.read_bytes()
            for path in sorted(root.rglob("*"))
            if path.is_file()
        }
        return directories, files

    @staticmethod
    def transaction_plan(root: Path) -> build_scaffolding.ScaffoldPlan:
        generated = root / "Generated"
        descriptor = generated / "Gameplay.dmodule"
        root_cmake = root / "CMakeLists.txt"
        return build_scaffolding.ordered_plan(
            root,
            (root,),
            directories=(generated,),
            files=(
                (
                    descriptor,
                    b'{\n    "ModuleName": "Gameplay"\n}\n',
                ),
            ),
            replacements=(
                (
                    root_cmake,
                    root_cmake.read_bytes() + b"add_subdirectory(Generated)\n",
                ),
            ),
        )

    def test_workspace_discovery_cross_checks_root_cmake_and_targets(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.create_discovery_workspace(root)
            discovery = build_scaffolding.discover_workspace_projects(root)
            self.assertEqual(
                tuple(project.descriptor.name for project in discovery.projects),
                ("Engine", "Sandbox"),
            )
            self.assertEqual(discovery.projects[1].cmake_registration, "Sandbox")
            self.assertIn("Core", discovery.cmake_targets)
            with self.assertRaisesRegex(build_config.BuildToolError, "CMake target"):
                build_scaffolding.require_available_cmake_target("core", discovery)

            (root / "CMakeLists.txt").write_text(
                "add_subdirectory(Engine)\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(build_config.BuildToolError, "Sandbox.*exactly one"):
                build_scaffolding.discover_workspace_projects(root)

    def test_destination_checks_cover_containment_overlap_existing_and_case(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.create_discovery_workspace(root)
            discovery = build_scaffolding.discover_workspace_projects(root)
            with self.assertRaisesRegex(build_config.BuildToolError, "inside"):
                build_scaffolding.validate_destination(
                    root.parent / "Outside",
                    discovery,
                    label="Project destination",
                )
            with self.assertRaisesRegex(build_config.BuildToolError, "overlaps project"):
                build_scaffolding.validate_destination(
                    root / "Engine" / "Nested",
                    discovery,
                    label="Project destination",
                )
            existing = root / "Existing"
            existing.mkdir()
            with self.assertRaisesRegex(build_config.BuildToolError, "already exists"):
                build_scaffolding.validate_destination(
                    existing,
                    discovery,
                    label="Project destination",
                )
            case_path = root / "MixedCase"
            case_path.mkdir()
            with self.assertRaisesRegex(build_config.BuildToolError, "case-insensitive"):
                build_scaffolding.validate_destination(
                    root / "mixedcase",
                    discovery,
                    label="Project destination",
                )

    def test_templates_are_disk_assets_with_explicit_deterministic_variables(self) -> None:
        renderer = build_scaffolding.TemplateRenderer()
        module_variables = {
            "MODULE_NAME": "Gameplay",
            "LINK_TYPE": "Shared",
            "PCH": "Self",
            "PRIVATE_DEPENDENCIES": '["Core"]',
            "PUBLIC_DEPENDENCIES": "[]",
            "OPTIONAL_PRIVATE_DEPENDENCIES": "[]",
            "OPTIONAL_PUBLIC_DEPENDENCIES": "[]",
        }
        first = renderer.render("module/descriptor.json.template", module_variables)
        second = renderer.render("module/descriptor.json.template", module_variables)
        self.assertEqual(first, second)
        self.assertEqual(json.loads(first)["ModuleName"], "Gameplay")
        rendered_templates = {
            "module/entry_point.cpp.template": renderer.render(
                "module/entry_point.cpp.template",
                {"MODULE_NAME": "Gameplay"},
            ),
            "module/api.h.template": renderer.render(
                "module/api.h.template",
                {"MODULE_NAME_UPPER": "GAMEPLAY"},
            ),
            "module/CMakeLists.txt.template": renderer.render(
                "module/CMakeLists.txt.template",
                {"MODULE_NAME": "Gameplay"},
            ),
            "module/pch.h.template": renderer.render(
                "module/pch.h.template",
                {},
            ),
            "project/descriptor.json.template": renderer.render(
                "project/descriptor.json.template",
                {"PROJECT_NAME": "MyGame"},
            ),
            "project/CMakeLists.txt.template": renderer.render(
                "project/CMakeLists.txt.template",
                {"PROJECT_NAME": "MyGame"},
            ),
            "project/setup.cmake.template": renderer.render(
                "project/setup.cmake.template",
                {"PROJECT_NAME": "MyGame"},
            ),
        }
        self.assertEqual(
            json.loads(rendered_templates["project/descriptor.json.template"])["ProjectName"],
            "MyGame",
        )
        for content in rendered_templates.values():
            self.assertNotIn(b"{{", content)
            self.assertNotIn(str(REPO_ROOT).encode(), content)
        self.assertTrue(
            (build_scaffolding.TEMPLATE_DIR / "module" / "descriptor.json.template").is_file()
        )
        with self.assertRaisesRegex(build_config.BuildToolError, "missing MODULE_NAME_UPPER"):
            renderer.render("module/api.h.template", {"MODULE_NAME": "Gameplay"})
        with self.assertRaisesRegex(build_config.BuildToolError, "unknown EXTRA"):
            renderer.render(
                "module/CMakeLists.txt.template",
                {"MODULE_NAME": "Gameplay", "EXTRA": "value"},
            )

    def test_dry_run_format_is_stable_and_does_not_mutate(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "CMakeLists.txt").write_bytes(b"add_subdirectory(Engine)\r\n")
            before = self.snapshot(root)
            plan = self.transaction_plan(root)
            plain = plan.format(plain=True)
            styled = plan.format(plain=False)
            self.assertEqual(before, self.snapshot(root))
            self.assertEqual(
                plain,
                "\n".join(
                    (
                        "Scaffolding plan (3 operations)",
                        "  create directory: Generated",
                        "  create file: Generated/Gameplay.dmodule",
                        "  replace file: CMakeLists.txt",
                    )
                ),
            )
            self.assertEqual(styled.replace("[cyan]", "").replace("[/cyan]", ""), plain)

    def test_transaction_success_preserves_unrelated_bytes_and_reparses_outputs(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "CMakeLists.txt").write_bytes(b"add_subdirectory(Engine)\r\n")
            unrelated = root / "Unrelated.bin"
            unrelated.write_bytes(b"\x00unchanged\r\n")
            plan = self.transaction_plan(root)
            build_scaffolding.execute_plan(plan)
            self.assertEqual(unrelated.read_bytes(), b"\x00unchanged\r\n")
            self.assertEqual(
                (root / "Generated" / "Gameplay.dmodule").read_bytes(),
                b'{\n    "ModuleName": "Gameplay"\n}\n',
            )
            self.assertEqual(
                (root / "CMakeLists.txt").read_bytes(),
                b"add_subdirectory(Engine)\r\nadd_subdirectory(Generated)\n",
            )
            self.assertFalse(
                any(".backup." in path.name or ".write." in path.name for path in root.rglob("*"))
            )

    def test_every_injected_write_failure_rolls_back_exactly(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "CMakeLists.txt").write_bytes(b"add_subdirectory(Engine)\r\n")
            boundaries: list[tuple[str, int, Path]] = []
            build_scaffolding.execute_plan(
                self.transaction_plan(root),
                failure_injector=lambda phase, index, path: boundaries.append((phase, index, path)),
            )
        self.assertGreater(len(boundaries), 0)

        for failing_boundary in range(1, len(boundaries) + 1):
            with self.subTest(boundary=failing_boundary), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                (root / "CMakeLists.txt").write_bytes(b"add_subdirectory(Engine)\r\n")
                unrelated = root / "Unrelated.bin"
                unrelated.write_bytes(b"\xffkeep")
                before = self.snapshot(root)

                def fail_at_boundary(phase: str, index: int, path: Path) -> None:
                    if index == failing_boundary:
                        raise RuntimeError(f"injected at {phase}: {path}")

                with self.assertRaisesRegex(RuntimeError, "injected"):
                    build_scaffolding.execute_plan(
                        self.transaction_plan(root),
                        failure_injector=fail_at_boundary,
                    )
                self.assertEqual(self.snapshot(root), before)

    def test_validation_failure_rolls_back_and_plan_rejects_outside_roots(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "CMakeLists.txt").write_bytes(b"add_subdirectory(Engine)\n")
            before = self.snapshot(root)
            with self.assertRaisesRegex(build_config.BuildToolError, "unbalanced"):
                build_scaffolding.ordered_plan(
                    root,
                    (root,),
                    replacements=((root / "CMakeLists.txt", b"add_subdirectory(Engine\n"),),
                )
            self.assertEqual(self.snapshot(root), before)

            def reject_final_state(plan: build_scaffolding.ScaffoldPlan) -> None:
                raise build_config.BuildToolError("injected descriptor validation failure")

            validation_plan = build_scaffolding.ordered_plan(
                root,
                (root,),
                directories=(root / "Generated",),
                files=(
                    (
                        root / "Generated" / "Gameplay.dmodule",
                        b'{\n    "ModuleName": "Gameplay"\n}\n',
                    ),
                ),
                validators=(reject_final_state,),
            )
            with self.assertRaisesRegex(build_config.BuildToolError, "validation failure"):
                build_scaffolding.execute_plan(validation_plan)
            self.assertEqual(self.snapshot(root), before)

            with self.assertRaisesRegex(build_config.BuildToolError, "outside"):
                build_scaffolding.ordered_plan(
                    root,
                    (root,),
                    files=((root.parent / "outside.txt", b"no"),),
                )


class ModuleScaffoldingTests(unittest.TestCase):
    @staticmethod
    def write_json(path: Path, value: object) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")

    @classmethod
    def create_workspace(cls, root: Path) -> tuple[Path, Path]:
        (root / "CMakeLists.txt").write_text(
            "add_subdirectory(Engine)\nadd_subdirectory(Sandbox)\n",
            encoding="utf-8",
        )
        engine = root / "Engine"
        sandbox = root / "Sandbox"
        engine_project = engine / "Engine.dproject"
        sandbox_project = sandbox / "Sandbox.dproject"
        cls.write_json(
            engine_project,
            {
                "ProjectName": "Engine",
                "ModuleDirs": {
                    "Core": "Source/Runtime/Core",
                    "AssetCore": "Source/Runtime/AssetCore",
                    "DurinEd": "Source/Editor/DurinEd",
                },
                "BaseModules": ["Core"],
                "ExtraModules": {
                    "DurinEditor": {"Modules": ["DurinEd"]},
                    "DurinGame": {"Modules": []},
                },
            },
        )
        cls.write_json(
            engine / "Source" / "Runtime" / "Core" / "Core.dmodule",
            {"ModuleName": "Core"},
        )
        cls.write_json(
            engine / "Source" / "Runtime" / "AssetCore" / "AssetCore.dmodule",
            {"ModuleName": "AssetCore", "PrivateDependencies": ["Core"]},
        )
        cls.write_json(
            engine / "Source" / "Editor" / "DurinEd" / "DurinEd.dmodule",
            {"ModuleName": "DurinEd", "PrivateDependencies": ["Core"]},
        )
        cls.write_json(
            sandbox_project,
            {
                "ProjectName": "Sandbox",
                "ModuleDirs": {"Sandbox": "Source/Runtime/Sandbox"},
                "BaseModules": ["Sandbox"],
                "ExtraModules": {
                    "DurinEditor": {"Modules": []},
                    "DurinGame": {"Modules": []},
                },
            },
        )
        cls.write_json(
            sandbox / "Source" / "Runtime" / "Sandbox" / "Sandbox.dmodule",
            {"ModuleName": "Sandbox", "PrivateDependencies": ["Core"]},
        )
        for module_name, module_root in (
            ("Core", engine / "Source" / "Runtime" / "Core"),
            ("AssetCore", engine / "Source" / "Runtime" / "AssetCore"),
            ("DurinEd", engine / "Source" / "Editor" / "DurinEd"),
            ("Sandbox", sandbox / "Source" / "Runtime" / "Sandbox"),
        ):
            (module_root / "CMakeLists.txt").write_text(
                f"add_durin_module({module_name})\n",
                encoding="utf-8",
            )
        (sandbox / "Source" / "Editor").mkdir(parents=True)
        return engine_project, sandbox_project

    @staticmethod
    def snapshot(root: Path) -> dict[str, bytes]:
        return {
            path.relative_to(root).as_posix(): path.read_bytes()
            for path in root.rglob("*")
            if path.is_file()
        }

    def test_runtime_defaults_generate_complete_module_and_base_registration(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _, sandbox_project = self.create_workspace(root)
            request = parse_build_request(
                [
                    "create",
                    "module",
                    "Gameplay",
                    "--project",
                    "Sandbox/Sandbox.dproject",
                    "--private-dependency",
                    "Core",
                ]
            )
            plan = build_scaffolding.plan_module_creation(request, root)
            self.assertEqual(len(plan.operations), 9)
            build_scaffolding.execute_plan(plan)

            module_root = root / "Sandbox" / "Source" / "Runtime" / "Gameplay"
            descriptor = json.loads((module_root / "Gameplay.dmodule").read_text(encoding="utf-8"))
            project = json.loads(sandbox_project.read_text(encoding="utf-8"))
            self.assertEqual(descriptor["LinkType"], "Shared")
            self.assertEqual(descriptor["PCH"], "Self")
            self.assertEqual(descriptor["PrivateDependencies"], ["Core"])
            self.assertEqual(project["ModuleDirs"]["Gameplay"], "Source/Runtime/Gameplay")
            self.assertEqual(project["BaseModules"], ["Sandbox", "Gameplay"])
            self.assertTrue((module_root / "Private" / "GameplayModule.cpp").is_file())
            self.assertTrue((module_root / "Private" / "PCH.Gameplay.h").is_file())
            self.assertTrue((module_root / "Public" / "GameplayAPI.h").is_file())
            self.assertIn(
                "add_durin_module(Gameplay)",
                (module_root / "CMakeLists.txt").read_text(encoding="utf-8"),
            )
            self.assertEqual(
                build_descriptors.load_workspace_descriptors(root)
                .find_module("Gameplay")
                .owning_project,
                "Sandbox",
            )

    def test_editor_overrides_preserve_all_dependency_categories_and_profiles(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _, sandbox_project = self.create_workspace(root)
            request = parse_build_request(
                [
                    "create",
                    "module",
                    "SceneEditor",
                    "--project",
                    "Sandbox/Sandbox.dproject",
                    "--kind",
                    "editor",
                    "--link",
                    "static",
                    "--pch",
                    "SharedPCH_Core",
                    "--private-dependency",
                    "Core",
                    "--public-dependency",
                    "Sandbox",
                    "--optional-private-dependency",
                    "DurinEd",
                    "--optional-public-dependency",
                    "AssetCore",
                    "--enable",
                    "DurinEditor",
                    "--enable",
                    "DurinGame",
                ]
            )
            build_scaffolding.execute_plan(
                build_scaffolding.plan_module_creation(request, root)
            )
            module_root = root / "Sandbox" / "Source" / "Editor" / "SceneEditor"
            descriptor = json.loads(
                (module_root / "SceneEditor.dmodule").read_text(encoding="utf-8")
            )
            project = json.loads(sandbox_project.read_text(encoding="utf-8"))
            self.assertEqual(descriptor["LinkType"], "Static")
            self.assertEqual(descriptor["PCH"], "SharedPCH_Core")
            self.assertFalse((module_root / "Private" / "PCH.SceneEditor.h").exists())
            self.assertEqual(descriptor["PrivateDependencies"], ["Core"])
            self.assertEqual(descriptor["PublicDependencies"], ["Sandbox"])
            self.assertEqual(descriptor["OptionalPrivateDependencies"], ["DurinEd"])
            self.assertEqual(descriptor["OptionalPublicDependencies"], ["AssetCore"])
            self.assertEqual(
                project["ExtraModules"]["DurinEditor"]["Modules"],
                ["SceneEditor"],
            )
            self.assertEqual(
                project["ExtraModules"]["DurinGame"]["Modules"],
                ["SceneEditor"],
            )
            self.assertNotIn("SceneEditor", project["BaseModules"])

    def test_custom_path_is_independent_from_kind_and_default_enablement(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _, sandbox_project = self.create_workspace(root)
            request = parse_build_request(
                [
                    "create",
                    "module",
                    "WorldTools",
                    "--project",
                    "Sandbox/Sandbox.dproject",
                    "--path",
                    "Source/Features/World Tools",
                    "--kind",
                    "editor",
                    "--private-dependency",
                    "Core",
                ]
            )
            build_scaffolding.execute_plan(
                build_scaffolding.plan_module_creation(request, root)
            )

            module_root = root / "Sandbox" / "Source" / "Features" / "World Tools"
            project = json.loads(sandbox_project.read_text(encoding="utf-8"))
            self.assertTrue((module_root / "WorldTools.dmodule").is_file())
            self.assertEqual(
                project["ModuleDirs"]["WorldTools"],
                "Source/Features/World Tools",
            )
            self.assertEqual(
                project["ExtraModules"]["DurinEditor"]["Modules"],
                ["WorldTools"],
            )
            self.assertNotIn("WorldTools", project["BaseModules"])

    def test_custom_path_rejects_outside_and_existing_module_overlap(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.create_workspace(root)
            outside_request = parse_build_request(
                [
                    "create",
                    "module",
                    "Outside",
                    "--project",
                    "Sandbox/Sandbox.dproject",
                    "--path",
                    str(root.parent / "Outside"),
                ]
            )
            with self.assertRaisesRegex(build_config.BuildToolError, "inside"):
                build_scaffolding.plan_module_creation(outside_request, root)

            inside_absolute_request = parse_build_request(
                [
                    "create",
                    "module",
                    "Absolute",
                    "--project",
                    "Sandbox/Sandbox.dproject",
                    "--path",
                    str(root / "Sandbox" / "Code" / "Absolute"),
                ]
            )
            inside_plan = build_scaffolding.plan_module_creation(
                inside_absolute_request,
                root,
            )
            self.assertTrue(
                any(
                    operation.path
                    == root / "Sandbox" / "Code" / "Absolute" / "Absolute.dmodule"
                    for operation in inside_plan.operations
                )
            )

            overlap_request = parse_build_request(
                [
                    "create",
                    "module",
                    "Nested",
                    "--project",
                    "Sandbox/Sandbox.dproject",
                    "--path",
                    "Source/Runtime/Sandbox/Nested",
                ]
            )
            with self.assertRaisesRegex(build_config.BuildToolError, "overlaps module"):
                build_scaffolding.plan_module_creation(overlap_request, root)

    def test_none_enablement_dry_run_and_conflicts_leave_workspace_unchanged(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.create_workspace(root)
            request = parse_build_request(
                [
                    "create",
                    "module",
                    "Utility",
                    "--project",
                    "Sandbox/Sandbox.dproject",
                    "--enable",
                    "none",
                    "--dry-run",
                    "--plain",
                ]
            )
            before = self.snapshot(root)
            stdout = io.StringIO()
            build_cli.execute_create_request(
                request,
                BuildOutput(plain=True, stdout=stdout, stderr=io.StringIO()),
                root=root,
            )
            self.assertEqual(self.snapshot(root), before)
            self.assertIn("Source/Runtime/Utility/Utility.dmodule", stdout.getvalue())

            conflict = root / "Sandbox" / "Source" / "Runtime" / "Utility"
            conflict.mkdir()
            before_conflict = self.snapshot(root)
            with self.assertRaisesRegex(build_config.BuildToolError, "already exists"):
                build_scaffolding.plan_module_creation(request, root)
            self.assertEqual(self.snapshot(root), before_conflict)


class ProjectScaffoldingTests(unittest.TestCase):
    @staticmethod
    def snapshot(root: Path) -> tuple[tuple[str, ...], dict[str, bytes]]:
        directories = tuple(
            sorted(
                path.relative_to(root).as_posix()
                for path in root.rglob("*")
                if path.is_dir()
            )
        )
        files = {
            path.relative_to(root).as_posix(): path.read_bytes()
            for path in root.rglob("*")
            if path.is_file()
        }
        return directories, files

    def test_project_creation_generates_registered_project_and_initial_module(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            ModuleScaffoldingTests.create_workspace(root)
            (root / "CMakeLists.txt").write_bytes(
                b"add_subdirectory(Engine)\r\nadd_subdirectory(Sandbox)"
            )
            request = parse_build_request(
                ["create", "project", "MyGame", "--path", "Games With Spaces"]
            )
            plan = build_scaffolding.plan_project_creation(request, root)
            build_scaffolding.execute_plan(plan)

            project_root = root / "Games With Spaces"
            descriptor = json.loads(
                (project_root / "MyGame.dproject").read_text(encoding="utf-8")
            )
            module = json.loads(
                (
                    project_root
                    / "Source"
                    / "Runtime"
                    / "MyGame"
                    / "MyGame.dmodule"
                ).read_text(encoding="utf-8")
            )
            root_cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
            self.assertEqual(descriptor["ModuleDirs"], {"MyGame": "Source/Runtime/MyGame"})
            self.assertEqual(descriptor["BaseModules"], ["MyGame"])
            self.assertEqual(module["PrivateDependencies"], ["Core"])
            self.assertTrue((project_root / "Configs").is_dir())
            self.assertTrue((project_root / "Content").is_dir())
            self.assertTrue((project_root / "CMake" / "MyGameSetup.cmake").is_file())
            self.assertEqual(root_cmake.count('add_subdirectory("Games With Spaces")'), 1)
            self.assertIn(
                'add_subdirectory(Sandbox)\nadd_subdirectory("Games With Spaces")\n',
                root_cmake.replace("\r\n", "\n"),
            )
            self.assertLess(
                root_cmake.index("add_subdirectory(Sandbox)"),
                root_cmake.index('add_subdirectory("Games With Spaces")'),
            )
            workspace = build_descriptors.load_workspace_descriptors(root)
            self.assertEqual(workspace.find_module("MyGame").owning_project, "MyGame")

    def test_project_dry_run_is_pure_and_direct_execution_reports_success(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            ModuleScaffoldingTests.create_workspace(root)
            request = parse_build_request(
                [
                    "create",
                    "project",
                    "MyGame",
                    "--path",
                    "MyGame",
                    "--dry-run",
                    "--plain",
                ]
            )
            before = self.snapshot(root)
            stdout = io.StringIO()
            build_cli.execute_create_request(
                request,
                BuildOutput(plain=True, stdout=stdout, stderr=io.StringIO()),
                root=root,
            )
            self.assertEqual(self.snapshot(root), before)
            self.assertIn("MyGame/MyGame.dproject", stdout.getvalue())
            self.assertIn("replace file: CMakeLists.txt", stdout.getvalue())

            create_request = parse_build_request(
                ["create", "project", "MyGame", "--path", "MyGame"]
            )
            stdout = io.StringIO()
            build_cli.execute_create_request(
                create_request,
                BuildOutput(plain=True, stdout=stdout, stderr=io.StringIO()),
                root=root,
            )
            self.assertIn('Created project "MyGame"', stdout.getvalue())

    def test_project_creation_rejects_names_paths_and_unsafe_registration(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            ModuleScaffoldingTests.create_workspace(root)
            invalid_requests = (
                (
                    ["create", "project", "Sandbox", "--path", "Another"],
                    "already exists",
                ),
                (
                    ["create", "project", "Core", "--path", "Another"],
                    "Initial module name",
                ),
                (
                    ["create", "project", "Nested", "--path", "Games/Nested"],
                    "direct child",
                ),
                (
                    ["create", "project", "InsideEngine", "--path", "Engine/Nested"],
                    "overlaps project",
                ),
                (
                    ["create", "project", "Unsafe", "--path", "Unsafe$Path"],
                    "safely",
                ),
            )
            for arguments, message in invalid_requests:
                with self.subTest(arguments=arguments):
                    before = self.snapshot(root)
                    with self.assertRaisesRegex(build_config.BuildToolError, message):
                        build_scaffolding.plan_project_creation(
                            parse_build_request(arguments),
                            root,
                        )
                    self.assertEqual(self.snapshot(root), before)

            outside = root.parent / "OutsideProject"
            with self.assertRaisesRegex(build_config.BuildToolError, "inside"):
                build_scaffolding.plan_project_creation(
                    parse_build_request(
                        ["create", "project", "Outside", "--path", str(outside)]
                    ),
                    root,
                )

            with (root / "CMakeLists.txt").open("a", encoding="utf-8") as stream:
                stream.write("add_subdirectory(Stale)\n")
            with self.assertRaisesRegex(build_config.BuildToolError, "already has"):
                build_scaffolding.plan_project_creation(
                    parse_build_request(
                        ["create", "project", "StaleProject", "--path", "Stale"]
                    ),
                    root,
                )

    def test_project_creation_failure_restores_root_and_removes_project_tree(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            ModuleScaffoldingTests.create_workspace(root)
            request = parse_build_request(
                ["create", "project", "MyGame", "--path", "MyGame"]
            )
            plan = build_scaffolding.plan_project_creation(request, root)
            before = self.snapshot(root)

            def fail_after_root_replacement(phase: str, index: int, path: Path) -> None:
                if phase == "after-replace" and path == (root / "CMakeLists.txt").resolve():
                    raise RuntimeError(f"injected project failure at {index}")

            with self.assertRaisesRegex(RuntimeError, "injected project failure"):
                build_scaffolding.execute_plan(
                    plan,
                    failure_injector=fail_after_root_replacement,
                )
            self.assertEqual(self.snapshot(root), before)


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
        self.assertTrue(output.compact)

    def test_rich_tty_output_contains_ansi_and_semantic_status(self) -> None:
        stdout = io.StringIO()
        with mock.patch.dict(os.environ, {}, clear=True):
            output = BuildOutput(stdout=stdout, stderr=io.StringIO(), force_terminal=True)
            output.success("done")
        self.assertIn("\x1b[", stdout.getvalue())
        self.assertIn("success", stdout.getvalue())
        self.assertFalse(output.compact)
        self.assertTrue(output.progress)

    def test_explicit_output_mode_overrides_terminal_detection(self) -> None:
        compact = BuildOutput(
            output_mode=build_config.OutputMode.COMPACT,
            stdout=io.StringIO(),
            stderr=io.StringIO(),
            force_terminal=True,
        )
        full = BuildOutput(
            output_mode=build_config.OutputMode.FULL,
            stdout=io.StringIO(),
            stderr=io.StringIO(),
        )
        self.assertTrue(compact.compact)
        self.assertFalse(full.compact)
        self.assertFalse(full.progress)

    def test_progress_mode_falls_back_to_compact_without_terminal(self) -> None:
        output = BuildOutput(
            output_mode=build_config.OutputMode.PROGRESS,
            stdout=io.StringIO(),
            stderr=io.StringIO(),
        )
        self.assertTrue(output.compact)
        self.assertFalse(output.progress)

    def test_progress_mode_replaces_ninja_status_and_streams_other_output(self) -> None:
        stdout = io.StringIO()
        output = BuildOutput(
            plain=True,
            output_mode=build_config.OutputMode.PROGRESS,
            stdout=stdout,
            stderr=io.StringIO(),
            force_terminal=True,
        )
        output.child_output("[1/2] Building first.cpp\n")
        output.child_output("[2/2] Linking result.dll\n")
        output.child_output("compiler diagnostic\n")
        text = stdout.getvalue()
        self.assertIn("\r[1/2] Building first.cpp", text)
        self.assertIn("\r[2/2] Linking result.dll", text)
        self.assertNotIn("[1/2] Building first.cpp\n", text)
        self.assertIn("[2/2] Linking result.dll\ncompiler diagnostic\n", text)

    def test_runtime_log_levels_are_colored_for_terminal_output(self) -> None:
        stdout = io.StringIO()
        with mock.patch.dict(os.environ, {}, clear=True):
            output = BuildOutput(
                stdout=stdout,
                stderr=io.StringIO(),
                force_terminal=True,
                output_mode=build_config.OutputMode.FULL,
            )
            output.child_output(
                "[12:34:56][warning]Runtime warning\n",
                colorize_log_levels=True,
            )
        text = stdout.getvalue()
        self.assertIn("\x1b[", text)
        self.assertIn("warning", text)
        self.assertIn("Runtime warning", text)

    def test_runtime_log_level_coloring_respects_plain_output(self) -> None:
        stdout = io.StringIO()
        output = BuildOutput(
            plain=True,
            stdout=stdout,
            stderr=io.StringIO(),
            force_terminal=True,
            output_mode=build_config.OutputMode.FULL,
        )
        output.child_output(
            "[12:34:56][error]Runtime error\n",
            colorize_log_levels=True,
        )
        self.assertNotIn("\x1b[", stdout.getvalue())

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
    def test_buildtool_rejects_missing_windows_long_paths_policy(self) -> None:
        fake_winreg = mock.MagicMock(HKEY_LOCAL_MACHINE=object(), REG_DWORD=4)
        fake_winreg.OpenKey.return_value.__enter__.return_value = object()
        fake_winreg.QueryValueEx.return_value = (0, fake_winreg.REG_DWORD)
        with mock.patch.object(build_core.os, "name", "nt"), mock.patch.dict(
            os.sys.modules, {"winreg": fake_winreg}
        ), self.assertRaisesRegex(build_config.BuildToolError, "LongPathsEnabled"):
            build_core.require_windows_long_paths_enabled()

    def test_buildtool_accepts_enabled_windows_long_paths_policy(self) -> None:
        fake_winreg = mock.MagicMock(HKEY_LOCAL_MACHINE=object(), REG_DWORD=4)
        fake_winreg.OpenKey.return_value.__enter__.return_value = object()
        fake_winreg.QueryValueEx.return_value = (1, fake_winreg.REG_DWORD)
        with mock.patch.object(build_core.os, "name", "nt"), mock.patch.dict(
            os.sys.modules, {"winreg": fake_winreg}
        ):
            build_core.require_windows_long_paths_enabled()

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

    def make_preset(
        self,
        name: str = "debug",
        testing: str = "ON",
        runtime_variant: str = "DurinEditor",
    ) -> build_config.ConfigurePreset:
        return build_config.ConfigurePreset(
            name,
            {
                "name": name,
                "binaryDir": "${sourceDir}/Build/${presetName}",
                "cacheVariables": {
                    "CMAKE_BUILD_TYPE": "Debug",
                    "DURIN_RUNTIME_VARIANT": runtime_variant,
                    "BUILD_TESTING": testing,
                },
            },
        )

    def test_run_project_is_normalized_and_validated_without_toolchain_state(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            descriptor = root / "Games" / "示例 Project" / "Example.dproject"
            descriptor.parent.mkdir(parents=True)
            descriptor.write_text("{}", encoding="utf-8")
            request = build_config.CommandRequest(
                build_config.Action.RUN,
                project_path=Path("Games") / "示例 Project" / "Example.dproject",
                run_arguments=("--hidden-window", "argument with spaces"),
            )
            normalized = build_core.normalize_run_request(request, root=root)
        self.assertEqual(normalized.project_path, descriptor.resolve())
        self.assertEqual(normalized.run_arguments, request.run_arguments)

    def test_run_defaults_durin_game_to_sandbox_project(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            descriptor = root / "Sandbox" / "Sandbox.dproject"
            descriptor.parent.mkdir(parents=True)
            descriptor.write_text("{}", encoding="utf-8")
            request = build_config.CommandRequest(build_config.Action.RUN)
            normalized = build_core.normalize_run_request(
                request,
                preset=self.make_preset(runtime_variant="DurinGame"),
                root=root,
            )
        self.assertEqual(normalized.project_path, descriptor.resolve())

    def test_run_does_not_default_editor_or_override_raw_project_selector(self) -> None:
        request = build_config.CommandRequest(build_config.Action.RUN)
        editor_request = build_core.normalize_run_request(
            request,
            preset=self.make_preset(runtime_variant="DurinEditor"),
        )
        raw_game_request = build_config.CommandRequest(
            build_config.Action.RUN,
            run_arguments=("--project=Other.dproject",),
        )
        normalized_raw_game_request = build_core.normalize_run_request(
            raw_game_request,
            preset=self.make_preset(runtime_variant="DurinGame"),
        )
        self.assertIsNone(editor_request.project_path)
        self.assertIsNone(normalized_raw_game_request.project_path)
        self.assertEqual(
            normalized_raw_game_request.run_arguments,
            raw_game_request.run_arguments,
        )

    def test_run_project_rejects_missing_and_wrong_extension_descriptors(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            wrong_extension = build_config.CommandRequest(
                build_config.Action.RUN,
                project_path=Path("Example.json"),
            )
            with self.assertRaisesRegex(build_config.BuildToolError, r"\.dproject extension"):
                build_core.normalize_run_request(wrong_extension, root=root)

            missing = build_config.CommandRequest(
                build_config.Action.RUN,
                project_path=Path("Missing.dproject"),
            )
            with self.assertRaisesRegex(build_config.BuildToolError, "was not found"):
                build_core.normalize_run_request(missing, root=root)

    def test_run_project_rejects_conflicting_runtime_project_selectors(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            descriptor = Path(directory) / "Example.dproject"
            descriptor.write_text("{}", encoding="utf-8")
            for arguments in (
                ("--project", "Other.dproject"),
                ("--project=Other.dproject",),
            ):
                with self.subTest(arguments=arguments), self.assertRaisesRegex(
                    build_config.BuildToolError,
                    "either through --project or through --args",
                ):
                    build_core.normalize_run_request(
                        build_config.CommandRequest(
                            build_config.Action.RUN,
                            project_path=descriptor,
                            run_arguments=arguments,
                        )
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

    def test_runtime_path_uses_runtime_variant_and_preset_role(self) -> None:
        preset = self.make_preset()
        values = dict(preset.values)
        cache = dict(values["cacheVariables"])
        cache["CMAKE_BUILD_TYPE"] = "Release"
        cache["DURIN_PRESET_ROLE"] = "Profiling"
        preset = build_config.ConfigurePreset("profiling", {**values, "cacheVariables": cache})
        path = build_core.runtime_executable_path(self.make_profile(), preset, root=Path("repo"))
        self.assertEqual(
            path,
            Path("repo/Engine/Binaries/Win64/Release-Profiling/Runtime/DurinEditor/DurinEditor.exe"),
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
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            project = root / "示例 Project" / "Example.dproject"
            project.parent.mkdir()
            project.touch()
            context = build_config.BuildContext(
                build_config.CommandRequest(
                    build_config.Action.RUN,
                    project_path=project.resolve(),
                    run_arguments=("--hidden-window", "argument with spaces"),
                ),
                build_config.LocalConfig(),
                self.make_profile(),
                {"debug": preset},
                preset,
                "windows",
            )
            executable = root / "DurinEditor.exe"
            executable.touch()
            with mock.patch.object(
                build_core,
                "runtime_executable_path",
                return_value=executable,
            ), mock.patch.object(build_core, "run_command") as run:
                build_core.run_application(context, output)
        self.assertEqual(
            run.call_args.args[0],
            [
                str(executable),
                f"--project={project.resolve()}",
                "--hidden-window",
                "argument with spaces",
            ],
        )
        self.assertTrue(run.call_args.kwargs["wait_for_descendants"])
        self.assertFalse(run.call_args.kwargs["show_heartbeat"])
        self.assertTrue(run.call_args.kwargs["colorize_log_levels"])

    def test_run_command_waits_for_windows_process_job(self) -> None:
        process = mock.Mock(pid=42, returncode=0)
        process.stdout = io.StringIO()
        process.wait.return_value = 0
        process_job = mock.Mock()
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(
            build_core, "command_log_path", return_value=Path(directory) / "command.log"
        ), mock.patch.object(build_core.subprocess, "Popen", return_value=process), mock.patch.object(
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
        process.stdout = io.StringIO()
        process.wait.return_value = 0
        process.poll.return_value = 0
        process_job = mock.Mock()
        process_job.wait.side_effect = KeyboardInterrupt
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(
            build_core, "command_log_path", return_value=Path(directory) / "command.log"
        ), mock.patch.object(build_core.subprocess, "Popen", return_value=process), mock.patch.object(
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

    def test_compact_native_test_enables_gtest_brief_output(self) -> None:
        preset = self.make_preset()
        context = build_config.BuildContext(
            build_config.CommandRequest(
                build_config.Action.TEST,
                target="CoreTests",
                test_filter="Core.*",
            ),
            build_config.LocalConfig(),
            self.make_profile(),
            {"debug": preset},
            preset,
            "windows",
            environment={},
        )
        output = BuildOutput(
            plain=True,
            output_mode=build_config.OutputMode.COMPACT,
            stdout=io.StringIO(),
            stderr=io.StringIO(),
        )
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(
            build_core,
            "test_executable_path",
            return_value=Path(directory) / "CoreTests.exe",
        ) as executable_path, mock.patch.object(build_core, "run_command") as run:
            executable_path.return_value.touch()
            build_core.run_native_test(context, output)
        self.assertEqual(
            run.call_args.args[0],
            [str(executable_path.return_value), "--gtest_filter=Core.*", "--gtest_brief=1"],
        )

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
                    metadata={"pid": 1, "action": "build", "target": "Core"},
                    operation=interrupt,
                )
            with self.assertRaisesRegex(
                build_config.BuildToolError,
                "did not return normally",
            ) as blocked:
                build_core.execute_with_recovery_marker(
                    action=build_config.Action.BUILD,
                    marker_file=marker,
                    metadata={"pid": 1, "action": "build", "target": "Core"},
                    operation=lambda: None,
                )
            self.assertIn("run recover", blocked.exception.recovery)
            build_core.execute_with_recovery_marker(
                action=build_config.Action.REBUILD,
                marker_file=marker,
                metadata={"pid": 1, "action": "rebuild", "target": "Core"},
                operation=lambda: None,
            )
            self.assertFalse(marker.exists())

    def test_rebuild_rejects_an_unrelated_recovery_target(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / "interrupted.json"
            marker.write_text(
                json.dumps({"pid": 1, "action": "build", "target": "Core"}),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(build_config.BuildToolError, 'Interrupted target "Core"'):
                build_core.execute_with_recovery_marker(
                    action=build_config.Action.REBUILD,
                    marker_file=marker,
                    metadata={"pid": 2, "action": "rebuild", "target": "Editor"},
                    operation=lambda: None,
                )
            self.assertEqual(build_core.recovery_target(marker), "Core")

    def test_invalid_or_non_target_recovery_state_falls_back_to_all(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / "interrupted.json"
            marker.write_text(json.dumps({"action": "unknown", "target": "Core"}), encoding="utf-8")
            self.assertIsNone(build_core.recoverable_target(marker))
            self.assertEqual(build_core.recovery_target(marker), "all")
            marker.write_text("{invalid", encoding="utf-8")
            self.assertIsNone(build_core.recoverable_target(marker))
            self.assertEqual(build_core.recovery_target(marker), "all")

    def test_recover_clears_a_valid_interruption_marker(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / "interrupted.json"
            marker.write_text(
                json.dumps({"pid": 1, "action": "build", "target": "Core"}),
                encoding="utf-8",
            )
            build_core.execute_with_recovery_marker(
                action=build_config.Action.RECOVER,
                marker_file=marker,
                metadata={"pid": 2, "action": "recover", "target": "Core"},
                operation=lambda: None,
            )
            self.assertFalse(marker.exists())

    def test_recover_builds_incrementally_without_cleaning(self) -> None:
        preset = self.make_preset()
        context = build_config.BuildContext(
            build_config.CommandRequest(build_config.Action.RECOVER),
            build_config.LocalConfig(),
            self.make_profile(),
            {"debug": preset},
            preset,
            "windows",
            cmake="cmake",
            jobs=4,
            environment={"PATH": "cached"},
        )
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        with tempfile.TemporaryDirectory() as directory:
            build_directory = Path(directory)
            with mock.patch.object(
                build_core,
                "preset_build_directory",
                return_value=build_directory,
            ), mock.patch.object(
                build_core,
                "cache_is_usable",
                return_value=True,
            ), mock.patch.object(
                build_core,
                "ninja_uses_english_msvc_prefix",
                return_value=True,
            ), mock.patch.object(build_core, "run_command") as run:
                build_core.perform_action(context, output, target_override="Core")
        run.assert_called_once_with(
            ["cmake", "--build", str(build_directory), "--target", "Core", "-j", "4"],
            environment={"PATH": "cached"},
            output=output,
        )

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
        process.stdout = io.StringIO()
        process.wait.side_effect = KeyboardInterrupt
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(
            build_core, "command_log_path", return_value=Path(directory) / "command.log"
        ), mock.patch.object(build_core.subprocess, "Popen", return_value=process), mock.patch.object(
            build_core,
            "terminate_process_tree",
        ) as terminate:
            with self.assertRaises(build_config.BuildToolInterruptedError):
                build_core.run_command(["cmake", "--version"], environment={}, output=output)
        terminate.assert_called_once_with(process)

    def test_run_command_does_not_inherit_buildtool_handles(self) -> None:
        process = mock.Mock()
        process.stdout = io.StringIO()
        process.wait.return_value = 0
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(
            build_core, "command_log_path", return_value=Path(directory) / "command.log"
        ), mock.patch.object(build_core.subprocess, "Popen", return_value=process) as popen:
            build_core.run_command(["cmake", "--version"], environment={}, output=output)
        self.assertTrue(popen.call_args.kwargs["close_fds"])
        self.assertIs(popen.call_args.kwargs["stdout"], build_core.subprocess.PIPE)

    def test_command_timeout_terminates_child_process_tree(self) -> None:
        process = mock.Mock()
        process.stdout = io.StringIO("compiler.cpp(7): error C1234: broken\n")
        process.wait.side_effect = build_core.subprocess.TimeoutExpired(["CoreTests"], 0)
        output = BuildOutput(plain=True, stdout=io.StringIO(), stderr=io.StringIO())
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(
            build_core, "command_log_path", return_value=Path(directory) / "command.log"
        ), mock.patch.object(build_core.subprocess, "Popen", return_value=process), mock.patch.object(
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

    def test_compact_command_output_is_logged_and_failure_is_summarized(self) -> None:
        stdout = io.StringIO()
        output = BuildOutput(
            plain=True,
            output_mode=build_config.OutputMode.COMPACT,
            stdout=stdout,
            stderr=io.StringIO(),
        )
        with tempfile.TemporaryDirectory() as directory:
            log_path = Path(directory) / "command.log"
            with mock.patch.object(build_core, "command_log_path", return_value=log_path):
                with self.assertRaises(build_config.BuildToolError) as raised:
                    build_core.run_command(
                        [
                            os.sys.executable,
                            "-c",
                            "print('noise'); print('source.cpp(9): error C1000: failed'); raise SystemExit(1)",
                        ],
                        environment=os.environ,
                        output=output,
                    )
            self.assertIn("noise", log_path.read_text(encoding="utf-8"))
        self.assertNotIn("\nnoise\n", stdout.getvalue())
        self.assertIn("error C1000", raised.exception.output_excerpt)
        self.assertEqual(raised.exception.log_path, log_path)

    def test_full_command_output_streams_and_is_logged(self) -> None:
        stdout = io.StringIO()
        output = BuildOutput(
            plain=True,
            output_mode=build_config.OutputMode.FULL,
            stdout=stdout,
            stderr=io.StringIO(),
        )
        with tempfile.TemporaryDirectory() as directory:
            log_path = Path(directory) / "command.log"
            with mock.patch.object(build_core, "command_log_path", return_value=log_path):
                build_core.run_command(
                    [os.sys.executable, "-c", "print('visible child output')"],
                    environment=os.environ,
                    output=output,
                )
            self.assertIn("visible child output", log_path.read_text(encoding="utf-8"))
        self.assertIn("visible child output", stdout.getvalue())

    def test_compact_command_output_preserves_gtest_summary(self) -> None:
        stdout = io.StringIO()
        output = BuildOutput(
            plain=True,
            output_mode=build_config.OutputMode.COMPACT,
            stdout=stdout,
            stderr=io.StringIO(),
        )
        child_script = (
            "print('[==========] 122 tests from 25 test suites ran. (100 ms total)'); "
            "print('[  PASSED  ] 122 tests.')"
        )
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(
            build_core,
            "command_log_path",
            return_value=Path(directory) / "command.log",
        ):
            build_core.run_command(
                [os.sys.executable, "-c", child_script],
                environment=os.environ,
                output=output,
            )
        self.assertIn("122 tests from 25 test suites ran", stdout.getvalue())
        self.assertIn("[  PASSED  ] 122 tests.", stdout.getvalue())

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
            self.assertIn(root / "Engine/Binaries/Win64/ThirdParty/Debug", paths)
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
