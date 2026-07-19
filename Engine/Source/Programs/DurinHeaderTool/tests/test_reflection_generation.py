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
        configs.BUILD_IDENTIFIER = "DHTTests"
        configs.TOOL_FINGERPRINT = "dht-tests"
        configs.init_configs()

        for module_name in ("CoreDObject", "Engine"):
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
        actor_cpp = utils.get_module_dht_output_dir("Engine") / "Actor.gen.cpp"
        actor_content = actor_cpp.read_text(encoding="utf-8")
        self.assertIn('"/Cpp/Engine",', actor_content)

        module_cpp = utils.get_module_dht_output_dir("Engine") / "Engine.module.gen.cpp"
        module_content = module_cpp.read_text(encoding="utf-8")
        self.assertIn('Durin::RegisterCompiledInPackage("Engine")', module_content)

    def test_class_display_and_default_object_name_metadata(self):
        static_mesh_actor_cpp = utils.get_module_dht_output_dir("Engine") / "StaticMeshActor.gen.cpp"
        content = static_mesh_actor_cpp.read_text(encoding="utf-8")

        self.assertIn('"Durin::AStaticMeshActor",', content)
        self.assertIn('"AStaticMeshActor",', content)
        self.assertIn('1,\n\t"Static Mesh Actor",', content)
        self.assertIn('"Static Mesh Actor",', content)
        self.assertIn('"StaticMeshActor"', content)

    def test_engine_runtime_property_flags(self):
        scene_component_cpp = utils.get_module_dht_output_dir("Engine") / "SceneComponent.gen.cpp"
        scene_component_content = scene_component_cpp.read_text(encoding="utf-8")

        self.assertIn(
            'NewProp_RelativeTransform = { "RelativeTransform", Durin::EPropertyFlags::Edit,',
            scene_component_content,
        )
        self.assertIn(
            'NewProp_ComponentToWorld = { "ComponentToWorld", Durin::EPropertyFlags::Edit | Durin::EPropertyFlags::ReadOnly | Durin::EPropertyFlags::Transient,',
            scene_component_content,
        )
        self.assertIn(
            'NewProp_AttachChildren = { "AttachChildren", Durin::EPropertyFlags::Transient,',
            scene_component_content,
        )

        engine_cpp = utils.get_module_dht_output_dir("Engine") / "Engine.gen.cpp"
        engine_content = engine_cpp.read_text(encoding="utf-8")
        self.assertIn(
            'NewProp_MainWorld = { "MainWorld", Durin::EPropertyFlags::Transient,',
            engine_content,
        )

        directional_light_cpp = utils.get_module_dht_output_dir("Engine") / "DirectionalLightComponent.gen.cpp"
        directional_light_content = directional_light_cpp.read_text(encoding="utf-8")
        self.assertIn("Z_Construct_DStruct_Durin_FLinearColor", directional_light_content)
        self.assertIn(
            'NewProp_Color_MetaData[] = { { "HideAlpha", "true" } };',
            directional_light_content,
        )
        self.assertIn(
            'NewProp_Color = { "Color", Durin::EPropertyFlags::Edit,',
            directional_light_content,
        )

    def test_brace_initialized_intrinsic_struct_properties_are_generated(self):
        spline_types_cpp = utils.get_module_dht_output_dir("Engine") / "SplineTypes.gen.cpp"
        content = spline_types_cpp.read_text(encoding="utf-8")

        for property_name in ("Position", "ArriveTangent", "LeaveTangent", "Rotation", "Scale"):
            self.assertIn(f'NewProp_{property_name} = {{ "{property_name}",', content)
        self.assertIn("Z_Construct_DStruct_Durin_FVector3", content)

    def test_manifest_records_generator_contract(self):
        manifest_path = utils.get_module_manifest_file_path("Engine")
        data = json.loads(manifest_path.read_text(encoding="utf-8"))

        self.assertEqual(data["SchemaVersion"], 3)
        self.assertEqual(data["ToolFingerprint"], "dht-tests")
        self.assertEqual(data["SymbolNameScheme"], "qualified-underscore-v1")
        self.assertEqual(data["ModuleName"], "Engine")
        self.assertEqual(data["Profile"], "DurinEditor")
        self.assertEqual(data["Platform"], "Win64")
        actor_dependencies = data["ResolvedSymbolDependencies"]["Public/Engine/Actor.h"]
        self.assertEqual(actor_dependencies["Durin::DObject"]["GeneratedHelperName"], "Z_Construct_DClass_Durin_DObject")


if __name__ == "__main__":
    unittest.main()
