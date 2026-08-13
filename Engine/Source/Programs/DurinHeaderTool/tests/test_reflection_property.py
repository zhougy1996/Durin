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
class TestReflectionProperties:
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
            property_parser,
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

