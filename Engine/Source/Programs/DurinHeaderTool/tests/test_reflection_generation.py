import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from durin_header_tool import config as configs
from durin_header_tool import io as utils
from durin_header_tool.generators.module_export_file_generator import generate_module_export_file
from durin_header_tool.generators.module_reflection_files_generator import generate_reflection_files
from durin_header_tool.model.reflection_info import make_generated_enum_helper_name, make_generated_helper_name


class ReflectionGenerationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        configs.ARCH = "Win64"
        configs.PROFILE_NAME = "DurinEditor"
        configs.init_configs()

        for module_name in ("CoreDObject", "Engine", "LevelEditor"):
            generate_module_export_file(module_name)
            generate_reflection_files(module_name)

    def test_export_schema_uses_qualified_symbol_identity(self):
        export_path = utils.get_module_export_file_path("Engine")
        data = json.loads(export_path.read_text(encoding="utf-8"))

        self.assertEqual(data["SchemaVersion"], 4)
        actor = data["Symbols"]["Durin::AActor"]
        self.assertEqual(actor["QualifiedName"], "Durin::AActor")
        self.assertEqual(actor["GeneratedHelperName"], "Z_Construct_DClass_Durin_AActor")
        self.assertEqual(actor["BaseQualifiedName"], "Durin::DObject")

        level_editor_export_path = utils.get_module_export_file_path("LevelEditor")
        level_editor_data = json.loads(level_editor_export_path.read_text(encoding="utf-8"))
        test_enum = level_editor_data["Symbols"]["Durin::ETestDHTMode"]
        self.assertEqual(test_enum["Kind"], "enum")
        self.assertEqual(test_enum["GeneratedHelperName"], "Z_Construct_DEnum_Durin_ETestDHTMode")
        self.assertTrue(test_enum["IsScoped"])
        self.assertEqual(test_enum["UnderlyingKind"], "UInt8")
        test_struct = level_editor_data["Symbols"]["Durin::FTestDHTStruct"]
        self.assertEqual(test_struct["Kind"], "struct")
        self.assertEqual(test_struct["GeneratedHelperName"], "Z_Construct_DStruct_Durin_FTestDHTStruct")

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

    def test_generated_property_params_for_test_dht(self):
        generated_cpp = utils.get_module_dht_output_dir("LevelEditor") / "TestDHT.gen.cpp"
        content = generated_cpp.read_text(encoding="utf-8")

        self.assertIn("FUInt16PropertyParams NewProp_a1", content)
        self.assertIn("FInt32PropertyParams NewProp_a2", content)
        self.assertIn("FUInt8PropertyParams NewProp_a3", content)
        self.assertIn("FObjectPropertyParams NewProp_ObjectRef", content)
        self.assertIn("FObjectPropertyParams NewProp_ObjectPtrRef", content)
        self.assertIn("FStringPropertyParams NewProp_DisplayName", content)
        self.assertIn("FEnumPropertyParams NewProp_Mode", content)
        self.assertIn("FStructPropertyParams NewProp_StructValue", content)
        self.assertIn("Durin::DurinCodeGen::ConstructDStruct", content)
        self.assertIn("Z_Construct_DStruct_Durin_FTestDHTStruct", content)
        self.assertIn("FArrayPropertyParams NewProp_Scores", content)
        self.assertIn("FArrayPropertyParams NewProp_Modes", content)
        self.assertIn("FEnumPropertyParams NewProp_Modes_Inner", content)
        self.assertIn("FInt32PropertyParams NewProp_Scores_Inner", content)
        self.assertIn("FArrayPropertyParams NewProp_ObjectRefs", content)
        self.assertIn("FObjectPropertyParams NewProp_ObjectRefs_Inner", content)
        self.assertIn("FArrayPropertyParams NewProp_ObjectPtrRefs", content)
        self.assertIn("FObjectPropertyParams NewProp_ObjectPtrRefs_Inner", content)
        self.assertIn("FArrayPropertyHelper NewProp_ObjectPtrRefs_ArrayHelper", content)
        self.assertIn("NewProp_ObjectPtrRefs_ArrayNum(const void* Container)", content)
        self.assertIn("static_cast<const std::vector<Durin::TObjectPtr<Durin::DObject>>*>(Container)", content)
        self.assertIn("FMapPropertyParams NewProp_NamedScores", content)
        self.assertIn("FMapPropertyHelper NewProp_NamedScores_MapHelper", content)
        self.assertIn("NewProp_NamedScores_MapInsert", content)
        self.assertIn("FStringPropertyParams NewProp_NamedScores_Key", content)
        self.assertIn("FInt32PropertyParams NewProp_NamedScores_Value", content)
        self.assertIn("FMapPropertyParams NewProp_NamedModes", content)
        self.assertIn("FStringPropertyParams NewProp_NamedModes_Key", content)
        self.assertIn("FEnumPropertyParams NewProp_NamedModes_Value", content)
        self.assertIn("FMapPropertyParams NewProp_ModeScores", content)
        self.assertIn("FEnumPropertyParams NewProp_ModeScores_Key", content)
        self.assertIn("FInt32PropertyParams NewProp_ModeScores_Value", content)
        self.assertIn("FArrayPropertyParams NewProp_NestedScores", content)
        self.assertIn("FArrayPropertyParams NewProp_NestedScores_Inner", content)
        self.assertIn("FInt32PropertyParams NewProp_NestedScores_Inner_Inner", content)
        self.assertIn("FArrayPropertyHelper NewProp_NestedScores_ArrayHelper", content)
        self.assertIn("FArrayPropertyHelper NewProp_NestedScores_Inner_ArrayHelper", content)
        self.assertIn("FArrayPropertyParams NewProp_ObjectMapList", content)
        self.assertIn("FMapPropertyParams NewProp_ObjectMapList_Inner", content)
        self.assertIn("FStringPropertyParams NewProp_ObjectMapList_Inner_Key", content)
        self.assertIn("FObjectPropertyParams NewProp_ObjectMapList_Inner_Value", content)
        self.assertIn("FMapPropertyParams NewProp_ObjectPtrMap", content)
        self.assertIn("FObjectPropertyParams NewProp_ObjectPtrMap_Value", content)
        self.assertIn("FMapPropertyParams NewProp_ScoreGroups", content)
        self.assertIn("FArrayPropertyParams NewProp_ScoreGroups_Value", content)
        self.assertIn("FInt32PropertyParams NewProp_ScoreGroups_Value_Inner", content)
        self.assertIn("Durin::DurinCodeGen::EPropertyGenFlags::UInt16", content)
        self.assertIn("Durin::DurinCodeGen::EPropertyGenFlags::Int32", content)
        self.assertIn("Durin::DurinCodeGen::EPropertyGenFlags::Object", content)
        self.assertIn("Durin::DurinCodeGen::EPropertyGenFlags::String", content)
        self.assertIn("Durin::DurinCodeGen::EPropertyGenFlags::Enum", content)
        self.assertIn("Durin::DurinCodeGen::EPropertyGenFlags::Array", content)
        self.assertIn("Durin::DurinCodeGen::EPropertyGenFlags::Map", content)
        self.assertIn("Durin::DurinCodeGen::FEnumValueParams EnumValues", content)
        self.assertIn("Durin::DurinCodeGen::FEnumParams EnumParams", content)
        self.assertIn("Durin::DurinCodeGen::ConstructDEnum", content)
        self.assertIn("Z_Construct_DEnum_Durin_ETestDHTMode", content)
        self.assertIn("static_cast<Durin::uint16>(STRUCT_OFFSET(Durin::TestDHT, a1))", content)
        self.assertIn("Durin::EPropertyFlags::Edit | Durin::EPropertyFlags::Transient", content)
        self.assertIn("Durin::EPropertyFlags::EditConst", content)
        self.assertIn("static_cast<Durin::uint16>(2), Durin::DurinCodeGen::EPropertyGenFlags::UInt16", content)
        self.assertIn("static_cast<Durin::uint16>(1), Durin::DurinCodeGen::EPropertyGenFlags::UInt8", content)
        self.assertIn("static_cast<Durin::uint16>(8), Durin::DurinCodeGen::EPropertyGenFlags::Object", content)
        self.assertIn("sizeof(Durin::TObjectPtr<Durin::DObject>)), Durin::DurinCodeGen::EPropertyGenFlags::Object", content)
        self.assertIn("NewProp_ObjectPtrRef = { \"ObjectPtrRef\"", content)
        self.assertIn("Durin::DurinCodeGen::EPropertyGenFlags::Object, Z_Construct_DClass_Durin_DObject, nullptr, nullptr, nullptr, nullptr, true, nullptr", content)
        self.assertIn("Z_Construct_DClass_Durin_DObject", content)
        self.assertIn("3, static_cast<Durin::uint16>(STRUCT_OFFSET(Durin::TestDHT, a3))", content)
        self.assertIn("NewProp_Mode = { \"Mode\"", content)
        self.assertIn("Durin::DurinCodeGen::EPropertyGenFlags::Enum, nullptr, Z_Construct_DEnum_Durin_ETestDHTMode", content)
        self.assertIn("NewProp_ModeScores_Key = { \"ModeScores_Key\"", content)
        self.assertNotIn("Durin::DClass* Z_Construct_DEnum_Durin_ETestDHTMode", content)
        self.assertIn("Durin::DurinCodeGen::EPropertyGenFlags::Array, nullptr, nullptr, &Z_Construct_DClass_Durin_TestDHT_Statics::NewProp_Scores_Inner", content)
        self.assertIn("false, &Z_Construct_DClass_Durin_TestDHT_Statics::NewProp_Scores_ArrayHelper", content)
        self.assertIn("Durin::DurinCodeGen::EPropertyGenFlags::Map, nullptr, nullptr, nullptr, &Z_Construct_DClass_Durin_TestDHT_Statics::NewProp_NamedScores_Key, &Z_Construct_DClass_Durin_TestDHT_Statics::NewProp_NamedScores_Value", content)
        self.assertIn("Durin::DurinCodeGen::EPropertyGenFlags::Array, nullptr, nullptr, &Z_Construct_DClass_Durin_TestDHT_Statics::NewProp_NestedScores_Inner", content)
        self.assertIn("Durin::DurinCodeGen::EPropertyGenFlags::Array, nullptr, nullptr, &Z_Construct_DClass_Durin_TestDHT_Statics::NewProp_NestedScores_Inner_Inner", content)
        self.assertIn("Durin::DurinCodeGen::EPropertyGenFlags::Map, nullptr, nullptr, nullptr, &Z_Construct_DClass_Durin_TestDHT_Statics::NewProp_ObjectMapList_Inner_Key, &Z_Construct_DClass_Durin_TestDHT_Statics::NewProp_ObjectMapList_Inner_Value", content)
        self.assertIn("Durin::DurinCodeGen::EPropertyGenFlags::Map, nullptr, nullptr, nullptr, &Z_Construct_DClass_Durin_TestDHT_Statics::NewProp_ScoreGroups_Key, &Z_Construct_DClass_Durin_TestDHT_Statics::NewProp_ScoreGroups_Value", content)
        self.assertNotIn("NewProp_UnsupportedTooDeep", content)
        self.assertNotIn("NewProp_UnsupportedObjectKeyMap", content)
        self.assertNotIn("NewProp_UnsupportedUniqueObjects", content)

    def test_generated_types_use_module_cpp_package(self):
        generated_cpp = utils.get_module_dht_output_dir("LevelEditor") / "TestDHT.gen.cpp"
        content = generated_cpp.read_text(encoding="utf-8")

        self.assertIn('"/Cpp/LevelEditor",', content)
        self.assertIn('Singleton->Register(Durin::DStruct::StaticClass, "/Cpp/LevelEditor"', content)
        self.assertIn('Singleton->Register(Durin::DEnum::StaticClass, "/Cpp/LevelEditor"', content)

        module_cpp = utils.get_module_dht_output_dir("LevelEditor") / "LevelEditor.module.gen.cpp"
        module_content = module_cpp.read_text(encoding="utf-8")
        self.assertIn('Durin::RegisterCompiledInPackage("LevelEditor")', module_content)

    def test_manifest_records_generator_contract(self):
        manifest_path = utils.get_module_manifest_file_path("Engine")
        data = json.loads(manifest_path.read_text(encoding="utf-8"))

        self.assertEqual(data["SchemaVersion"], 2)
        self.assertEqual(data["SymbolNameScheme"], "qualified-underscore-v1")
        self.assertEqual(data["ModuleName"], "Engine")
        self.assertEqual(data["Profile"], "DurinEditor")
        self.assertEqual(data["Platform"], "Win64")
        actor_dependencies = data["ResolvedSymbolDependencies"]["Public/Engine/Actor.h"]
        self.assertEqual(actor_dependencies["Durin::DObject"]["GeneratedHelperName"], "Z_Construct_DClass_Durin_DObject")


if __name__ == "__main__":
    unittest.main()
