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
config = DurinModuleConfig(module_name="FunctionFixture", module_dir=Path(__file__).parent,
                          reflect_headers=["Private/FunctionReflectionTestTypes.h"])
symbols = {"Durin::DObject": ExportedSymbolInfo(
    Kind="class", ShortName="DObject", Namespace="Durin", QualifiedName="Durin::DObject",
    Header="Public/DObject/Object.h", API="COREDOBJECT_API")}
_add_builtin_symbols(symbols)
with (mock.patch.object(configs, "get_module_config", return_value=config),
      mock.patch.object(configs, "collect_all_dependent_modules", return_value=set())):
    info = parse_reflection_header("FunctionFixture", config.reflect_headers[0], symbols)
for name, content in [("FunctionReflectionTestTypes.gen.h", generate_header_content(info)),
                      ("FunctionReflectionTestTypes.gen.cpp", generate_cpp_content(info, symbols))]:
    path = output / name
    if not path.exists() or path.read_text(encoding="utf-8") != content:
        path.write_text(content, encoding="utf-8")
