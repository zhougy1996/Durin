import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from durin_header_tool import config as configs
from durin_header_tool import io as utils
from durin_header_tool.config.module_config import DurinModuleConfig
from durin_header_tool.extractors.export_symbol_extractor import extract_module_export_info
from durin_header_tool.generators.module_reflection_files_generator import (
    _write_reflection_files,
    make_new_module_manifest,
)
from durin_header_tool.model.export_info import ExportedSymbolInfo, save_module_export_file
from durin_header_tool.model.reflection_manifest import ModuleManifest, save_module_manifest_file
from durin_header_tool.model.reflection_info import (
    ReflectedEnumInfo,
    ReflectedEnumValueInfo,
    make_generated_enum_helper_name,
    make_generated_helper_name,
)
from durin_header_tool.parser.reflection_parser import parse_reflection_header
from durin_header_tool.resolver.reflection_resolver import (
    load_available_symbols,
    resolved_symbol_dependencies_for_header,
)
from durin_header_tool.writers.reflection_source_writer import (
    _enum_definitions,
    generate_cpp_content,
)


class ReflectionSourceWriterTests(unittest.TestCase):
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

        self.assertIn('{ "Negative", static_cast<Durin::uint64>(-1) },', content)
        self.assertIn('{ "High", static_cast<Durin::uint64>(18446744073709551615) },', content)


class ReflectionGenerationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Keep parser/writer integration coverage self-contained; unit tests must not scan production modules.
        cls._temp_dir = tempfile.TemporaryDirectory()
        cls.temp_root = Path(cls._temp_dir.name)
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
    class DObject {};
}

namespace Fixture
{
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

    @classmethod
    def tearDownClass(cls):
        cls._temp_dir.cleanup()

    def test_export_schema_uses_qualified_symbol_identity(self):
        export_path = self.temp_root / "Fixture.export"
        with mock.patch.object(utils, "get_module_export_file_path", return_value=export_path):
            content = save_module_export_file(self.export_info)
        data = json.loads(content)

        self.assertEqual(data["SchemaVersion"], 4)
        actor = data["Symbols"]["Fixture::ASampleActor"]
        self.assertEqual(actor["QualifiedName"], "Fixture::ASampleActor")
        self.assertEqual(actor["GeneratedHelperName"], "Z_Construct_DClass_Fixture_ASampleActor")
        self.assertEqual(actor["BaseQualifiedName"], "Durin::DObject")

    def test_qualified_helper_name_and_validation(self):
        self.assertEqual(
            make_generated_helper_name("Durin::Gameplay::AActor"),
            "Z_Construct_DClass_Durin_Gameplay_AActor",
        )
        self.assertEqual(
            make_generated_enum_helper_name("Durin::Gameplay::ETeam"),
            "Z_Construct_DEnum_Durin_Gameplay_ETeam",
        )
        with self.assertRaises(ValueError):
            make_generated_helper_name("Durin::Gameplay_AActor")

    def test_generated_types_use_module_cpp_package(self):
        self.assertIn('"/Cpp/Fixture",', self.generated_cpp)

        manifest = ModuleManifest(module_name="Fixture")
        with mock.patch.object(utils, "get_module_dht_output_dir", return_value=self.dht_output_dir):
            _write_reflection_files("Fixture", [], {}, manifest, max_workers=1)
        module_content = (self.dht_output_dir / "Fixture.module.gen.cpp").read_text(encoding="utf-8")
        self.assertIn('Durin::RegisterCompiledInPackage("Fixture")', module_content)

    def test_class_display_and_default_object_name_metadata(self):
        self.assertIn('"Fixture::ASampleActor",', self.generated_cpp)
        self.assertIn('"ASampleActor",', self.generated_cpp)
        self.assertIn('3,\n\t"Sample Actor",', self.generated_cpp)
        self.assertIn('"Sample Actor",', self.generated_cpp)
        self.assertIn('"SampleActor"', self.generated_cpp)

    def test_property_flags_metadata_and_value_lifecycle_are_generated(self):
        self.assertIn(
            'NewProp_Value = { "Value", Durin::EPropertyFlags::Edit | Durin::EPropertyFlags::ReadOnly,',
            self.generated_cpp,
        )
        self.assertIn(
            'NewProp_Color_MetaData[] = { { "HideAlpha", "true" } };',
            self.generated_cpp,
        )
        self.assertIn("Z_Construct_DStruct_Durin_FLinearColor", self.generated_cpp)
        self.assertIn(
            'NewProp_Color = { "Color", Durin::EPropertyFlags::Edit,',
            self.generated_cpp,
        )
        value_type = "std::remove_extent_t<decltype(((Fixture::ASampleActor*)0)->Color)>"
        self.assertIn(f"sizeof({value_type})", self.generated_cpp)
        self.assertIn(f"alignof({value_type})", self.generated_cpp)
        self.assertIn(f"InitializePropertyValue<{value_type}>", self.generated_cpp)
        self.assertIn(f"DestroyPropertyValue<{value_type}>", self.generated_cpp)

    def test_fname_property_is_generated(self):
        self.assertIn("Durin::DurinCodeGen::FNamePropertyParams NewProp_Identifier", self.generated_cpp)
        self.assertIn("Durin::DurinCodeGen::EPropertyGenFlags::Name", self.generated_cpp)

    def test_brace_initialized_intrinsic_struct_properties_are_generated(self):
        for property_name in ("Position", "Tangent"):
            self.assertIn(f'NewProp_{property_name} = {{ "{property_name}",', self.generated_cpp)
        self.assertIn("Z_Construct_DStruct_Durin_FVector3", self.generated_cpp)

    def test_default_double_vector_intrinsics_are_available(self):
        missing_export = self.temp_root / "missing.export"
        with (
            mock.patch.object(utils, "get_module_export_file_path", return_value=missing_export),
            mock.patch.object(configs, "collect_all_dependent_module_with_export_file", return_value=[]),
        ):
            symbols = load_available_symbols("Fixture")

        for type_name in ("FVector2", "FVector3", "FVector4"):
            qualified_name = f"Durin::{type_name}"
            self.assertIn(qualified_name, symbols)
            self.assertEqual(symbols[qualified_name].GeneratedHelperName, f"Z_Construct_DStruct_Durin_{type_name}")

    def test_manifest_records_generator_contract(self):
        manifest_path = self.temp_root / "Fixture.manifest"
        with (
            mock.patch.object(configs, "get_module_config", return_value=self.module_config),
            mock.patch.object(configs, "collect_all_dependent_module_with_export_file", return_value=[]),
            mock.patch.object(configs, "ARCH", "Win64"),
            mock.patch.object(configs, "PROFILE_NAME", "DurinEditor"),
            mock.patch.object(configs, "TOOL_FINGERPRINT", "fixture-fingerprint"),
            mock.patch.object(utils, "get_module_manifest_file_path", return_value=manifest_path),
        ):
            manifest = make_new_module_manifest("Fixture")
            manifest.resolved_symbol_dependencies[self.header] = resolved_symbol_dependencies_for_header(self.header_info, self.symbols)
            content = save_module_manifest_file(manifest)
        data = json.loads(content)

        self.assertEqual(data["SchemaVersion"], 3)
        self.assertEqual(data["ToolFingerprint"], "fixture-fingerprint")
        self.assertEqual(data["SymbolNameScheme"], "qualified-underscore-v1")
        self.assertEqual(data["ModuleName"], "Fixture")
        self.assertEqual(data["Profile"], "DurinEditor")
        self.assertEqual(data["Platform"], "Win64")
        actor_dependencies = data["ResolvedSymbolDependencies"][self.header]
        self.assertEqual(actor_dependencies["Durin::DObject"]["GeneratedHelperName"], "Z_Construct_DClass_Durin_DObject")


if __name__ == "__main__":
    unittest.main()
