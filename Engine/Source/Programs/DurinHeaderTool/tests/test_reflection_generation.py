import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

import configs
import utils
from generators.module_export_file_generator import generate_module_export_file
from generators.module_reflection_files_generator import generate_reflection_files
from models.reflection_info import make_generated_helper_name


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

        self.assertEqual(data["schemaVersion"], 1)
        actor = data["symbols"]["Durin::AActor"]
        self.assertEqual(actor["qualifiedName"], "Durin::AActor")
        self.assertEqual(actor["generatedHelperName"], "Z_Construct_DClass_Durin_AActor")
        self.assertEqual(actor["baseQualifiedName"], "Durin::DObject")

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
        self.assertIn("Durin::DurinCodeGen::EPropertyGenFlags::UInt16", content)
        self.assertIn("Durin::DurinCodeGen::EPropertyGenFlags::Int32", content)
        self.assertIn("static_cast<Durin::uint16>(STRUCT_OFFSET(Durin::TestDHT, a1))", content)

    def test_manifest_records_generator_contract(self):
        manifest_path = utils.get_module_manifest_file_path("Engine")
        data = json.loads(manifest_path.read_text(encoding="utf-8"))

        self.assertEqual(data["schemaVersion"], 1)
        self.assertEqual(data["symbolNameScheme"], "qualified-underscore-v1")
        self.assertEqual(data["moduleName"], "Engine")
        self.assertEqual(data["profile"], "DurinEditor")
        self.assertEqual(data["platform"], "Win64")


if __name__ == "__main__":
    unittest.main()
