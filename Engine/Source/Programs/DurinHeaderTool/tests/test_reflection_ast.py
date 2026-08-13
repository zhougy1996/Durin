import json
import re
import sys
import traceback
from pathlib import Path
from unittest import mock

import clang.cindex
import pytest

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from durin_header_tool import config as configs
from durin_header_tool import io as utils
from durin_header_tool.config.module_config import DurinModuleConfig
from durin_header_tool.extractors.export_symbol_extractor import (
    _extract_header_export_symbols_impl,
    extract_module_export_info,
    resolve_module_export_info,
)
from durin_header_tool.generators import module_reflection_files_generator as reflection_generator
from durin_header_tool.generators.module_reflection_files_generator import (
    _write_reflection_files,
    make_new_reflection_state,
)
from durin_header_tool.model.export_info import (
    ExportedSymbolInfo,
    load_module_export_file,
    save_module_export_file,
)
from durin_header_tool.cache.phase_state import ReflectionPhaseState, save_reflection_phase_state
from durin_header_tool.model.reflection_info import (
    ReflectedEnumInfo,
    ReflectedEnumValueInfo,
    make_generated_enum_helper_name,
    make_generated_helper_name,
)
from durin_header_tool.parser.reflection_parser import (
    _make_property_from_spelling,
    _make_property_from_type,
    _parse_translation_unit,
    _scan_generated_body_line,
    _validate_explicit_container_spelling,
    _validate_soft_object_spelling,
    make_dht_parse_source,
    parse_reflection_header,
)
from durin_header_tool.parser import property_parser, reflection_parser
from durin_header_tool.resolver.reflection_resolver import (
    load_available_symbols,
    resolve_header_symbols,
    resolve_symbol,
    resolved_symbol_dependencies_for_header,
)
from durin_header_tool.resolver import reflection_resolver
from durin_header_tool.writers.reflection_source_writer import (
    _enum_definitions,
    generate_cpp_content,
    generate_header_content,
)


from reflection_test_support import reflection_fixture


@pytest.mark.usefixtures("reflection_fixture")
class TestReflectionAstAndState:
    def test_export_schema_uses_qualified_symbol_identity(self):
        export_path = self.temp_root / "Fixture.export"
        with mock.patch.object(utils, "get_module_export_file_path", return_value=export_path):
            content = save_module_export_file(self.export_info)
        data = json.loads(content)

        assert data["SchemaVersion"] == 5
        actor = data["Symbols"]["Fixture::ASampleActor"]
        assert actor["QualifiedName"] == "Fixture::ASampleActor"
        assert actor["GeneratedHelperName"] == "Z_Construct_DClass_Fixture_ASampleActor"
        assert actor["BaseQualifiedName"] == "Durin::DObject"
        assert not actor["IsAbstract"]
        assert data["Symbols"]["Fixture::AAbstractActor"]["IsAbstract"]

        loaded = load_module_export_file(export_path)
        assert loaded.Symbols["Fixture::AAbstractActor"].IsAbstract


    def test_parallel_worker_failure_propagates_without_sequential_retry(self):
        successful_future = mock.Mock()
        successful_future.result.return_value = {"header": "First.h"}
        failed_future = mock.Mock()
        failure = RuntimeError("worker failed")

        def raise_worker_failure():
            raise failure

        failed_future.result.side_effect = raise_worker_failure
        executor = mock.MagicMock()
        executor.__enter__.return_value = executor
        executor.submit.side_effect = [successful_future, failed_future]
        worker = mock.Mock()
        manifest = ReflectionPhaseState(module="Fixture")

        with (
            mock.patch.object(utils, "get_module_dht_output_dir", return_value=self.dht_output_dir),
            mock.patch.object(reflection_generator, "resolve_worker_count", return_value=2),
            mock.patch.object(reflection_generator, "ProcessPoolExecutor", return_value=executor),
            mock.patch.object(reflection_generator, "as_completed", return_value=[successful_future, failed_future]),
            mock.patch.object(reflection_generator, "_generate_reflection_output_worker", worker),
            pytest.raises(RuntimeError, match="worker failed") as raised,
        ):
            _write_reflection_files(
                "Fixture",
                ["First.h", "Second.h"],
                {},
                manifest,
                max_workers=2,
            )

        assert raised.value is failure
        assert "raise_worker_failure" in {
            frame.name for frame in traceback.extract_tb(raised.value.__traceback__)
        }
        worker.assert_not_called()


    def test_generated_body_line_comes_from_source_instead_of_synthetic_cursor(self):
        source = '''namespace Fixture
{
    class FItem
    {
        GENERATED_BODY()
    };
}
'''
        class_cursor = mock.Mock()
        class_cursor.extent.start.line = 3
        class_cursor.extent.start.column = 5
        class_cursor.extent.end.line = 3
        synthetic_member = mock.Mock()
        synthetic_member.spelling = "DHT_GENERATED_BODY"
        synthetic_member.location.line = 3
        class_cursor.get_children.return_value = [synthetic_member]

        assert _scan_generated_body_line(source, class_cursor) == 5


    def test_available_symbols_load_current_module_export_once(self):
        dependency_export = self.temp_root / "Dependency.export"
        current_export = self.temp_root / "Fixture.export"
        dependency_export.touch()
        current_export.touch()
        exports = {
            dependency_export: mock.Mock(Symbols={"Dependency::Type": object()}),
            current_export: mock.Mock(Symbols={"Fixture::Type": object()}),
        }
        with (
            mock.patch.object(
                configs,
                "collect_all_dependent_module_with_export_file",
                return_value=["Dependency", "Fixture"],
            ),
            mock.patch.object(configs, "collect_all_dependent_modules", return_value={"Dependency"}),
            mock.patch.object(configs, "get_module_config", return_value=self.module_config),
            mock.patch.object(
                utils,
                "get_module_export_file_path",
                side_effect=lambda module_name: {
                    "Dependency": dependency_export,
                    "Fixture": current_export,
                }[module_name],
            ),
            mock.patch.object(
                reflection_resolver,
                "load_module_export_file",
                side_effect=exports.__getitem__,
            ) as load_export,
        ):
            symbols = load_available_symbols("Fixture")

        assert load_export.call_args_list == [
            mock.call(dependency_export),
            mock.call(current_export),
        ]
        assert "Dependency::Type" in symbols
        assert "Fixture::Type" in symbols


    def test_phase_state_records_generator_contract(self):
        state_root = self.temp_root / "DHTCache"
        with (
            mock.patch.object(configs, "get_module_config", return_value=self.module_config),
            mock.patch.object(configs, "collect_all_dependent_module_with_export_file", return_value=[]),
            mock.patch.object(configs, "ARCH", "Win64"),
            mock.patch.object(configs, "RUNTIME_VARIANT", "DurinEditor"),
            mock.patch.object(configs, "TOOL_FINGERPRINT", "fixture-fingerprint"),
            mock.patch.object(utils, "get_module_dht_cache_root", return_value=state_root),
        ):
            state = make_new_reflection_state("Fixture")
            content = save_reflection_phase_state(state)
        data = json.loads(content)["Payload"]

        assert data["SchemaVersion"] == 1
        assert data["ToolFingerprint"] == "fixture-fingerprint"
        assert data["Module"] == "Fixture"
        assert data["RuntimeVariant"] == "DurinEditor"
        assert "Profile" not in data
        assert data["Platform"] == "Win64"
        assert data["GeneratedOutputs"] == [
            "Fixture.module.gen.cpp",
            "FixtureTypes.gen.cpp",
            "FixtureTypes.gen.h",
        ]
        assert data["PendingCleanupOutputs"] == []
        assert data["GeneratedOutputDigests"] == {}

