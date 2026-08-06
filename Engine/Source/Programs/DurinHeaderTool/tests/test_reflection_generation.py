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
    _validate_explicit_container_spelling,
    _validate_soft_object_spelling,
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
    template<typename T> class TObjectPtr {};
    template<typename T> class TSoftObjectPtr {};
    using int32 = int;
    template<typename T> struct TDStructOpsTraitsBase {};
    template<typename T> struct TDStructOpsTraits {};
}

namespace std
{
    template<typename T>
    class vector {};
    template<typename K, typename V>
    class unordered_map {};
}

namespace Fixture
{
    using Durin::int32;
    using FFloatVector = std::vector<float>;

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

        DPROPERTY(Edit, MetaData = "Role=Primary")
        std::string Label;

        DPROPERTY()
        float Weights[2]{};

        DPROPERTY()
        EFixtureMode Mode = EFixtureMode::Disabled;

        DPROPERTY()
        Durin::DObject* RawReference = nullptr;

        DPROPERTY()
        Durin::TObjectPtr<Durin::DObject> StrongReference;
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

    DSTRUCT()
    struct FMoveOnly
    {
        GENERATED_BODY()
        FMoveOnly() = default;
        FMoveOnly(const FMoveOnly&) = delete;
        auto operator=(const FMoveOnly&) -> FMoveOnly& = delete;
        FMoveOnly(FMoveOnly&&) = default;
        auto operator=(FMoveOnly&&) -> FMoveOnly& = default;
    };

    DSTRUCT()
    struct FDeletedDefault
    {
        GENERATED_BODY()
        FDeletedDefault() = delete;
        explicit FDeletedDefault(int InValue);
        int Value = 0;
    };

    DSTRUCT()
    struct FTrivialOps
    {
        GENERATED_BODY()
        DPROPERTY()
        float Value = 0.0f;
    };

    DSTRUCT()
    struct FNonTrivialOps
    {
        GENERATED_BODY()
        DPROPERTY()
        std::vector<float> Values;
    };

    DSTRUCT()
    struct FCustomEquality
    {
        GENERATED_BODY()
        DPROPERTY()
        float Value = 0.0f;
    };

    DSTRUCT()
    struct FMalformedEquality
    {
        GENERATED_BODY()
    };

    DSTRUCT()
    struct FUnavailableContainers
    {
        GENERATED_BODY()
        DPROPERTY()
        std::vector<FDeletedDefault> Values;
        DPROPERTY()
        std::unordered_map<float, FMoveOnly> MoveValues;
    };

    DCLASS()
    class AContainerShapes : public Durin::DObject
    {
        GENERATED_BODY()

        DPROPERTY()
        std::vector<int32> DirectScores;

        DPROPERTY()
        FFloatVector AliasedScores;

        DPROPERTY()
        std::vector<std::vector<int32>> NestedScores;

        DPROPERTY()
        std::vector<Durin::TObjectPtr<Durin::DObject>> ObjectReferences;

        DPROPERTY()
        std::unordered_map<std::string, int32> NamedScores;

        DPROPERTY()
        std::unordered_map<std::string, std::vector<Durin::TObjectPtr<Durin::DObject>>> ObjectLists;

        DPROPERTY(Edit)
        Durin::TSoftObjectPtr<Durin::DObject> SoftReference;

        DPROPERTY()
        Durin::TSoftObjectPtr<Durin::DObject> SoftFixed[2];

        DPROPERTY()
        std::vector<Durin::TSoftObjectPtr<Durin::DObject>> SoftReferences;

        DPROPERTY()
        std::unordered_map<std::string, std::vector<Durin::TSoftObjectPtr<Durin::DObject>>> SoftLists;
    };

    DSTRUCT()
    struct FNonTrivialDestructor
    {
        GENERATED_BODY()
        ~FNonTrivialDestructor();
    };

    DSTRUCT()
    struct FStructPropertyShapes
    {
        GENERATED_BODY()

        DPROPERTY()
        FTrivialOps Direct;

        DPROPERTY()
        FDeletedDefault DeletedDefault;

        DPROPERTY()
        FMoveOnly DeletedCopy;

        DPROPERTY()
        FNonTrivialDestructor NonTrivialDestructor;

        DPROPERTY(Edit, MetaData = "Role=Primary")
        FTrivialOps Metadata;

        DPROPERTY()
        FTrivialOps Fixed[3];

        DPROPERTY()
        std::vector<FDeletedDefault> ArrayValues;

        DPROPERTY()
        std::unordered_map<FMoveOnly, float> MapByStruct;

        DPROPERTY()
        std::unordered_map<float, FDeletedDefault> MapToStruct;
    };
}

template<>
struct Durin::TDStructOpsTraits<Fixture::FCustomEquality>
    : Durin::TDStructOpsTraitsBase<Fixture::FCustomEquality>
{
    static constexpr bool bWithIdentical = true;
    static bool Identical(const Fixture::FCustomEquality&, const Fixture::FCustomEquality&);
};

template<>
struct Durin::TDStructOpsTraits<Fixture::FMalformedEquality>
    : Durin::TDStructOpsTraitsBase<Fixture::FMalformedEquality>
{
    static constexpr bool bWithIdentical = true;
    static void Identical(const Fixture::FMalformedEquality&, const Fixture::FMalformedEquality&);
};
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
        assert '10,\n\t"Sample Actor",' in self.generated_cpp
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
            mock.patch.object(clang.cindex.Index, "create", return_value=index),
            mock.patch("durin_header_tool.parser.reflection_parser._clang_args", return_value=[]),
        ):
            translation_unit, dmeta_uses = _parse_translation_unit(
                "Fixture",
                "Public/FixtureTypes.h",
                Path("FixtureTypes.h"),
                '#include "Ordinary.h"\nDCLASS() class FItem {};\n',
                export_mode=False,
            )

        assert translation_unit is mock.sentinel.translation_unit
        assert dmeta_uses == {}
        parse_args = index.parse.call_args.kwargs["args"]
        assert not any(argument.startswith("-I") for argument in parse_args)
        unsaved_files = index.parse.call_args.kwargs["unsaved_files"]
        assert len(unsaved_files) == 2
        assert "Ordinary.h" not in unsaved_files[0][1]
        assert unsaved_files[0][1].count("\n") == 2
        assert (
            index.parse.call_args.kwargs["options"]
            == clang.cindex.TranslationUnit.PARSE_SKIP_FUNCTION_BODIES
        )

    def test_ordinary_include_content_is_not_a_semantic_input(self):
        header = "Public/Hermetic.h"
        header_path = self.module_dir / header
        included_path = header_path.parent / "Ordinary.h"
        header_path.write_text(
            '''#include "Ordinary.h"
namespace Fixture
{
    DCLASS()
    class AHermetic : public Durin::DObject
    {
        GENERATED_BODY()
        DPROPERTY()
        float Value = 0.0f;
    };
}
''',
            encoding="utf-8",
        )
        included_path.write_text("using IncludedMeaning = int;\n", encoding="utf-8")
        config = DurinModuleConfig(
            module_name="Fixture",
            module_dir=self.module_dir,
            reflect_headers=[header],
        )

        def extract_outputs():
            header_info = parse_reflection_header("Fixture", header, exported_symbols=self.symbols)
            return (
                _extract_header_export_symbols_impl("Fixture", header),
                generate_header_content(header_info),
                generate_cpp_content(header_info, self.symbols),
            )

        with (
            mock.patch.object(configs, "get_module_config", return_value=config),
            mock.patch.object(configs, "collect_all_dependent_modules", return_value=set()),
        ):
            expected = extract_outputs()
            included_path.write_text("this is deliberately invalid C++\n", encoding="utf-8")
            assert extract_outputs() == expected
            included_path.unlink()
            assert extract_outputs() == expected

    def test_missing_include_alias_has_deterministic_diagnostic(self):
        header = "Public/UnsupportedAlias.h"
        header_path = self.module_dir / header
        header_path.write_text(
            '''#include "Alias.h"
namespace Fixture
{
    DSTRUCT()
    struct FUnsupportedAlias
    {
        GENERATED_BODY()
        DPROPERTY()
        IncludedAlias Value;
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
            pytest.raises(ValueError, match="unsupported non-hermetic type spelling 'IncludedAlias'"),
        ):
            parse_reflection_header("Fixture", header, exported_symbols=self.symbols)

    def test_unknown_conditional_macro_has_deterministic_diagnostic(self):
        header = "Public/UnsupportedConditional.h"
        header_path = self.module_dir / header
        header_path.write_text(
            "#if INCLUDED_FEATURE\nDCLASS() class FConditional {};\n#endif\n",
            encoding="utf-8",
        )
        config = DurinModuleConfig(
            module_name="Fixture",
            module_dir=self.module_dir,
            reflect_headers=[header],
        )
        with (
            mock.patch.object(configs, "get_module_config", return_value=config),
            pytest.raises(ValueError, match="unsupported non-hermetic macro dependency 'INCLUDED_FEATURE'"),
        ):
            parse_reflection_header("Fixture", header)

    def test_module_export_resolves_cold_same_module_base(self):
        base = ExportedSymbolInfo(
            Kind="class", ShortName="ABase", Namespace="Fixture", QualifiedName="Fixture::ABase",
            GeneratedHelperName="Z_Construct_DClass_Fixture_ABase", Header="Public/Base.h", API="FIXTURE_API",
        )
        derived = ExportedSymbolInfo(
            Kind="class", ShortName="ADerived", Namespace="Fixture", QualifiedName="Fixture::ADerived",
            GeneratedHelperName="Z_Construct_DClass_Fixture_ADerived", Header="Public/Derived.h", API="FIXTURE_API",
            BaseQualifiedName="ABase",
        )
        first = resolve_module_export_info(
            "Fixture",
            {"Public/Derived.h": {derived.QualifiedName: derived}, "Public/Base.h": {base.QualifiedName: base}},
            {},
        )
        second = resolve_module_export_info(
            "Fixture",
            {"Public/Base.h": {base.QualifiedName: base}, "Public/Derived.h": {derived.QualifiedName: derived}},
            {},
        )

        assert first == second
        assert first.Symbols[derived.QualifiedName].BaseQualifiedName == base.QualifiedName

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

    def test_property_flags_metadata_and_typed_struct_registration_are_generated(self):
        assert (
            'NewProp_Value = { "Value", Durin::EPropertyFlags::Edit | Durin::EPropertyFlags::ReadOnly,'
            in self.generated_cpp
        )
        assert 'NewProp_Color_MetaData[] = { { "HideAlpha", "true" } };' in self.generated_cpp
        color_definition = next(
            line for line in self.generated_cpp.splitlines()
            if "::NewProp_Color =" in line
        )
        assert (
            'NewProp_Color = { "Color", Durin::EPropertyFlags::Edit, 1, '
            'static_cast<Durin::uint16>(STRUCT_OFFSET(Fixture::ASampleActor, Color)), '
            'Z_Construct_DStruct_Durin_FLinearColor, '
            'Z_Construct_DClass_Fixture_ASampleActor_Statics::NewProp_Color_MetaData, 1 };'
        ) in color_definition
        assert "sizeof(" not in color_definition
        assert "alignof(" not in color_definition
        assert "EPropertyGenFlags::Struct" not in color_definition
        assert "InitializePropertyValue" not in color_definition
        assert "DestroyPropertyValue" not in color_definition
        assert "nullptr" not in color_definition

    def test_fname_property_is_generated(self):
        assert "Durin::DurinCodeGen::FNamePropertyParams NewProp_Identifier" in self.generated_cpp
        definition = next(
            line for line in self.generated_cpp.splitlines()
            if "ASampleActor_Statics::NewProp_Identifier =" in line
        )
        assert definition.endswith(
            'NewProp_Identifier = { "Identifier", Durin::EPropertyFlags::Edit, 1, '
            'static_cast<Durin::uint16>(STRUCT_OFFSET(Fixture::ASampleActor, Identifier)) };'
        )
        assert "EPropertyGenFlags" not in definition

    def test_guid_properties_are_generated_directly_and_in_arrays(self):
        assert "Durin::DurinCodeGen::FGuidPropertyParams NewProp_PersistentId" in self.generated_cpp
        assert "Durin::DurinCodeGen::FArrayPropertyParams NewProp_RelatedIds" in self.generated_cpp
        assert "Durin::DurinCodeGen::FGuidPropertyParams NewProp_RelatedIds_Inner" in self.generated_cpp
        direct = next(
            line for line in self.generated_cpp.splitlines()
            if "ASampleActor_Statics::NewProp_PersistentId =" in line
        )
        nested = next(
            line for line in self.generated_cpp.splitlines()
            if "ASampleActor_Statics::NewProp_RelatedIds_Inner =" in line
        )
        assert direct.endswith(
            'NewProp_PersistentId = { "PersistentId", Durin::EPropertyFlags::Edit, 1, '
            'static_cast<Durin::uint16>(STRUCT_OFFSET(Fixture::ASampleActor, PersistentId)) };'
        )
        assert nested.endswith(
            'NewProp_RelatedIds_Inner = { "RelatedIds_Inner", Durin::EPropertyFlags::None, 1, 0 };'
        )
        for definition in (direct, nested):
            assert "sizeof(" not in definition
            assert "alignof(" not in definition
            assert "EPropertyGenFlags" not in definition
            assert "InitializePropertyValue" not in definition
            assert "DestroyPropertyValue" not in definition

    def test_leaf_property_forms_use_concise_typed_registration(self):
        statics = "Z_Construct_DClass_Fixture_ASampleActor_Statics"
        expected_suffixes = {
            "Value": (
                'NewProp_Value = { "Value", Durin::EPropertyFlags::Edit | Durin::EPropertyFlags::ReadOnly, 1, '
                'static_cast<Durin::uint16>(STRUCT_OFFSET(Fixture::ASampleActor, Value)) };'
            ),
            "Weights": (
                'NewProp_Weights = { "Weights", Durin::EPropertyFlags::None, 2, '
                'static_cast<Durin::uint16>(STRUCT_OFFSET(Fixture::ASampleActor, Weights)) };'
            ),
            "Mode": (
                'NewProp_Mode = { "Mode", Durin::EPropertyFlags::None, 1, '
                'static_cast<Durin::uint16>(STRUCT_OFFSET(Fixture::ASampleActor, Mode)), '
                'Z_Construct_DEnum_Fixture_EFixtureMode };'
            ),
            "RawReference": (
                'NewProp_RawReference = Durin::DurinCodeGen::FObjectPropertyParams::Raw<Durin::DObject>('
                '"RawReference", Durin::EPropertyFlags::None, 1, '
                'static_cast<Durin::uint16>(STRUCT_OFFSET(Fixture::ASampleActor, RawReference)), '
                'Z_Construct_DClass_Durin_DObject);'
            ),
            "StrongReference": (
                'NewProp_StrongReference = Durin::DurinCodeGen::FObjectPropertyParams::ObjectPtr<Durin::DObject>('
                '"StrongReference", Durin::EPropertyFlags::None, 1, '
                'static_cast<Durin::uint16>(STRUCT_OFFSET(Fixture::ASampleActor, StrongReference)), '
                'Z_Construct_DClass_Durin_DObject);'
            ),
        }
        definitions = {}
        for name, suffix in expected_suffixes.items():
            definition = next(
                line for line in self.generated_cpp.splitlines()
                if f"{statics}::NewProp_{name} =" in line
            )
            assert definition.endswith(suffix)
            definitions[name] = definition
        label = next(
            line for line in self.generated_cpp.splitlines()
            if f"{statics}::NewProp_Label =" in line
        )
        assert label.endswith(
            f'NewProp_Label = {{ "Label", Durin::EPropertyFlags::Edit, 1, '
            f'static_cast<Durin::uint16>(STRUCT_OFFSET(Fixture::ASampleActor, Label)), '
            f'{statics}::NewProp_Label_MetaData, 1 }};'
        )
        object_inner = next(
            line for line in self.generated_cpp.splitlines()
            if "AContainerShapes_Statics::NewProp_ObjectReferences_Inner =" in line
        )
        assert "FObjectPropertyParams::ObjectPtr<Durin::DObject>" in object_inner
        assert '"ObjectReferences_Inner", Durin::EPropertyFlags::None, 1, 0,' in object_inner
        for definition in (*definitions.values(), label, object_inner):
            assert "sizeof(" not in definition
            assert "alignof(" not in definition
            assert "decltype" not in definition
            assert "InitializePropertyValue" not in definition
            assert "DestroyPropertyValue" not in definition

    def test_brace_initialized_intrinsic_struct_properties_are_generated(self):
        for property_name in ("Position", "Tangent"):
            definition = next(
                line for line in self.generated_cpp.splitlines()
                if f"::NewProp_{property_name} =" in line
            )
            assert f'NewProp_{property_name} = {{ "{property_name}",' in definition
            assert definition.endswith("Z_Construct_DStruct_Durin_FVector3 };")
            assert "nullptr" not in definition
            assert "sizeof(" not in definition
            assert "alignof(" not in definition
            assert "EPropertyGenFlags::Struct" not in definition

    def test_all_struct_property_forms_use_concise_typed_registration(self):
        statics = "Z_Construct_DStruct_Fixture_FStructPropertyShapes_Statics"
        expected_definitions = {
            "Direct": (
                'NewProp_Direct = { "Direct", Durin::EPropertyFlags::None, 1, '
                'static_cast<Durin::uint16>(STRUCT_OFFSET(Fixture::FStructPropertyShapes, Direct)), '
                'Z_Construct_DStruct_Fixture_FTrivialOps };'
            ),
            "DeletedDefault": (
                'NewProp_DeletedDefault = { "DeletedDefault", Durin::EPropertyFlags::None, 1, '
                'static_cast<Durin::uint16>(STRUCT_OFFSET(Fixture::FStructPropertyShapes, DeletedDefault)), '
                'Z_Construct_DStruct_Fixture_FDeletedDefault };'
            ),
            "DeletedCopy": (
                'NewProp_DeletedCopy = { "DeletedCopy", Durin::EPropertyFlags::None, 1, '
                'static_cast<Durin::uint16>(STRUCT_OFFSET(Fixture::FStructPropertyShapes, DeletedCopy)), '
                'Z_Construct_DStruct_Fixture_FMoveOnly };'
            ),
            "NonTrivialDestructor": (
                'NewProp_NonTrivialDestructor = { "NonTrivialDestructor", Durin::EPropertyFlags::None, 1, '
                'static_cast<Durin::uint16>(STRUCT_OFFSET(Fixture::FStructPropertyShapes, NonTrivialDestructor)), '
                'Z_Construct_DStruct_Fixture_FNonTrivialDestructor };'
            ),
            "Metadata": (
                'NewProp_Metadata = { "Metadata", Durin::EPropertyFlags::Edit, 1, '
                'static_cast<Durin::uint16>(STRUCT_OFFSET(Fixture::FStructPropertyShapes, Metadata)), '
                f'Z_Construct_DStruct_Fixture_FTrivialOps, {statics}::NewProp_Metadata_MetaData, 1 }};'
            ),
            "Fixed": (
                'NewProp_Fixed = { "Fixed", Durin::EPropertyFlags::None, 3, '
                'static_cast<Durin::uint16>(STRUCT_OFFSET(Fixture::FStructPropertyShapes, Fixed)), '
                'Z_Construct_DStruct_Fixture_FTrivialOps };'
            ),
            "ArrayValues_Inner": (
                'NewProp_ArrayValues_Inner = { "ArrayValues_Inner", Durin::EPropertyFlags::None, 1, 0, '
                'Z_Construct_DStruct_Fixture_FDeletedDefault };'
            ),
            "MapByStruct_Key": (
                'NewProp_MapByStruct_Key = { "MapByStruct_Key", Durin::EPropertyFlags::None, 1, 0, '
                'Z_Construct_DStruct_Fixture_FMoveOnly };'
            ),
            "MapToStruct_Value": (
                'NewProp_MapToStruct_Value = { "MapToStruct_Value", Durin::EPropertyFlags::None, 1, 0, '
                'Z_Construct_DStruct_Fixture_FDeletedDefault };'
            ),
        }

        assert f'{statics}::NewProp_Metadata_MetaData[] = {{ {{ "Role", "Primary" }} }};' in self.generated_cpp
        forbidden_tokens = (
            "EPropertyGenFlags::Struct",
            "sizeof(",
            "alignof(",
            "nullptr",
            "InitializePropertyValue",
            "DestroyPropertyValue",
        )
        for property_name, expected in expected_definitions.items():
            definition = next(
                line for line in self.generated_cpp.splitlines()
                if f"{statics}::NewProp_{property_name} =" in line
            )
            assert definition == (
                f"const Durin::DurinCodeGen::FStructPropertyParams {statics}::{expected}"
            )
            assert not any(token in definition for token in forbidden_tokens)

    def test_struct_property_shape_order_and_resolver_identities_are_stable(self):
        shape_info = next(
            struct_info
            for struct_info in self.header_info.structs
            if struct_info.qualified_name == "Fixture::FStructPropertyShapes"
        )
        expected_properties = [
            "Direct",
            "DeletedDefault",
            "DeletedCopy",
            "NonTrivialDestructor",
            "Metadata",
            "Fixed",
            "ArrayValues",
            "MapByStruct",
            "MapToStruct",
        ]
        assert [prop.name for prop in shape_info.properties] == expected_properties

        statics = shape_info.generated_statics_name
        pointer_block = self.generated_cpp.split(
            f"const Durin::DurinCodeGen::FPropertyParamsBase* const {statics}::PropertyParams[] = {{", 1
        )[1].split("};", 1)[0]
        assert [
            line.strip().removeprefix(f"&{statics}::NewProp_").removesuffix(",")
            for line in pointer_block.splitlines()
            if line.strip()
        ] == expected_properties

    def test_struct_lifecycle_registration_uses_compiler_checked_operation_tables(self):
        for type_name in (
            "FCurvePoint",
            "FMoveOnly",
            "FDeletedDefault",
            "FTrivialOps",
            "FNonTrivialOps",
            "FCustomEquality",
            "FMalformedEquality",
            "FUnavailableContainers",
            "FNonTrivialDestructor",
            "FStructPropertyShapes",
        ):
            assert f"&Durin::GetDStructOps<Fixture::{type_name}>()" in self.generated_cpp

        assert "static void Initialize(void* Memory);" not in self.generated_cpp
        assert "static void Destroy(void* Memory);" not in self.generated_cpp
        assert "static void Copy(void* Destination, const void* Source);" not in self.generated_cpp
        assert "new (Memory) Fixture::" not in self.generated_cpp

    def test_unavailable_nested_struct_operations_are_guarded(self):
        assert "&Durin::ResolveArrayOps<std::remove_extent_t<decltype(((Fixture::FUnavailableContainers*)0)->Values)>>" in self.generated_cpp
        assert "&Durin::ResolveMapOps<std::remove_extent_t<decltype(((Fixture::FUnavailableContainers*)0)->MoveValues)>>" in self.generated_cpp
        assert "NewProp_Values_ArrayResize" not in self.generated_cpp
        assert "NewProp_MoveValues_MapInsert" not in self.generated_cpp
        inner_definition = next(
            line for line in self.generated_cpp.splitlines()
            if "FUnavailableContainers_Statics::NewProp_Values_Inner =" in line
        )
        assert inner_definition.endswith("Z_Construct_DStruct_Fixture_FDeletedDefault };")
        assert "nullptr" not in inner_definition
        assert "sizeof(" not in inner_definition
        assert "alignof(" not in inner_definition
        assert "EPropertyGenFlags::Struct" not in inner_definition
        assert "InitializePropertyValue" not in inner_definition
        assert "DestroyPropertyValue" not in inner_definition

    def test_container_spelling_contract_and_alias_resolution(self):
        symbols = self.symbols

        assert _make_property_from_spelling("Bits", "std::vector<bool>", symbols) is None
        assert _make_property_from_spelling(
            "Allocated", "std::vector<int32, FAllocator>", symbols
        ) is None
        custom_map = _make_property_from_spelling(
            "CustomMap",
            "std::unordered_map<std::string, int32, FHash, FEqual, FAllocator>",
            symbols,
        )
        assert custom_map is None
        assert _make_property_from_spelling("Alias", "FIntVector", symbols) is None
        assert _make_property_from_spelling(
            "ObjectKey", "std::unordered_map<Durin::DObject*, int32>", symbols
        ) is None
        assert _make_property_from_spelling(
            "DepthFour", "std::vector<std::vector<std::vector<std::vector<int32>>>>", symbols
        ) is not None
        assert _make_property_from_spelling(
            "DepthFive",
            "std::vector<std::vector<std::vector<std::vector<std::vector<int32>>>>>",
            symbols,
        ) is None

        for property_name in (
            "DirectScores",
            "NestedScores",
            "ObjectReferences",
            "NamedScores",
            "ObjectLists",
            "SoftReference",
            "SoftFixed",
            "SoftReferences",
            "SoftLists",
            "AliasedScores",
        ):
            assert f"NewProp_{property_name}" in self.generated_cpp

        alias_definition = next(
            line for line in self.generated_cpp.splitlines()
            if "AContainerShapes_Statics::NewProp_AliasedScores =" in line
        )
        assert "ResolveArrayOps<std::remove_extent_t<decltype(" in alias_definition
        assert "std::vector" not in alias_definition

    def test_soft_objects_emit_typed_expected_class_metadata_without_generic_placeholders(self):
        shape_info = next(info for info in self.header_info.classes if info.short_name == "AContainerShapes")
        properties = {prop.name: prop for prop in shape_info.properties}
        assert properties["SoftReference"].kind == "SoftObject"
        assert properties["SoftReference"].referenced_type == "Durin::DObject"
        assert properties["SoftFixed"].array_dim == 2
        assert properties["SoftReferences"].inner.kind == "SoftObject"
        assert properties["SoftLists"].value.inner.kind == "SoftObject"

        direct_definition = next(
            line for line in self.generated_cpp.splitlines()
            if "AContainerShapes_Statics::NewProp_SoftReference =" in line
        )
        assert direct_definition == (
            "const Durin::DurinCodeGen::FSoftObjectPropertyParams "
            "Z_Construct_DClass_Fixture_AContainerShapes_Statics::NewProp_SoftReference = "
            "Durin::DurinCodeGen::FSoftObjectPropertyParams::Create<"
            "std::remove_extent_t<decltype(((Fixture::AContainerShapes*)0)->SoftReference)>>("
            '\"SoftReference\", Durin::EPropertyFlags::Edit, 1, '
            "static_cast<Durin::uint16>(STRUCT_OFFSET(Fixture::AContainerShapes, SoftReference)), "
            "Z_Construct_DClass_Durin_DObject);"
        )
        nested_definition = next(
            line for line in self.generated_cpp.splitlines()
            if "AContainerShapes_Statics::NewProp_SoftReferences_Inner =" in line
        )
        assert "Create<Durin::TSoftObjectPtr<Durin::DObject>>" in nested_definition
        for line in (direct_definition, nested_definition):
            assert "InitializePropertyValue" not in line
            assert "DestroyPropertyValue" not in line
            assert "nullptr" not in line
            assert "EPropertyGenFlags::Object" not in line

    @pytest.mark.parametrize(
        ("spelling", "diagnostic"),
        [
            (
                "Durin::FSoftObjectPtr",
                "[DHT-SOFT001] DPROPERTY 'Value' at line 23: raw FSoftObjectPtr is unsupported; use TSoftObjectPtr<ReflectedObjectClass>",
            ),
            (
                "Durin::TSoftObjectPtr",
                "[DHT-SOFT001] DPROPERTY 'Value' at line 23: TSoftObjectPtr requires exactly one reflected object class",
            ),
            (
                "const Durin::TSoftObjectPtr<Durin::DObject>",
                "[DHT-SOFT003] DPROPERTY 'Value' at line 23: soft object properties do not support cv-qualifiers, pointers, or references",
            ),
            (
                "Durin::TSoftObjectPtr<Durin::DObject>&",
                "[DHT-SOFT003] DPROPERTY 'Value' at line 23: soft object properties do not support cv-qualifiers, pointers, or references",
            ),
            (
                "Durin::TSoftObjectPtr<const Durin::DObject>",
                "[DHT-SOFT003] DPROPERTY 'Value' at line 23: soft object target 'const Durin::DObject' must be an unqualified object class",
            ),
            (
                "Durin::TSoftObjectPtr<Durin::DObject*>",
                "[DHT-SOFT003] DPROPERTY 'Value' at line 23: soft object target 'Durin::DObject*' must be an unqualified object class",
            ),
            (
                "Durin::TSoftObjectPtr<Fixture::FTrivialOps>",
                "[DHT-SOFT004] DPROPERTY 'Value' at line 23: soft object target 'Fixture::FTrivialOps' is not an object class",
            ),
            (
                "Durin::TSoftObjectPtr<Fixture::DMissing>",
                "[DHT-SOFT005] DPROPERTY 'Value' at line 23: soft object target 'Fixture::DMissing' could not be resolved",
            ),
            (
                "std::unordered_map<Durin::TSoftObjectPtr<Durin::DObject>, float>",
                "[DHT-SOFT006] DPROPERTY 'Value' at line 23: soft object references are unsupported as Map keys",
            ),
        ],
    )
    def test_unsupported_soft_object_forms_have_stable_diagnostics(self, spelling, diagnostic):
        with pytest.raises(ValueError, match=re.escape(diagnostic)):
            _validate_soft_object_spelling(spelling, "Value", 23, self.symbols)

    @pytest.mark.parametrize(
        "declaration",
        ["FSoftAlias Value;", "std::vector<FSoftAlias> Value;"],
    )
    def test_soft_object_alias_is_rejected_before_cpp_generation(self, declaration):
        header = "Public/UnsupportedSoftAlias.h"
        header_path = self.module_dir / header
        header_path.write_text(
            '''#pragma once
namespace Fixture
{
    using FSoftAlias = Durin::TSoftObjectPtr<Durin::DObject>;
    DSTRUCT()
    struct FUnsupportedSoftAlias
    {
        GENERATED_BODY()
        DPROPERTY()
        PLACEHOLDER
    };
}
'''.replace("PLACEHOLDER", declaration),
            encoding="utf-8",
        )

        with (
            mock.patch.object(configs, "get_module_config", return_value=self.module_config),
            mock.patch.object(configs, "collect_all_dependent_modules", return_value=set()),
            pytest.raises(
                ValueError,
                match=re.escape(
                    "[DHT-SOFT002] DPROPERTY 'Value' at line 10: aliases of "
                    "TSoftObjectPtr<T> are unsupported; spell the template directly"
                ),
            ),
        ):
            parse_reflection_header("Fixture", header, exported_symbols=self.symbols)

    @pytest.mark.parametrize(
        ("spelling", "diagnostic"),
        [
            ("std::vector<bool>", "[DHT-CONT002] DPROPERTY 'Value' at line 17: std::vector<bool> proxy references are unsupported"),
            ("std::vector<int32, FAllocator>", "[DHT-CONT001] DPROPERTY 'Value' at line 17: std::vector requires the default allocator form std::vector<T>"),
            ("std::unordered_map<int32, float, FHash>", "[DHT-CONT003] DPROPERTY 'Value' at line 17: std::unordered_map requires the default hash, equality, and allocator form std::unordered_map<K, V>"),
            ("std::unordered_map<Durin::DObject*, float>", "[DHT-CONT004] DPROPERTY 'Value' at line 17: Map key type 'Durin::DObject*' is unsupported"),
            ("std::vector<std::vector<std::vector<std::vector<std::vector<float>>>>>", "[DHT-CONT005] DPROPERTY 'Value' at line 17: container nesting exceeds the supported depth of 4"),
            ("std::vector<std::vector<std::vector<std::vector<std::vector<Durin::TSoftObjectPtr<Durin::DObject>>>>>>", "[DHT-CONT005] DPROPERTY 'Value' at line 17: container nesting exceeds the supported depth of 4"),
        ],
    )
    def test_unsupported_container_forms_have_stable_diagnostics(self, spelling, diagnostic):
        with pytest.raises(ValueError, match=re.escape(diagnostic)):
            _validate_explicit_container_spelling(spelling, "Value", 17)

    def test_unsupported_container_diagnostic_precedes_cpp_generation(self):
        header = "Public/UnsupportedContainer.h"
        header_path = self.module_dir / header
        header_path.write_text(
            '''#pragma once
namespace std { template<typename T> class vector {}; }
namespace Fixture
{
    DSTRUCT()
    struct FUnsupportedContainer
    {
        GENERATED_BODY()
        DPROPERTY()
        std::vector<bool> Bits;
    };
}
''',
            encoding="utf-8",
        )

        with (
            mock.patch.object(configs, "get_module_config", return_value=self.module_config),
            mock.patch.object(configs, "collect_all_dependent_modules", return_value=set()),
            pytest.raises(
                ValueError,
                match=re.escape(
                    "[DHT-CONT002] DPROPERTY 'Bits' at line 10: std::vector<bool> proxy references are unsupported"
                ),
            ),
        ):
            parse_reflection_header("Fixture", header, exported_symbols=self.symbols)

    def test_generated_containers_use_typed_params_and_reusable_resolvers(self):
        assert "FArrayPropertyHelper" not in self.generated_cpp
        assert "FMapPropertyHelper" not in self.generated_cpp
        assert "_ArrayNum(" not in self.generated_cpp
        assert "_MapGetKey(" not in self.generated_cpp
        assert "InitializePropertyValue<std::vector" not in self.generated_cpp
        assert "DestroyPropertyValue<std::vector" not in self.generated_cpp
        assert "&Durin::ResolveArrayOps<" in self.generated_cpp
        assert "&Durin::ResolveMapOps<" in self.generated_cpp

    def test_default_double_vector_intrinsics_are_available(self):
        missing_export = self.temp_root / "missing.export"
        with (
            mock.patch.object(utils, "get_module_export_file_path", return_value=missing_export),
            mock.patch.object(configs, "collect_all_dependent_modules", return_value=set()),
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

        assert data["SchemaVersion"] == 6
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
        assert data["GeneratedOutputDigests"] == {}
        actor_dependencies = data["ResolvedSymbolDependencies"][self.header]
        assert actor_dependencies["Durin::DObject"]["GeneratedHelperName"] == "Z_Construct_DClass_Durin_DObject"
