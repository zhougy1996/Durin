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
from durin_header_tool.extractors.export_symbol_extractor import extract_module_export_info
from durin_header_tool.generators import module_reflection_files_generator as reflection_generator
from durin_header_tool.generators.module_reflection_files_generator import (
    _write_reflection_files,
    make_new_module_manifest,
)
from durin_header_tool.model.export_info import (
    ExportedSymbolInfo,
    load_module_export_file,
    save_module_export_file,
)
from durin_header_tool.model.reflection_manifest import ModuleManifest, save_module_manifest_file
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
    make_dht_parse_source,
    parse_reflection_header,
)
from durin_header_tool.parser import reflection_parser
from durin_header_tool.resolver.reflection_resolver import (
    load_available_symbols,
    resolved_symbol_dependencies_for_header,
)
from durin_header_tool.resolver import reflection_resolver
from durin_header_tool.writers.reflection_source_writer import (
    _enum_definitions,
    generate_cpp_content,
    generate_header_content,
)


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


@pytest.fixture(scope="class")
def reflection_fixture(request, tmp_path_factory):
    request.cls._build_fixture(tmp_path_factory.mktemp("dht-reflection"))


@pytest.mark.usefixtures("reflection_fixture")
class TestReflectionGeneration:
    @classmethod
    def _build_fixture(cls, temp_root):
        # Keep parser/writer integration coverage self-contained; unit tests must not scan production modules.
        cls.temp_root = temp_root
        cls.module_dir = cls.temp_root / "Fixture"
        cls.header = "Public/FixtureTypes.h"
        header_path = cls.module_dir / cls.header
        header_path.parent.mkdir(parents=True)
        header_path.write_text(
            '''#pragma once

namespace Durin
{
    struct FVector3 {};
    struct FLinearColor {};
    class FName {};
    struct FGuid {};
    class DObject {};
}

namespace std
{
    template<typename T>
    class vector {};
}

namespace Fixture
{
    DENUM(DisplayName = "Fixture Mode")
    enum class EFixtureMode : int
    {
        Disabled DMETA(DisplayName = "Not Enabled") = -1,
        URLValue,
        FinalValue DMETA(DisplayName = "Final (Ready)") = 7,
    };

    DENUM()
    enum ELegacyMode : unsigned char
    {
        LegacyFirst,
        LegacySecond = 4
    };

    DCLASS(DisplayName = "Sample Actor", DefaultObjectName = "SampleActor")
    class ASampleActor : public Durin::DObject
    {
        GENERATED_BODY()

        DPROPERTY(Edit, ReadOnly)
        float Value = 0.0f;

        DPROPERTY(Edit, MetaData = "HideAlpha=true")
        Durin::FLinearColor Color{};

        DPROPERTY(Edit)
        Durin::FName Identifier{};

        DPROPERTY(Edit)
        Durin::FGuid PersistentId{};

        DPROPERTY()
        std::vector<Durin::FGuid> RelatedIds;
    };

    DCLASS(Abstract, DisplayName = "Abstract Actor")
    class AAbstractActor : public ASampleActor
    {
        GENERATED_BODY()
    };

    DSTRUCT()
    struct FCurvePoint
    {
        GENERATED_BODY()

        DPROPERTY()
        Durin::FVector3 Position{};

        DPROPERTY()
        Durin::FVector3 Tangent{};
    };
}
''',
            encoding="utf-8",
        )
        cls.module_config = DurinModuleConfig(
            module_name="Fixture",
            module_dir=cls.module_dir,
            reflect_headers=[cls.header],
        )
        cls.dht_output_dir = cls.temp_root / "DHT"

        with (
            mock.patch.object(configs, "get_module_config", return_value=cls.module_config),
            mock.patch.object(configs, "collect_all_dependent_modules", return_value=set()),
            mock.patch.object(utils, "get_module_dht_output_dir", return_value=cls.dht_output_dir),
        ):
            cls.export_info = extract_module_export_info("Fixture")
            cls.symbols = dict(cls.export_info.Symbols)
            cls.symbols.update({
                "Durin::DObject": ExportedSymbolInfo(
                    Kind="class", ShortName="DObject", Namespace="Durin", QualifiedName="Durin::DObject",
                    GeneratedHelperName="Z_Construct_DClass_Durin_DObject", Header="DObject/Object.h", API="COREDOBJECT_API",
                ),
                "Durin::FVector3": ExportedSymbolInfo(
                    Kind="struct", ShortName="FVector3", Namespace="Durin", QualifiedName="Durin::FVector3",
                    GeneratedHelperName="Z_Construct_DStruct_Durin_FVector3", Header="DObject/MathStructs.h", API="COREDOBJECT_API",
                ),
                "Durin::FLinearColor": ExportedSymbolInfo(
                    Kind="struct", ShortName="FLinearColor", Namespace="Durin", QualifiedName="Durin::FLinearColor",
                    GeneratedHelperName="Z_Construct_DStruct_Durin_FLinearColor", Header="DObject/MathStructs.h", API="COREDOBJECT_API",
                ),
            })
            cls.header_info = parse_reflection_header("Fixture", cls.header, exported_symbols=cls.symbols)
            cls.generated_cpp = generate_cpp_content(cls.header_info, cls.symbols)

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

    def test_ambiguous_source_property_symbols_are_not_selected_by_insertion_order(self):
        symbols = {
            "Beta::FData": ExportedSymbolInfo(
                Kind="struct",
                ShortName="FData",
                Namespace="Beta",
                QualifiedName="Beta::FData",
                GeneratedHelperName="Z_Construct_DStruct_Beta_FData",
                Header="Beta.h",
                API="BETA_API",
            ),
            "Alpha::FData": ExportedSymbolInfo(
                Kind="struct",
                ShortName="FData",
                Namespace="Alpha",
                QualifiedName="Alpha::FData",
                GeneratedHelperName="Z_Construct_DStruct_Alpha_FData",
                Header="Alpha.h",
                API="ALPHA_API",
            ),
            "Beta::EMode": ExportedSymbolInfo(
                Kind="enum",
                ShortName="EMode",
                Namespace="Beta",
                QualifiedName="Beta::EMode",
                GeneratedHelperName="Z_Construct_DEnum_Beta_EMode",
                Header="Beta.h",
                API="BETA_API",
                UnderlyingSize=4,
            ),
            "Alpha::EMode": ExportedSymbolInfo(
                Kind="enum",
                ShortName="EMode",
                Namespace="Alpha",
                QualifiedName="Alpha::EMode",
                GeneratedHelperName="Z_Construct_DEnum_Alpha_EMode",
                Header="Alpha.h",
                API="ALPHA_API",
                UnderlyingSize=4,
            ),
        }

        assert _make_property_from_spelling("Data", "FData", symbols) is None
        assert _make_property_from_spelling("Mode", "EMode", symbols) is None
        assert (
            _make_property_from_spelling("Data", "Alpha::FData", symbols).referenced_struct_type
            == "Alpha::FData"
        )
        assert (
            _make_property_from_spelling("Mode", "Alpha::EMode", symbols).referenced_enum_type
            == "Alpha::EMode"
        )

    def test_ast_and_source_properties_share_qualified_symbol_policy(self):
        struct_symbol = ExportedSymbolInfo(
            Kind="struct",
            ShortName="FData",
            Namespace="Alpha",
            QualifiedName="Alpha::FData",
            GeneratedHelperName="Z_Construct_DStruct_Alpha_FData",
            Header="Alpha.h",
            API="ALPHA_API",
        )
        enum_symbol = ExportedSymbolInfo(
            Kind="enum",
            ShortName="EMode",
            Namespace="Alpha",
            QualifiedName="Alpha::EMode",
            GeneratedHelperName="Z_Construct_DEnum_Alpha_EMode",
            Header="Alpha.h",
            API="ALPHA_API",
            UnderlyingSize=4,
        )
        symbols = {
            struct_symbol.QualifiedName: struct_symbol,
            enum_symbol.QualifiedName: enum_symbol,
        }

        struct_type = mock.Mock()
        struct_type.spelling = "FData"
        struct_type.get_canonical.return_value.spelling = "Alpha::FData"
        struct_type.get_declaration.return_value.kind = clang.cindex.CursorKind.STRUCT_DECL
        struct_type.get_size.return_value = 16
        ast_struct = _make_property_from_type("Data", struct_type, symbols)
        source_struct = _make_property_from_spelling("Data", "Alpha::FData", symbols)

        enum_type = mock.Mock()
        enum_type.spelling = "EMode"
        enum_type.get_canonical.return_value.spelling = "EMode"
        enum_declaration = enum_type.get_declaration.return_value
        enum_declaration.kind = clang.cindex.CursorKind.ENUM_DECL
        enum_declaration.spelling = "EMode"
        enum_type.get_size.return_value = 4
        with mock.patch.object(
            reflection_parser,
            "_qualified_name",
            return_value="Alpha::EMode",
        ):
            ast_enum = _make_property_from_type("Mode", enum_type, symbols)
        source_enum = _make_property_from_spelling("Mode", "Alpha::EMode", symbols)

        assert (ast_struct.kind, ast_struct.referenced_struct_type) == (
            source_struct.kind,
            source_struct.referenced_struct_type,
        )
        assert (ast_enum.kind, ast_enum.referenced_enum_type) == (
            source_enum.kind,
            source_enum.referenced_enum_type,
        )

    def test_generated_types_use_module_cpp_package(self):
        assert '"/Cpp/Fixture",' in self.generated_cpp

        manifest = ModuleManifest(module_name="Fixture")
        with mock.patch.object(utils, "get_module_dht_output_dir", return_value=self.dht_output_dir):
            _write_reflection_files("Fixture", [], {}, manifest, max_workers=1)
        module_content = (self.dht_output_dir / "Fixture.module.gen.cpp").read_text(encoding="utf-8")
        assert 'Durin::RegisterCompiledInPackage("Fixture")' in module_content

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
        manifest = ModuleManifest(module_name="Fixture")

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

    def test_class_display_and_default_object_name_metadata(self):
        assert '"Fixture::ASampleActor",' in self.generated_cpp
        assert '"ASampleActor",' in self.generated_cpp
        assert '5,\n\t"Sample Actor",' in self.generated_cpp
        assert '"Sample Actor",' in self.generated_cpp
        assert '"SampleActor"' in self.generated_cpp

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

    @pytest.mark.parametrize(
        ("source", "diagnostic"),
        [
            (
                'DENUM(DisplayName = "First", DisplayName = "Second") enum E { A };',
                "duplicate DisplayName metadata",
            ),
            (
                'DENUM(ToolTip = "No") enum E { A };',
                "unsupported metadata key 'ToolTip'",
            ),
            (
                'DENUM(DisplayName = Bare) enum E { A };',
                "DisplayName requires a quoted string",
            ),
            (
                'DENUM(DisplayName = "Broken) enum E { A };',
                "missing closing ')'",
            ),
            (
                'DENUM(DisplayName = "Missing"',
                "missing closing ')'",
            ),
            (
                'enum E { A DMETA(DisplayName = "First", DisplayName = "Second") };',
                "duplicate DisplayName metadata",
            ),
        ],
        ids=[
            "duplicate-enum-metadata",
            "unsupported-enum-metadata",
            "unquoted-enum-display-name",
            "unterminated-enum-string",
            "unterminated-enum-annotation",
            "duplicate-enumerator-metadata",
        ],
    )
    def test_invalid_enum_metadata_has_deterministic_diagnostics(self, source, diagnostic):
        with pytest.raises(ValueError, match=re.escape(diagnostic)):
            make_dht_parse_source(source)

    @pytest.mark.parametrize(
        ("source", "diagnostic"),
        [
            ("DCLASS(Abstract, Abstract) class FItem {};", "duplicate Abstract class specifier"),
            ("DCLASS(Transient) class FItem {};", "unsupported class specifier 'Transient'"),
            (
                'DCLASS(Abstract = "true") class FItem {};',
                "unsupported class metadata key 'Abstract'",
            ),
            (
                'DCLASS(DisplayName = Bare) class FItem {};',
                "DisplayName requires a quoted string",
            ),
        ],
    )
    def test_invalid_class_specifiers_have_deterministic_diagnostics(self, source, diagnostic):
        with pytest.raises(ValueError, match=re.escape(diagnostic)):
            make_dht_parse_source(source)

    def test_annotation_names_in_comments_and_strings_are_ignored(self):
        source = '''// DENUM(Unknown = "comment")
const char* Text = "DMETA(Unknown = \\"string\\")";
/* DMETA(DisplayName = Bare) */
'''
        assert make_dht_parse_source(source) == source

    def test_translation_unit_skips_function_bodies(self):
        index = mock.Mock()
        index.parse.return_value = mock.sentinel.translation_unit
        with (
            mock.patch("durin_header_tool.parser.reflection_parser._init_clang"),
            mock.patch.object(clang.cindex.Index, "create", return_value=index),
            mock.patch("durin_header_tool.parser.reflection_parser._clang_args", return_value=[]),
            mock.patch("durin_header_tool.parser.reflection_parser._fake_generated_headers", return_value=[]),
        ):
            translation_unit, dmeta_uses = _parse_translation_unit(
                "Fixture",
                Path("FixtureTypes.h"),
                "",
                export_mode=False,
            )

        assert translation_unit is mock.sentinel.translation_unit
        assert dmeta_uses == {}
        assert (
            index.parse.call_args.kwargs["options"]
            == clang.cindex.TranslationUnit.PARSE_SKIP_FUNCTION_BODIES
        )

    def test_dmeta_outside_reflected_enum_is_rejected(self):
        header = "Public/Misplaced.h"
        header_path = self.module_dir / header
        header_path.write_text(
            '''namespace Fixture
{
    enum EPlain
    {
        Value DMETA(DisplayName = "Visible")
    };
}
''',
            encoding="utf-8",
        )
        config = DurinModuleConfig(
            module_name="Fixture",
            module_dir=self.module_dir,
            reflect_headers=[header],
        )
        with (
            mock.patch.object(configs, "get_module_config", return_value=config),
            mock.patch.object(configs, "collect_all_dependent_modules", return_value=set()),
            mock.patch.object(utils, "get_module_dht_output_dir", return_value=self.dht_output_dir),
        ):
            with pytest.raises(ValueError, match="DMETA at line 5, column 15: "
                "annotation is only valid on an enumerator in a reflected enum"):
                with mock.patch(
                    "clang.cindex.Cursor.walk_preorder",
                    side_effect=AssertionError("DMETA validation must not walk the entire translation unit"),
                ):
                    parse_reflection_header("Fixture", header)

    def test_dmeta_usage_is_matched_by_source_occurrence(self):
        header = "Public/MixedMetadata.h"
        header_path = self.module_dir / header
        header_path.write_text(
            '''namespace Fixture
{
    DENUM()
    enum EReflected
    {
        Accepted DMETA(DisplayName = "Same")
    };

    enum EPlain
    {
        Rejected DMETA(DisplayName = "Same")
    };
}
''',
            encoding="utf-8",
        )
        config = DurinModuleConfig(
            module_name="Fixture",
            module_dir=self.module_dir,
            reflect_headers=[header],
        )
        with (
            mock.patch.object(configs, "get_module_config", return_value=config),
            mock.patch.object(configs, "collect_all_dependent_modules", return_value=set()),
            mock.patch.object(utils, "get_module_dht_output_dir", return_value=self.dht_output_dir),
        ):
            with pytest.raises(ValueError, match="DMETA at line 11, column 18"):
                parse_reflection_header("Fixture", header)

    def test_same_short_name_in_different_namespaces_uses_local_class_source(self):
        header = "Public/DuplicateShortNames.h"
        source = '''namespace Alpha
{
    DCLASS()
    class FItem
    {
        GENERATED_BODY()

        DPROPERTY()
        int32 First;
    };
}

namespace Beta
{
    DCLASS()
    class FItem
    {
        GENERATED_BODY()

        DPROPERTY()
        float Second;
    };
}
'''
        header_path = self.module_dir / header
        header_path.write_text(source, encoding="utf-8")
        config = DurinModuleConfig(
            module_name="Fixture",
            module_dir=self.module_dir,
            reflect_headers=[header],
        )
        generated_body_lines = [
            line_number
            for line_number, line in enumerate(source.splitlines(), start=1)
            if "GENERATED_BODY" in line
        ]

        with (
            mock.patch.object(configs, "get_module_config", return_value=config),
            mock.patch.object(configs, "collect_all_dependent_modules", return_value=set()),
            mock.patch.object(utils, "get_module_dht_output_dir", return_value=self.dht_output_dir),
            mock.patch("durin_header_tool.parser.reflection_parser._make_property", return_value=None),
        ):
            header_info = parse_reflection_header("Fixture", header)

        classes = {class_info.qualified_name: class_info for class_info in header_info.classes}
        assert classes["Alpha::FItem"].generated_body_line == generated_body_lines[0]
        assert classes["Beta::FItem"].generated_body_line == generated_body_lines[1]
        assert [prop.name for prop in classes["Alpha::FItem"].properties] == ["First"]
        assert [prop.name for prop in classes["Beta::FItem"].properties] == ["Second"]

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

    def test_property_flags_metadata_and_value_lifecycle_are_generated(self):
        assert (
            'NewProp_Value = { "Value", Durin::EPropertyFlags::Edit | Durin::EPropertyFlags::ReadOnly,'
            in self.generated_cpp
        )
        assert 'NewProp_Color_MetaData[] = { { "HideAlpha", "true" } };' in self.generated_cpp
        assert "Z_Construct_DStruct_Durin_FLinearColor" in self.generated_cpp
        assert 'NewProp_Color = { "Color", Durin::EPropertyFlags::Edit,' in self.generated_cpp
        value_type = "std::remove_extent_t<decltype(((Fixture::ASampleActor*)0)->Color)>"
        assert f"sizeof({value_type})" in self.generated_cpp
        assert f"alignof({value_type})" in self.generated_cpp
        assert f"InitializePropertyValue<{value_type}>" in self.generated_cpp
        assert f"DestroyPropertyValue<{value_type}>" in self.generated_cpp

    def test_fname_property_is_generated(self):
        assert "Durin::DurinCodeGen::FNamePropertyParams NewProp_Identifier" in self.generated_cpp
        assert "Durin::DurinCodeGen::EPropertyGenFlags::Name" in self.generated_cpp

    def test_guid_properties_are_generated_directly_and_in_arrays(self):
        assert "Durin::DurinCodeGen::FGuidPropertyParams NewProp_PersistentId" in self.generated_cpp
        assert "Durin::DurinCodeGen::EPropertyGenFlags::Guid" in self.generated_cpp
        assert "Durin::DurinCodeGen::FArrayPropertyParams NewProp_RelatedIds" in self.generated_cpp
        assert "Durin::DurinCodeGen::FGuidPropertyParams NewProp_RelatedIds_Inner" in self.generated_cpp

    def test_brace_initialized_intrinsic_struct_properties_are_generated(self):
        for property_name in ("Position", "Tangent"):
            assert f'NewProp_{property_name} = {{ "{property_name}",' in self.generated_cpp
        assert "Z_Construct_DStruct_Durin_FVector3" in self.generated_cpp

    def test_default_double_vector_intrinsics_are_available(self):
        missing_export = self.temp_root / "missing.export"
        with (
            mock.patch.object(utils, "get_module_export_file_path", return_value=missing_export),
            mock.patch.object(configs, "collect_all_dependent_module_with_export_file", return_value=[]),
        ):
            symbols = load_available_symbols("Fixture")

        for type_name in ("FVector2", "FVector3", "FVector4"):
            qualified_name = f"Durin::{type_name}"
            assert qualified_name in symbols
            assert symbols[qualified_name].GeneratedHelperName == f"Z_Construct_DStruct_Durin_{type_name}"

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

    def test_manifest_records_generator_contract(self):
        manifest_path = self.temp_root / "Fixture.manifest"
        with (
            mock.patch.object(configs, "get_module_config", return_value=self.module_config),
            mock.patch.object(configs, "collect_all_dependent_module_with_export_file", return_value=[]),
            mock.patch.object(configs, "ARCH", "Win64"),
            mock.patch.object(configs, "RUNTIME_VARIANT", "DurinEditor"),
            mock.patch.object(configs, "TOOL_FINGERPRINT", "fixture-fingerprint"),
            mock.patch.object(utils, "get_module_manifest_file_path", return_value=manifest_path),
        ):
            manifest = make_new_module_manifest("Fixture")
            manifest.resolved_symbol_dependencies[self.header] = resolved_symbol_dependencies_for_header(self.header_info, self.symbols)
            content = save_module_manifest_file(manifest)
        data = json.loads(content)

        assert data["SchemaVersion"] == 5
        assert data["ToolFingerprint"] == "fixture-fingerprint"
        assert data["SymbolNameScheme"] == "qualified-underscore-v1"
        assert data["ModuleName"] == "Fixture"
        assert data["RuntimeVariant"] == "DurinEditor"
        assert "Profile" not in data
        assert data["Platform"] == "Win64"
        assert data["GeneratedOutputs"] == [
            "Fixture.module.gen.cpp",
            "FixtureTypes.gen.cpp",
            "FixtureTypes.gen.h",
        ]
        assert data["PendingCleanupOutputs"] == []
        actor_dependencies = data["ResolvedSymbolDependencies"][self.header]
        assert actor_dependencies["Durin::DObject"]["GeneratedHelperName"] == "Z_Construct_DClass_Durin_DObject"
