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
from durin_header_tool.model.reflection_info import make_generated_helper_name


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

        self.assertEqual(data["SchemaVersion"], 2)
        actor = data["Symbols"]["Durin::AActor"]
        self.assertEqual(actor["QualifiedName"], "Durin::AActor")
        self.assertEqual(actor["GeneratedHelperName"], "Z_Construct_DClass_Durin_AActor")
        self.assertEqual(actor["BaseQualifiedName"], "Durin::DObject")

    def test_qualified_helper_name_and_validation(self):
        self.assertEqual(
            make_generated_helper_name("Durin::Gameplay::AActor"),
            "Z_Construct_DClass_Durin_Gameplay_AActor",
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
        self.assertIn("FStringPropertyParams NewProp_DisplayName", content)
        self.assertIn("FArrayPropertyParams NewProp_Scores", content)
        self.assertIn("FInt32PropertyParams NewProp_Scores_Inner", content)
        self.assertIn("FArrayPropertyParams NewProp_ObjectRefs", content)
        self.assertIn("FObjectPropertyParams NewProp_ObjectRefs_Inner", content)
        self.assertIn("FMapPropertyParams NewProp_NamedScores", content)
        self.assertIn("FStringPropertyParams NewProp_NamedScores_Key", content)
        self.assertIn("FInt32PropertyParams NewProp_NamedScores_Value", content)
        self.assertIn("Durin::DurinCodeGen::EPropertyGenFlags::UInt16", content)
        self.assertIn("Durin::DurinCodeGen::EPropertyGenFlags::Int32", content)
        self.assertIn("Durin::DurinCodeGen::EPropertyGenFlags::Object", content)
        self.assertIn("Durin::DurinCodeGen::EPropertyGenFlags::String", content)
        self.assertIn("Durin::DurinCodeGen::EPropertyGenFlags::Array", content)
        self.assertIn("Durin::DurinCodeGen::EPropertyGenFlags::Map", content)
        self.assertIn("static_cast<Durin::uint16>(STRUCT_OFFSET(Durin::TestDHT, a1))", content)
        self.assertIn("Durin::EPropertyFlags::Edit | Durin::EPropertyFlags::Transient", content)
        self.assertIn("Durin::EPropertyFlags::EditConst", content)
        self.assertIn("static_cast<Durin::uint16>(2), Durin::DurinCodeGen::EPropertyGenFlags::UInt16", content)
        self.assertIn("static_cast<Durin::uint16>(1), Durin::DurinCodeGen::EPropertyGenFlags::UInt8", content)
        self.assertIn("static_cast<Durin::uint16>(8), Durin::DurinCodeGen::EPropertyGenFlags::Object", content)
        self.assertIn("Z_Construct_DClass_Durin_DObject", content)
        self.assertIn("3, static_cast<Durin::uint16>(STRUCT_OFFSET(Durin::TestDHT, a3))", content)
        self.assertIn("Durin::DurinCodeGen::EPropertyGenFlags::Array, nullptr, &Z_Construct_DClass_Durin_TestDHT_Statics::NewProp_Scores_Inner", content)
        self.assertIn("Durin::DurinCodeGen::EPropertyGenFlags::Map, nullptr, nullptr, &Z_Construct_DClass_Durin_TestDHT_Statics::NewProp_NamedScores_Key, &Z_Construct_DClass_Durin_TestDHT_Statics::NewProp_NamedScores_Value", content)
        self.assertNotIn("NewProp_UnsupportedNested", content)
        self.assertNotIn("NewProp_UnsupportedUniqueObjects", content)

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
