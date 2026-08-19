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


class TestReflectionSourceWriter:
    def test_enum_values_use_unsigned_64_bit_channel(self):
        enum_info = ReflectedEnumInfo(
            short_name="EValue",
            namespace="Durin",
            qualified_name="Durin::EValue",
            generated_helper_name="Z_Construct_DEnum_Durin_EValue",
            header="Value.h",
            api="CORE_API",
            values=[
                ReflectedEnumValueInfo(name="Negative", value=-1),
                ReflectedEnumValueInfo(name="High", value=(1 << 64) - 1),
            ],
        )

        content = _enum_definitions(enum_info, "/Cpp/Core")

        assert '{ "Negative", static_cast<Durin::uint64>(-1), nullptr },' in content
        assert '{ "High", static_cast<Durin::uint64>(18446744073709551615), nullptr },' in content

    def test_enum_display_metadata_is_escaped_and_transported(self):
        enum_info = ReflectedEnumInfo(
            short_name="EMode",
            namespace="Durin",
            qualified_name="Durin::EMode",
            generated_helper_name="Z_Construct_DEnum_Durin_EMode",
            header="Mode.h",
            api="CORE_API",
            display_name='Editor "Mode"',
            values=[
                ReflectedEnumValueInfo(name="Path", value=1, display_name=r"C:\Mode"),
                ReflectedEnumValueInfo(name="DefaultValue", value=2),
            ],
        )

        content = _enum_definitions(enum_info, "/Cpp/Core")

        assert r'"Editor \"Mode\"",' in content
        assert r'{ "Path", static_cast<Durin::uint64>(1), "C:\\Mode" },' in content
        assert '{ "DefaultValue", static_cast<Durin::uint64>(2), nullptr },' in content


@pytest.mark.usefixtures("reflection_fixture")
class TestReflectionWriterIntegration:
    def test_abstract_class_emits_flag_without_object_constructor(self):
        abstract_class = next(
            class_info
            for class_info in self.header_info.classes
            if class_info.qualified_name == "Fixture::AAbstractActor"
        )
        assert abstract_class.is_abstract
        assert abstract_class.display_name == "Abstract Actor"
        abstract_definition = self.generated_cpp.split(
            "Durin::DClass* Fixture::AAbstractActor::GetPrivateStaticClass()", 1
        )[1].split(
            "Durin::DClass* Z_Construct_DClass_Fixture_AAbstractActor_NoRegister()", 1
        )[0]
        assert "Durin::EClassFlags::Abstract," in abstract_definition
        assert "nullptr," in abstract_definition
        assert "InternalConstructor<Fixture::AAbstractActor>" not in abstract_definition

        generated_header = generate_header_content(self.header_info)
        abstract_macros = generated_header.split(
            "Z_Construct_DClass_Fixture_AAbstractActor_NoRegister", 1
        )[1]
        assert "DEFINE_DEFAULT_CONSTRUCTOR_CALL(AAbstractActor)" not in abstract_macros
        assert "DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(AAbstractActor)" not in abstract_macros


    def test_no_class_default_object_emits_flag_with_constructor(self):
        class_info = next(
            class_info
            for class_info in self.header_info.classes
            if class_info.qualified_name == "Fixture::AInfrastructure"
        )
        assert class_info.no_class_default_object
        definition = self.generated_cpp.split(
            "Durin::DClass* Fixture::AInfrastructure::GetPrivateStaticClass()", 1
        )[1].split(
            "Durin::DClass* Z_Construct_DClass_Fixture_AInfrastructure_NoRegister()", 1
        )[0]
        assert "Durin::EClassFlags::NoClassDefaultObject," in definition
        assert "InternalConstructor<Fixture::AInfrastructure>" in definition


    def test_qualified_helper_name_and_validation(self):
        assert (
            make_generated_helper_name("Durin::Gameplay::AActor")
            == "Z_Construct_DClass_Durin_Gameplay_AActor"
        )
        assert (
            make_generated_enum_helper_name("Durin::Gameplay::ETeam")
            == "Z_Construct_DEnum_Durin_Gameplay_ETeam"
        )
        with pytest.raises(ValueError):
            make_generated_helper_name("Durin::Gameplay_AActor")


    def test_generated_types_use_module_cpp_package(self):
        assert '"/Cpp/Fixture",' in self.generated_cpp

        manifest = ReflectionPhaseState(module="Fixture")
        with mock.patch.object(utils, "get_module_dht_output_dir", return_value=self.dht_output_dir):
            _write_reflection_files("Fixture", [], {}, manifest, max_workers=1)
        module_content = (self.dht_output_dir / "Fixture.module.gen.cpp").read_text(encoding="utf-8")
        assert 'Durin::RegisterCompiledInPackage("Fixture")' in module_content


    def test_class_display_and_default_object_name_metadata(self):
        assert '"Fixture::ASampleActor",' in self.generated_cpp
        assert '"ASampleActor",' in self.generated_cpp
        assert '11,\n\t"Sample Actor",' in self.generated_cpp
        assert '"Sample Actor",' in self.generated_cpp
        assert '"SampleActor"' in self.generated_cpp


    def test_generated_cpp_symbols_follow_current_qualified_names(self):
        assert '"Fixture::EFixtureMode"' in self.generated_cpp
        assert '"Legacy::EFixtureMode"' in self.generated_cpp
        assert "Fixture::ASampleActor::GetPrivateStaticClass()" in self.generated_cpp
        assert "sizeof(Fixture::FCurvePoint)" in self.generated_cpp


    def test_legacy_names_register_read_only_aliases_without_changing_runtime_identity(self):
        assert '"Fixture::ASampleActor"' in self.generated_cpp
        assert '"Fixture::FCurvePoint"' in self.generated_cpp
        assert '"Legacy::ASampleActor"' in self.generated_cpp
        assert '"Older::ASampleActor"' in self.generated_cpp
        assert '"Legacy::FCurvePoint"' in self.generated_cpp


    def test_enum_display_metadata_binds_to_type_and_values(self):
        fixture_mode = next(enum for enum in self.header_info.enums if enum.short_name == "EFixtureMode")
        assert fixture_mode.is_scoped
        assert fixture_mode.display_name == "Fixture Mode"
        assert [
            (value.name, value.value, value.display_name) for value in fixture_mode.values
        ] == [
            ("Disabled", -1, "Not Enabled"),
            ("URLValue", 0, ""),
            ("FinalValue", 7, "Final (Ready)"),
        ]

        legacy_mode = next(enum for enum in self.header_info.enums if enum.short_name == "ELegacyMode")
        assert not legacy_mode.is_scoped
        assert legacy_mode.display_name == ""
        assert [
            (value.name, value.value, value.display_name) for value in legacy_mode.values
        ] == [
            ("LegacyFirst", 0, ""),
            ("LegacySecond", 4, ""),
        ]
