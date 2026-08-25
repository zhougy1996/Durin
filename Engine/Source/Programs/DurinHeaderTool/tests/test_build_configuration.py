import argparse
import json
import logging
import sys
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

import pytest

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from durin_header_tool import config as configs
from durin_header_tool import io as utils
from durin_header_tool.cli.command import (
    add_common_arguments,
    generate_module_export_file,
    generate_reflection_files,
    prepare_project_build,
    setup_parser,
)
from durin_header_tool.cli.main import _get_output_lock_paths, init_logging
from durin_header_tool.generators import module_cmake_file_generator
from durin_header_tool.generators import project_cmake_file_generator
from durin_header_tool.runtime.parallelism import resolve_worker_count
from durin_header_tool.runtime.worker_context import initialize_worker_config


@pytest.fixture(scope="module")
def initialized_configs():
    previous_context = (
        configs.ARCH,
        configs.RUNTIME_VARIANT,
        configs.TOOL_FINGERPRINT,
    )
    configs.init_configs()
    yield
    (
        configs.ARCH,
        configs.RUNTIME_VARIANT,
        configs.TOOL_FINGERPRINT,
    ) = previous_context


@pytest.mark.usefixtures("initialized_configs")
class TestModuleDependency:
    def test_config_files_initialize_dataclasses_once(
        self,
        tmp_path_factory: pytest.TempPathFactory,
    ):
        root = Path(tmp_path_factory.mktemp("case"))
        module_path = root / "Fixture.dmodule"
        module_path.write_text('{"ModuleName": "Fixture"}', encoding="utf-8")
        project_path = root / "Fixture.dproject"
        project_path.write_text(
            '{"ProjectName": "Fixture", "ModuleDirs": {"Fixture": "Source/Fixture"}}',
            encoding="utf-8",
        )

        module_post_init = configs.module_config.DurinModuleConfig.__post_init__
        project_post_init = configs.project_config.DurinProjectConfig.__post_init__
        with (
            mock.patch.object(
                configs.module_config.DurinModuleConfig,
                "__post_init__",
                autospec=True,
                side_effect=module_post_init,
            ) as initialize_module,
            mock.patch.object(
                configs.project_config.DurinProjectConfig,
                "__post_init__",
                autospec=True,
                side_effect=project_post_init,
            ) as initialize_project,
        ):
            module_config = configs.module_config.DurinModuleConfig.from_file(module_path)
            project_config = configs.project_config.DurinProjectConfig.from_file(project_path)

        initialize_module.assert_called_once_with(module_config)
        initialize_project.assert_called_once_with(project_config)

    def test_config_files_use_explicit_fields_and_preserve_defaults(self, tmp_path: Path):
        module_path = tmp_path / "Fixture.dmodule"
        module_path.write_text(
            json.dumps(
                {
                    "ModuleName": "Fixture",
                    "LinkType": "Static",
                    "PCH": "SharedPCH_Core",
                    "PrivateDependencies": ["Core"],
                    "PublicDependencies": ["AssetCore"],
                    "OptionalPrivateDependencies": ["DurinEd"],
                    "OptionalPublicDependencies": ["MonaImGui"],
                    "ReflectHeaders": ["Public/Fixture.h"],
                }
            ),
            encoding="utf-8",
        )
        project_path = tmp_path / "Fixture.dproject"
        project_path.write_text(
            json.dumps(
                {
                    "ProjectName": "Fixture",
                    "ModuleDirs": {"Fixture": "Source/Fixture"},
                    "ExtraModules": {"DurinEditor": {"Modules": ["Fixture"]}},
                    "Mounts": [
                        {
                            "VirtualRoot": "/Plugins/Fixture/",
                            "Owner": "Extension",
                            "Root": "Plugins/Fixture",
                            "ContentPath": "Content",
                            "AutoScan": True,
                            "AuthoringWritable": False,
                            "Dependencies": ["/Engine/"],
                        }
                    ],
                }
            ),
            encoding="utf-8",
        )

        module_config = configs.module_config.DurinModuleConfig.from_file(module_path)
        project_config = configs.project_config.DurinProjectConfig.from_file(project_path)

        assert module_config.module_name == "Fixture"
        assert isinstance(module_config.module_name, str)
        assert module_config.link_type == "Static"
        assert module_config.pch == "SharedPCH_Core"
        assert module_config.private_dependencies == ["Core"]
        assert module_config.public_dependencies == ["AssetCore"]
        assert module_config.optional_private_dependencies == ["DurinEd"]
        assert module_config.optional_public_dependencies == ["MonaImGui"]
        assert module_config.reflect_headers == ["Public/Fixture.h"]
        assert module_config.api_macro == "FIXTURE_API"
        assert module_config.config_file_path == module_path.resolve()
        assert module_config.module_dir == tmp_path.resolve()

        assert project_config.project_name == "Fixture"
        assert isinstance(project_config.project_name, str)
        assert project_config.module_dirs == {"Fixture": "Source/Fixture"}
        assert project_config.modules == {"Fixture": "Source/Fixture/Fixture.dmodule"}
        assert project_config.base_modules == ["Fixture"]
        assert project_config.extra_modules["DurinEditor"].modules == ["Fixture"]
        assert project_config.config_file_path == project_path.resolve()
        assert project_config.project_dir == tmp_path.resolve()
        assert not hasattr(project_config, "mounts")

    @pytest.mark.parametrize(
        ("file_name", "contents", "expected"),
        [
            (
                "Invalid.dmodule",
                {"ModuleName": "Invalid", "ConfigFilePath": "injected"},
                "Additional properties are not allowed",
            ),
            (
                "Invalid.dproject",
                {"ProjectName": "Invalid", "BaseModules": [False]},
                "is not of type 'string'",
            ),
        ],
    )
    def test_explicit_config_parsers_reject_invalid_fields(
        self,
        tmp_path: Path,
        file_name: str,
        contents: dict[str, object],
        expected: str,
    ):
        path = tmp_path / file_name
        path.write_text(json.dumps(contents), encoding="utf-8")
        parser = (
            configs.module_config.DurinModuleConfig.from_file
            if path.suffix == ".dmodule"
            else configs.project_config.DurinProjectConfig.from_file
        )
        with pytest.raises(ValueError, match=expected):
            parser(path)

    def test_generic_dataclass_json_parser_is_not_public(self):
        assert not hasattr(utils, "dataclass_from_dict")

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


@pytest.mark.usefixtures("initialized_configs")
class TestIntermediateLayout:
    @pytest.fixture(autouse=True)
    def isolated_build_context(self):
        previous_context = (
            configs.ARCH,
            configs.RUNTIME_VARIANT,
            configs.TOOL_FINGERPRINT,
        )
        configs.ARCH = "Win64"
        configs.RUNTIME_VARIANT = "DurinEditor"
        configs.TOOL_FINGERPRINT = ""
        yield
        (
            configs.ARCH,
            configs.RUNTIME_VARIANT,
            configs.TOOL_FINGERPRINT,
        ) = previous_context

    def test_intermediate_path_uses_platform_and_runtime_variant(self):
        assert utils.get_project_intermediate_build_dir("Engine") == (
            utils.get_project_intermediate_dir("Engine") / "Build" / "Win64" / "DurinEditor"
        )

    def test_persistent_phase_state_is_local_to_the_module(self):
        assert utils.get_module_dht_state_dir("Core") == (
            utils.get_module_intermediate_build_dir("Core") / "DHTState"
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

    def test_module_locks_are_independent_within_a_runtime_variant(self):
        assert utils.get_dht_module_lock_file_path("Core") != utils.get_dht_module_lock_file_path(
            "Engine"
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
        assert utils.get_project_intermediate_build_dir("Engine") == (
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

    def test_cli_quiet_mode_overrides_info_log_level(self):
        parser = argparse.ArgumentParser()
        add_common_arguments(parser)
        args = parser.parse_args(["--log", "DEBUG", "--quiet"])
        assert args.quiet
        with mock.patch("durin_header_tool.cli.main.logging.basicConfig") as configure:
            init_logging(args.log, quiet=args.quiet)
        configure.assert_called_once_with(
            level=logging.WARNING,
            format="[%(levelname)s] %(message)s",
        )

    @pytest.mark.parametrize(
        ("arguments", "expected_function"),
        [
            (["prepare_project_build", "--project", "Engine.dproject"], prepare_project_build),
            (["generate_module_export_file", "--module", "Core"], generate_module_export_file),
            (["generate_reflection_files", "--module", "Core"], generate_reflection_files),
        ],
    )
    def test_cli_dispatches_active_commands_directly(self, arguments, expected_function):
        parser = argparse.ArgumentParser()
        setup_parser(parser)

        assert parser.parse_args(arguments).execute is expected_function

    @pytest.mark.parametrize(
        ("task_count", "worker_limit", "expected"),
        [
            (7, 8, 1),
            (8, 2, 2),
            (15, 8, 2),
            (16, 8, 4),
            (31, 8, 4),
            (32, 8, 8),
            (32, 4, 4),
        ],
    )
    def test_worker_parallelism_requires_a_large_task_set(
        self,
        task_count,
        worker_limit,
        expected,
    ):
        assert resolve_worker_count(task_count, worker_limit) == expected

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
            "${DURIN_PROJECT_BINARY_DIR}/${DURIN_ARCH}/${DURIN_THIRDPARTY_OUTPUT_CONFIG}/ThirdParty"
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
        assert "module_definitions_header" not in content
        assert "module_export_dependencies" in content
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
        assert project_targets.count("${module_export_dependencies}") == 1
        assert "JOB_POOLS durin_dht=${DURIN_DHT_JOB_POOL_SIZE}" in build_options
        assert project_targets.count("JOB_POOL durin_dht") == 2
        assert project_targets.count("--workers ${DURIN_DHT_WORKERS}") == 2
        assert "set(DURIN_DHT_LOG_LEVEL INFO CACHE STRING" in build_options
        assert project_targets.count("--log ${DURIN_DHT_LOG_LEVEL}") == 2
        assert "option(DURIN_ENABLE_UNITY_BUILD" in build_options
        assert "set(DURIN_UNITY_BUILD_BATCH_SIZE 8 CACHE STRING" in build_options
        assert "UNITY_BUILD_BATCH_SIZE ${DURIN_UNITY_BUILD_BATCH_SIZE}" in project_targets
        assert "SKIP_UNITY_BUILD_INCLUSION TRUE" in project_targets
