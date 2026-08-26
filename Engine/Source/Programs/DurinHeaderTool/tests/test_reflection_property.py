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
    ReflectedPropertyInfo,
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
    _validate_weak_object_spelling,
    _validate_property_legacy_names,
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
    @pytest.mark.parametrize("spelling", ["uint32", "::uint32"])
    def test_fixed_width_integer_spellings_use_global_canonical_type(self, spelling):
        prop = _make_property_from_spelling("Value", spelling, None)

        assert prop is not None
        assert prop.kind == "UInt32"
        assert prop.element_size == "sizeof(::uint32)"
        assert property_parser._cpp_type_spelling(spelling, None) == "::uint32"

    def test_property_legacy_names_are_generated_as_first_class_descriptor_data(self):
        statics = "Z_Construct_DClass_ASampleActor_Statics"
        assert (
            f"const char* const {statics}::NewProp_RenamedValue_LegacyNames[] = "
            '{ "OldRenamedValue", "OlderRenamedValue" };'
        ) in self.generated_cpp
        definition = next(
            line for line in self.generated_cpp.splitlines()
            if f"{statics}::NewProp_RenamedValue =" in line
        )
        assert "Durin::DurinCodeGen::WithLegacyNames(" in definition
        assert f"{statics}::NewProp_RenamedValue_LegacyNames, 2" in definition


    @pytest.mark.parametrize(
        ("properties", "diagnostic"),
        [
            (
                [
                    ReflectedPropertyInfo("Current", "float", "Float", legacy_names=["Current"])
                ],
                "LegacyNames must not contain the current property name",
            ),
            (
                [
                    ReflectedPropertyInfo("First", "float", "Float", legacy_names=["Second"]),
                    ReflectedPropertyInfo("Second", "float", "Float"),
                ],
                "collides with a current property name",
            ),
            (
                [
                    ReflectedPropertyInfo("First", "float", "Float", legacy_names=["Old"]),
                    ReflectedPropertyInfo("Second", "float", "Float", legacy_names=["Old"]),
                ],
                "share legacy name 'Old'",
            ),
        ],
    )
    def test_property_legacy_name_collisions_are_rejected(self, properties, diagnostic):
        with pytest.raises(ValueError, match=re.escape(diagnostic)):
            _validate_property_legacy_names("Fixture::FOwner", properties)

    def test_ambiguous_source_property_symbols_are_not_selected_by_insertion_order(self):
        symbols = {
            "Beta::FData": ExportedSymbolInfo(
                Kind="struct",
                ShortName="FData",
                Namespace="Beta",
                QualifiedName="Beta::FData",
                Header="Beta.h",
                API="BETA_API",
            ),
            "Alpha::FData": ExportedSymbolInfo(
                Kind="struct",
                ShortName="FData",
                Namespace="Alpha",
                QualifiedName="Alpha::FData",
                Header="Alpha.h",
                API="ALPHA_API",
            ),
            "Beta::EMode": ExportedSymbolInfo(
                Kind="enum",
                ShortName="EMode",
                Namespace="Beta",
                QualifiedName="Beta::EMode",
                Header="Beta.h",
                API="BETA_API",
                UnderlyingSize=4,
            ),
            "Alpha::EMode": ExportedSymbolInfo(
                Kind="enum",
                ShortName="EMode",
                Namespace="Alpha",
                QualifiedName="Alpha::EMode",
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
            Header="Alpha.h",
            API="ALPHA_API",
        )
        enum_symbol = ExportedSymbolInfo(
            Kind="enum",
            ShortName="EMode",
            Namespace="Alpha",
            QualifiedName="Alpha::EMode",
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
            '"Value", Durin::EPropertyFlags::Edit | Durin::EPropertyFlags::ReadOnly,'
            in self.generated_cpp
        )
        assert 'NewProp_Color_MetaData[] = { { "HideAlpha", "true" } };' in self.generated_cpp
        color_definition = next(
            line for line in self.generated_cpp.splitlines()
            if "::NewProp_Color =" in line
        )
        assert (
            'NewProp_Color = { "Color", Durin::EPropertyFlags::Edit, 1, '
            'static_cast<::uint16>(STRUCT_OFFSET(Fixture::ASampleActor, Color)), '
            '::Durin::Z_Construct_DStruct_FLinearColor, '
            'Z_Construct_DClass_ASampleActor_Statics::NewProp_Color_MetaData, 1 };'
        ) in color_definition
        assert "sizeof(" not in color_definition
        assert "alignof(" not in color_definition
        assert "EPropertyGenFlags::Struct" not in color_definition
        assert "InitializePropertyValue" not in color_definition

        assert (
            '"EditorDiagnostic", Durin::EPropertyFlags::EditorOnly, 1,'
            in self.generated_cpp
        )
        assert "DestroyPropertyValue" not in color_definition
        assert "nullptr" not in color_definition


    def test_typed_property_metadata_is_parsed_and_generated_exactly(self):
        actor = next(info for info in self.header_info.classes if info.short_name == "ASampleActor")
        prop = next(item for item in actor.properties if item.name == "Value")
        metadata = prop.typed_metadata
        assert metadata is not None
        assert (metadata.display_name, metadata.tooltip, metadata.category) == (
            "Scalar Value", "Authored scalar", "Numbers"
        )
        assert (metadata.units, metadata.numeric_kind, metadata.precision) == ("Percent", "Float", 3)
        assert (metadata.step, metadata.clamp_min, metadata.clamp_max, metadata.ui_min, metadata.ui_max) == (
            "1", "-100", "100", "-50", "50"
        )
        statics = "Z_Construct_DClass_ASampleActor_Statics"
        assert f"const Durin::FPropertyMetadataParams {statics}::NewProp_Value_TypedMetaData" in self.generated_cpp
        assert "FPropertyMetadataNumber::FromFloat(-100.0f)" in self.generated_cpp
        assert "WithTypedMetadata(Durin::DurinCodeGen::FFloatPropertyParams" in self.generated_cpp


    @pytest.mark.parametrize(
        ("annotation", "diagnostic"),
        [
            ('DPROPERTY, Step = "1"', "numeric metadata requires the Edit property flag"),
            ('DPROPERTY, Edit, Step = "nan"', "Step must be finite"),
            ('DPROPERTY, Edit, Step = "0"', "Step must be positive"),
            ('DPROPERTY, Edit, ClampMin = "2", ClampMax = "1"', "ClampMin exceeds ClampMax"),
            ('DPROPERTY, Edit, Units = "Pixels"', "unknown Units value 'Pixels'"),
            ('DPROPERTY, Edit, Precision = 10', "Precision exceeds the Float limit of 9"),
            ('DPROPERTY, Edit, Step = "1e-1000"', "Step is outside the float range"),
            ('DPROPERTY, Edit, DisplayName = "A", DisplayName = "B"', "duplicate metadata key 'DisplayName'"),
            ('DPROPERTY, Edit, DisplayName = "A", MetaData = "DisplayName=B"', "cannot also be declared through MetaData"),
        ],
    )
    def test_invalid_typed_metadata_has_stable_diagnostics(self, annotation, diagnostic):
        prop = ReflectedPropertyInfo("Value", "float", "Float", flags="Durin::EPropertyFlags::Edit")
        if annotation == 'DPROPERTY, Step = "1"':
            prop.flags = "None"
        with pytest.raises(ValueError, match=re.escape(diagnostic)):
            property_parser._apply_property_annotation(prop, annotation, "DPROPERTY 'Value' at line 7")


    @pytest.mark.parametrize(
        ("annotation", "diagnostic"),
        [
            ("DPROPERTY, Editoronly", "unknown property specifier 'Editoronly'"),
            ("DPROPERTY, EditorOnly, EditorOnly", "duplicate property specifier 'EditorOnly'"),
        ],
    )
    def test_invalid_property_specifiers_have_stable_diagnostics(self, annotation, diagnostic):
        prop = ReflectedPropertyInfo("Value", "float", "Float")
        with pytest.raises(ValueError, match=re.escape(diagnostic)):
            property_parser._apply_property_annotation(
                prop, annotation, "DPROPERTY 'Value' at line 8"
            )


    def test_integer_metadata_preserves_full_uint64_range(self):
        prop = ReflectedPropertyInfo("Value", "uint64", "UInt64", flags="Durin::EPropertyFlags::Edit")
        property_parser._apply_property_annotation(
            prop,
            'DPROPERTY, Edit, ClampMin = "0", ClampMax = "18446744073709551615"',
            "DPROPERTY 'Value' at line 9",
        )
        assert prop.typed_metadata is not None
        assert prop.typed_metadata.clamp_max == "18446744073709551615"

    def test_integer_metadata_canonicalizes_leading_zeroes(self):
        prop = ReflectedPropertyInfo("Value", "uint64", "UInt64", flags="Durin::EPropertyFlags::Edit")
        property_parser._apply_property_annotation(
            prop, 'DPROPERTY, Edit, Step = "0008"', "DPROPERTY 'Value' at line 10"
        )
        assert prop.typed_metadata is not None
        assert prop.typed_metadata.step == "8"

    def test_deprecated_property_route_is_explicit_and_generated(self):
        actor = next(info for info in self.header_info.classes if info.short_name == "ASampleActor")
        prop = next(item for item in actor.properties if item.name == "Value_DEPRECATED")
        assert prop.flags == "Durin::EPropertyFlags::Deprecated"
        assert prop.deprecation is not None
        assert prop.deprecation.custom_version_type == "FFixtureVersion"
        assert prop.deprecation.deprecated_before == "FFixtureVersion::FloatValue"
        assert prop.deprecation.historical_name == "Value"
        assert prop.deprecation.migrates_to == ["Value"]
        assert "FFixtureVersion::Guid" in self.generated_cpp
        assert "FFixtureVersion::LatestVersion" in self.generated_cpp
        assert "static_cast<::int32>(FFixtureVersion::FloatValue)" in self.generated_cpp
        assert "static_cast<::int32>(FFixtureVersion::LatestVersion)" in self.generated_cpp
        assert 'WithDeprecation(Durin::DurinCodeGen::FInt32PropertyParams' in self.generated_cpp

    @pytest.mark.parametrize(
        ("name", "annotation", "diagnostic"),
        [
            ("Old", 'DPROPERTY, Deprecated, CustomVersion = FVersion, DeprecatedBefore = FVersion::V1, MigratesTo = "Value"', "must end in _DEPRECATED"),
            ("Old_DEPRECATED", 'DPROPERTY, CustomVersion = FVersion', "require the Deprecated specifier"),
            ("Old_DEPRECATED", 'DPROPERTY, Deprecated, CustomVersion = FVersion, DeprecatedBefore = FVersion::V1', "Deprecated requires MigratesTo"),
            ("Old_DEPRECATED", 'DPROPERTY, Deprecated, Edit, CustomVersion = FVersion, DeprecatedBefore = FVersion::V1, MigratesTo = "Value"', "cannot be combined with Edit or Transient"),
        ],
    )
    def test_invalid_deprecated_routes_have_stable_diagnostics(self, name, annotation, diagnostic):
        prop = ReflectedPropertyInfo(name, "int32", "Int32", flags=property_parser._property_flags_from_annotation(annotation))
        with pytest.raises(ValueError, match=re.escape(diagnostic)):
            property_parser._apply_property_annotation(prop, annotation, f"DPROPERTY '{name}' at line 12")


    def test_fname_property_is_generated(self):
        assert "Durin::DurinCodeGen::FNamePropertyParams NewProp_Identifier" in self.generated_cpp
        definition = next(
            line for line in self.generated_cpp.splitlines()
            if "ASampleActor_Statics::NewProp_Identifier =" in line
        )
        assert definition.endswith(
            'NewProp_Identifier = { "Identifier", Durin::EPropertyFlags::Edit, 1, '
            'static_cast<::uint16>(STRUCT_OFFSET(Fixture::ASampleActor, Identifier)) };'
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
            'static_cast<::uint16>(STRUCT_OFFSET(Fixture::ASampleActor, PersistentId)) };'
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
        statics = "Z_Construct_DClass_ASampleActor_Statics"
        expected_suffixes = {
            "Value": (
                'NewProp_Value = { "Value", Durin::EPropertyFlags::Edit | Durin::EPropertyFlags::ReadOnly, 1, '
                'static_cast<::uint16>(STRUCT_OFFSET(Fixture::ASampleActor, Value)) };'
            ),
            "Weights": (
                'NewProp_Weights = { "Weights", Durin::EPropertyFlags::None, 2, '
                'static_cast<::uint16>(STRUCT_OFFSET(Fixture::ASampleActor, Weights)) };'
            ),
            "Mode": (
                'NewProp_Mode = { "Mode", Durin::EPropertyFlags::None, 1, '
                'static_cast<::uint16>(STRUCT_OFFSET(Fixture::ASampleActor, Mode)), '
                '::Fixture::Z_Construct_DEnum_EFixtureMode };'
            ),
            "RawReference": (
                'NewProp_RawReference = Durin::DurinCodeGen::FObjectPropertyParams::Raw<Durin::DObject>('
                '"RawReference", Durin::EPropertyFlags::None, 1, '
                'static_cast<::uint16>(STRUCT_OFFSET(Fixture::ASampleActor, RawReference)), '
                '::Durin::Z_Construct_DClass_DObject);'
            ),
            "StrongReference": (
                'NewProp_StrongReference = Durin::DurinCodeGen::FObjectPropertyParams::ObjectPtr<Durin::DObject>('
                '"StrongReference", Durin::EPropertyFlags::None, 1, '
                'static_cast<::uint16>(STRUCT_OFFSET(Fixture::ASampleActor, StrongReference)), '
                '::Durin::Z_Construct_DClass_DObject);'
            ),
        }
        definitions = {}
        for name, suffix in expected_suffixes.items():
            definition = next(
                line for line in self.generated_cpp.splitlines()
                if f"{statics}::NewProp_{name} =" in line
            )
            if name == "Value":
                assert "WithTypedMetadata(" in definition
                assert (
                    '"Value", Durin::EPropertyFlags::Edit | Durin::EPropertyFlags::ReadOnly, 1, '
                    'static_cast<::uint16>(STRUCT_OFFSET(Fixture::ASampleActor, Value))'
                ) in definition
            else:
                assert definition.endswith(suffix)
            definitions[name] = definition
        label = next(
            line for line in self.generated_cpp.splitlines()
            if f"{statics}::NewProp_Label =" in line
        )
        assert label.endswith(
            f'NewProp_Label = {{ "Label", Durin::EPropertyFlags::Edit, 1, '
            f'static_cast<::uint16>(STRUCT_OFFSET(Fixture::ASampleActor, Label)), '
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
            assert definition.endswith("Z_Construct_DStruct_FVector3 };")
            assert "nullptr" not in definition
            assert "sizeof(" not in definition
            assert "alignof(" not in definition
            assert "EPropertyGenFlags::Struct" not in definition

        float_definition = next(
            line for line in self.generated_cpp.splitlines()
            if "::NewProp_CompactTangent =" in line
        )
        assert float_definition.endswith("Z_Construct_DStruct_FVector3f };")


    def test_all_struct_property_forms_use_concise_typed_registration(self):
        statics = "Z_Construct_DStruct_FStructPropertyShapes_Statics"
        expected_definitions = {
            "Direct": (
                'NewProp_Direct = { "Direct", Durin::EPropertyFlags::None, 1, '
                'static_cast<::uint16>(STRUCT_OFFSET(Fixture::FStructPropertyShapes, Direct)), '
                '::Fixture::Z_Construct_DStruct_FTrivialOps };'
            ),
            "DeletedDefault": (
                'NewProp_DeletedDefault = { "DeletedDefault", Durin::EPropertyFlags::None, 1, '
                'static_cast<::uint16>(STRUCT_OFFSET(Fixture::FStructPropertyShapes, DeletedDefault)), '
                '::Fixture::Z_Construct_DStruct_FDeletedDefault };'
            ),
            "DeletedCopy": (
                'NewProp_DeletedCopy = { "DeletedCopy", Durin::EPropertyFlags::None, 1, '
                'static_cast<::uint16>(STRUCT_OFFSET(Fixture::FStructPropertyShapes, DeletedCopy)), '
                '::Fixture::Z_Construct_DStruct_FMoveOnly };'
            ),
            "NonTrivialDestructor": (
                'NewProp_NonTrivialDestructor = { "NonTrivialDestructor", Durin::EPropertyFlags::None, 1, '
                'static_cast<::uint16>(STRUCT_OFFSET(Fixture::FStructPropertyShapes, NonTrivialDestructor)), '
                '::Fixture::Z_Construct_DStruct_FNonTrivialDestructor };'
            ),
            "Metadata": (
                'NewProp_Metadata = { "Metadata", Durin::EPropertyFlags::Edit, 1, '
                'static_cast<::uint16>(STRUCT_OFFSET(Fixture::FStructPropertyShapes, Metadata)), '
                f'::Fixture::Z_Construct_DStruct_FTrivialOps, {statics}::NewProp_Metadata_MetaData, 1 }};'
            ),
            "Fixed": (
                'NewProp_Fixed = { "Fixed", Durin::EPropertyFlags::None, 3, '
                'static_cast<::uint16>(STRUCT_OFFSET(Fixture::FStructPropertyShapes, Fixed)), '
                '::Fixture::Z_Construct_DStruct_FTrivialOps };'
            ),
            "ArrayValues_Inner": (
                'NewProp_ArrayValues_Inner = { "ArrayValues_Inner", Durin::EPropertyFlags::None, 1, 0, '
                '::Fixture::Z_Construct_DStruct_FDeletedDefault };'
            ),
            "MapByStruct_Key": (
                'NewProp_MapByStruct_Key = { "MapByStruct_Key", Durin::EPropertyFlags::None, 1, 0, '
                '::Fixture::Z_Construct_DStruct_FMoveOnly };'
            ),
            "MapToStruct_Value": (
                'NewProp_MapToStruct_Value = { "MapToStruct_Value", Durin::EPropertyFlags::None, 1, 0, '
                '::Fixture::Z_Construct_DStruct_FDeletedDefault };'
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
            assert definition.lstrip() == (
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
        assert inner_definition.endswith("Z_Construct_DStruct_FDeletedDefault };")
        assert "nullptr" not in inner_definition
        assert "sizeof(" not in inner_definition
        assert "alignof(" not in inner_definition
        assert "EPropertyGenFlags::Struct" not in inner_definition
        assert "InitializePropertyValue" not in inner_definition
        assert "DestroyPropertyValue" not in inner_definition


    def test_container_spelling_contract_and_alias_resolution(self):
        symbols = self.symbols
        byte = _make_property_from_spelling("Value", "std::byte", symbols)
        blob = _make_property_from_spelling("Data", "std::vector<std::byte>", symbols)
        assert byte is not None and byte.kind == "Byte"
        assert blob is not None and blob.kind == "Blob" and blob.inner is None
        bulk = _make_property_from_spelling(
            "Payload", "Durin::Asset::FEditorBulkData", symbols
        )
        assert bulk is not None and bulk.kind == "BulkData" and bulk.inner is None
        assert _make_property_from_spelling(
            "Nested", "std::vector<std::vector<std::byte>>", symbols
        ) is None

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
            "DirectByte",
            "FixedBytes",
            "Blob",
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

        direct_byte = next(line for line in self.generated_cpp.splitlines()
            if "AContainerShapes_Statics::NewProp_DirectByte =" in line)
        fixed_bytes = next(line for line in self.generated_cpp.splitlines()
            if "AContainerShapes_Statics::NewProp_FixedBytes =" in line)
        blob_definition = next(line for line in self.generated_cpp.splitlines()
            if "AContainerShapes_Statics::NewProp_Blob =" in line)
        assert "FBytePropertyParams" in direct_byte
        assert "FBytePropertyParams" in fixed_bytes
        assert "FBlobPropertyParams" in blob_definition

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
        assert direct_definition.lstrip() == (
            "const Durin::DurinCodeGen::FSoftObjectPropertyParams "
            "Z_Construct_DClass_AContainerShapes_Statics::NewProp_SoftReference = "
            "Durin::DurinCodeGen::FSoftObjectPropertyParams::Create<"
            "std::remove_extent_t<decltype(((Fixture::AContainerShapes*)0)->SoftReference)>>("
            '\"SoftReference\", Durin::EPropertyFlags::Edit, 1, '
            "static_cast<::uint16>(STRUCT_OFFSET(Fixture::AContainerShapes, SoftReference)), "
            "::Durin::Z_Construct_DClass_DObject);"
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


    def test_weak_objects_require_transient_and_emit_typed_metadata(self):
        shape_info = next(info for info in self.header_info.classes if info.short_name == "AContainerShapes")
        properties = {prop.name: prop for prop in shape_info.properties}
        assert properties["WeakReference"].kind == "WeakObject"
        assert properties["WeakReference"].referenced_type == "Durin::DObject"
        assert properties["WeakFixed"].array_dim == 2
        assert properties["WeakReferences"].inner.kind == "WeakObject"
        assert properties["WeakValues"].value.kind == "WeakObject"
        nested_info = next(info for info in self.header_info.structs if info.short_name == "FWeakNested")
        assert nested_info.properties[0].name == "Target"
        assert nested_info.properties[0].kind == "WeakObject"
        assert properties["WeakNested"].kind == "Struct"
        direct_definition = next(
            line for line in self.generated_cpp.splitlines()
            if "AContainerShapes_Statics::NewProp_WeakReference =" in line
        )
        assert "FWeakObjectPropertyParams::Create<" in direct_definition
        assert "Durin::EPropertyFlags::Transient" in direct_definition
        assert "::Durin::Z_Construct_DClass_DObject" in direct_definition


    @pytest.mark.parametrize(
        ("spelling", "diagnostic"),
        [
            ("Durin::FWeakObjectPtr", "[DHT-WEAK001] DPROPERTY 'Value' at line 23: raw FWeakObjectPtr is unsupported; use TWeakObjectPtr<ReflectedObjectClass>"),
            ("Durin::TWeakObjectPtr", "[DHT-WEAK001] DPROPERTY 'Value' at line 23: TWeakObjectPtr requires exactly one reflected object class"),
            ("const Durin::TWeakObjectPtr<Durin::DObject>", "[DHT-WEAK003] DPROPERTY 'Value' at line 23: weak object properties do not support cv-qualifiers, pointers, or references"),
            ("Durin::TWeakObjectPtr<Fixture::FTrivialOps>", "[DHT-WEAK004] DPROPERTY 'Value' at line 23: weak object target 'Fixture::FTrivialOps' is not an object class"),
            ("Durin::TWeakObjectPtr<Fixture::DMissing>", "[DHT-WEAK005] DPROPERTY 'Value' at line 23: weak object target 'Fixture::DMissing' could not be resolved"),
            ("std::unordered_map<Durin::TWeakObjectPtr<Durin::DObject>, float>", "[DHT-WEAK006] DPROPERTY 'Value' at line 23: weak object references are unsupported as Map keys"),
        ],
    )
    def test_unsupported_weak_object_forms_have_stable_diagnostics(self, spelling, diagnostic):
        with pytest.raises(ValueError, match=re.escape(diagnostic)):
            _validate_weak_object_spelling(spelling, "Value", 23, self.symbols)


    def test_non_transient_weak_property_is_rejected(self):
        prop = ReflectedPropertyInfo(
            name="Value", type_name="Durin::TWeakObjectPtr<Durin::DObject>",
            kind="WeakObject", referenced_type="Durin::DObject",
        )
        with pytest.raises(ValueError, match="DHT-WEAK007"):
            property_parser._apply_property_annotation(prop, "DPROPERTY()", "DPROPERTY 'Value'")


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


    def test_default_vector_intrinsics_are_available(self):
        missing_export = self.temp_root / "missing.export"
        with (
            mock.patch.object(utils, "get_module_export_file_path", return_value=missing_export),
            mock.patch.object(configs, "collect_all_dependent_modules", return_value=set()),
            mock.patch.object(configs, "collect_all_dependent_module_with_export_file", return_value=[]),
        ):
            symbols = load_available_symbols("Fixture")

        for type_name in (
            "FVector2f", "FVector3f", "FVector4f",
            "FVector2", "FVector3", "FVector4",
            "FVector2d", "FVector3d", "FVector4d",
            "FQuatf", "FQuatd", "FQuat", "FMatrix4f",
        ):
            qualified_name = f"Durin::{type_name}"
            assert qualified_name in symbols
        expected_helpers = {
            "FVector2d": "FVector2", "FVector3d": "FVector3", "FVector4d": "FVector4",
            "FQuatd": "FQuat",
        }
        for type_name in (
            "FVector2f", "FVector3f", "FVector4f",
            "FVector2", "FVector3", "FVector4",
            "FVector2d", "FVector3d", "FVector4d",
            "FQuatf", "FQuatd", "FQuat", "FMatrix4f",
        ):
            helper_type = expected_helpers.get(type_name, type_name)
            assert symbols[f"Durin::{type_name}"].generated_symbol.helper_reference == f"::Durin::Z_Construct_DStruct_{helper_type}"

        for source_type, helper_type in expected_helpers.items():
            prop = _make_property_from_spelling("Value", f"Durin::{source_type}", symbols)
            assert prop is not None
            assert prop.kind == "Struct"
            assert symbols[prop.referenced_struct_type].generated_symbol.helper_reference == f"::Durin::Z_Construct_DStruct_{helper_type}"

        for source_type in ("FVector3d", "FQuatf", "FMatrix4f"):
            qualified_name = f"Durin::{source_type}"
            direct = _make_property_from_spelling("Direct", qualified_name, symbols, array_dim=3)
            array = _make_property_from_spelling("Values", f"std::vector<{qualified_name}>", symbols)
            value_map = _make_property_from_spelling(
                "Named", f"std::unordered_map<std::string, {qualified_name}>", symbols)
            assert direct is not None and direct.kind == "Struct" and direct.array_dim == 3
            assert array is not None and array.kind == "Array" and array.inner is not None
            assert array.inner.kind == "Struct"
            assert value_map is not None and value_map.kind == "Map" and value_map.value is not None
            assert value_map.value.kind == "Struct"
