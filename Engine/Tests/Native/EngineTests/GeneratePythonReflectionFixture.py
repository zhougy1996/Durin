"""Compile the native contract fixture through the production DHT parser and writers."""
import sys
from pathlib import Path
from unittest import mock

workspace = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(workspace / "Engine/Source/Programs/DurinHeaderTool"))
from durin_header_tool import config as configs
from durin_header_tool.config.module_config import DurinModuleConfig
from durin_header_tool.model.export_info import ExportedSymbolInfo
from durin_header_tool.parser.reflection_parser import parse_reflection_header
from durin_header_tool.resolver.reflection_resolver import _add_builtin_symbols
from durin_header_tool.writers.reflection_header_writer import generate_header_content
from durin_header_tool.writers.reflection_source_writer import generate_cpp_content

output = Path(sys.argv[1])
output.mkdir(parents=True, exist_ok=True)
config = DurinModuleConfig(module_name="PythonFixture", module_dir=Path(__file__).parent,
                          reflect_headers=["Private/PythonReflectionTestTypes.h"])
symbols = {"Durin::DObject": ExportedSymbolInfo(
    Kind="class", ShortName="DObject", Namespace="Durin", QualifiedName="Durin::DObject",
    Header="Public/DObject/Object.h", API="COREDOBJECT_API")}
symbols["Durin::DEditorSubsystem"] = ExportedSymbolInfo(
    Kind="class", ShortName="DEditorSubsystem", Namespace="Durin", QualifiedName="Durin::DEditorSubsystem",
    Header="Public/Editor/EditorSubsystem.h", API="DURINED_API", BaseQualifiedName="Durin::DObject")
symbols["Durin::DClass"] = ExportedSymbolInfo(
    Kind="class", ShortName="DClass", Namespace="Durin", QualifiedName="Durin::DClass",
    Header="Public/DObject/Class.h", API="COREDOBJECT_API", BaseQualifiedName="Durin::DObject")
_add_builtin_symbols(symbols)
symbols["Durin::AActor"] = ExportedSymbolInfo(
    Kind="class", ShortName="AActor", Namespace="Durin", QualifiedName="Durin::AActor",
    Header="Public/Engine/Actor.h", API="ENGINE_API", BaseQualifiedName="Durin::DObject")
symbols["Durin::PythonFixtures::FReflectionValue"] = ExportedSymbolInfo(
    Kind="struct", ShortName="FReflectionValue", Namespace="Durin::PythonFixtures",
    QualifiedName="Durin::PythonFixtures::FReflectionValue", Header=config.reflect_headers[0], API="PYTHONFIXTURE_API")
with (mock.patch.object(configs, "get_module_config", return_value=config),
      mock.patch.object(configs, "collect_all_dependent_modules", return_value=set())):
    info = parse_reflection_header("PythonFixture", config.reflect_headers[0], symbols)
# This process-resident test fixture uses the loaded, non-unloadable interpreter
# module as its test lifetime owner; production writers use the actual module name.
info.module_name = "DurinPython"
for name, content in [("PythonReflectionTestTypes.gen.h", generate_header_content(info)),
                      ("PythonReflectionTestTypes.gen.cpp", generate_cpp_content(info, symbols))]:
    path = output / name
    if not path.exists() or path.read_text(encoding="utf-8") != content:
        path.write_text(content, encoding="utf-8")
