from pathlib import Path
from unittest import mock
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from durin_header_tool import config as configs
from durin_header_tool.config.module_config import DurinModuleConfig
from durin_header_tool.model.export_info import ExportedSymbolInfo
from durin_header_tool.parser.reflection_parser import parse_reflection_header
from durin_header_tool.writers.reflection_source_writer import generate_cpp_content
from durin_header_tool.resolver.reflection_resolver import resolved_symbol_dependencies_for_header


def parse(tmp_path, body, symbols=None):
    header = tmp_path / "Public/Functions.h"
    header.parent.mkdir(exist_ok=True)
    header.write_text(body, encoding="utf-8")
    config = DurinModuleConfig(module_name="Fixture", module_dir=tmp_path,
                               reflect_headers=["Public/Functions.h"])
    with (mock.patch.object(configs, "get_module_config", return_value=config),
          mock.patch.object(configs, "collect_all_dependent_modules", return_value=set())):
        return parse_reflection_header("Fixture", "Public/Functions.h", symbols)


def declaration(method):
    return "DCLASS() class AActions { GENERATED_BODY() " + method + " };"


def test_private_virtual_noexcept_function_generation(tmp_path):
    info = parse(tmp_path, declaration('private: DFUNCTION(CallInEditor, DisplayName="Do, It") virtual void Run() noexcept;'))
    function = info.classes[0].functions[0]
    assert function.is_virtual and function.call_in_editor
    assert function.display_name == "Do, It"
    cpp = generate_cpp_content(info, {})
    assert "static_cast<AActions&>(Target).Run();" in cpp
    assert "AActions::Run" not in cpp  # Preserve native virtual dispatch.
    assert "FFunctionParams" in cpp


@pytest.mark.parametrize("method", [
    "DFUNCTION() int Run();", "DFUNCTION() void Run(int = 0);",
    "DFUNCTION() static void Run();", "DFUNCTION() void Run() const;",
    "DFUNCTION() void Run() &;", "DFUNCTION() virtual void Run() = 0;",
    "DFUNCTION() void Run(...);", "DFUNCTION() template<class T> void Run();",
    "DFUNCTION() void Run(); void Run(int);", "DFUNCTION() int Value;",
    "DFUNCTION(RPC) void Run();", "DFUNCTION(CallInEditor, CallInEditor) void Run();",
    "DFUNCTION(Category=1) void Run();",
    "DFUNCTION() void operator()();", "DFUNCTION() void Run() volatile;",
    "DFUNCTION() void Run() = delete;", "DFUNCTION() consteval void Run() {}",
])
def test_unsupported_function_rejected(tmp_path, method):
    with pytest.raises(ValueError, match="DFUNCTION|function|overload"):
        parse(tmp_path, declaration(method))


def base_symbol(virtual=True):
    return ExportedSymbolInfo(Kind="class", ShortName="ABase", Namespace="",
        QualifiedName="ABase", Header="Public/Base.h", API="FIXTURE_API",
        Functions=[{"Name": "Run", "Virtual": virtual, "Line": 7}])


def test_annotated_override_can_remove_editor_flag(tmp_path):
    info = parse(tmp_path,
        "DCLASS() class AActions : public ABase { GENERATED_BODY() DFUNCTION() void Run() override; };",
        {"ABase": base_symbol()})
    assert info.classes[0].functions[0].is_virtual
    assert not info.classes[0].functions[0].call_in_editor


@pytest.mark.parametrize("method", ["void Run();", "void Run(int);", "DFUNCTION() void Run();"])
def test_nonvirtual_hiding_reports_both_locations(tmp_path, method):
    with pytest.raises(ValueError, match=r"AActions::Run.*ABase::Run.*Base.h:7"):
        parse(tmp_path, "DCLASS() class AActions : public ABase { GENERATED_BODY() " + method + " };",
              {"ABase": base_symbol(False)})


def test_function_export_roundtrip_and_validation():
    symbol = base_symbol()
    assert ExportedSymbolInfo.from_json(symbol.to_json()) == symbol
    data = symbol.to_json()
    data["Functions"][0]["Virtual"] = 1
    with pytest.raises(ValueError, match="function"):
        ExportedSymbolInfo.from_json(data)


def test_export_parse_recognizes_override_without_base_semantics(tmp_path):
    info = parse(tmp_path,
        "DCLASS() class AActions : public AUnknownBase { GENERATED_BODY() DFUNCTION() void Run() override; };")
    assert info.classes[0].functions[0].is_virtual


def test_unannotated_virtual_override_preserves_inherited_slot(tmp_path):
    info = parse(tmp_path,
        "DCLASS() class AActions : public ABase { GENERATED_BODY() void Run() override; };",
        {"ABase": base_symbol()})
    assert not info.classes[0].functions


def test_transitive_function_dependency_invalidates_descendant(tmp_path):
    middle = ExportedSymbolInfo(Kind="class", ShortName="AMiddle", Namespace="",
        QualifiedName="AMiddle", Header="Public/Middle.h", API="FIXTURE_API", BaseQualifiedName="ABase")
    symbols = {"ABase": base_symbol(), "AMiddle": middle}
    info = parse(tmp_path,
        "DCLASS() class AActions : public AMiddle { GENERATED_BODY() DFUNCTION() void Run() override; };", symbols)
    before = resolved_symbol_dependencies_for_header(info, symbols)
    assert set(before) == {"ABase", "AMiddle"}
    symbols["ABase"].Functions[0]["Virtual"] = False
    assert before != resolved_symbol_dependencies_for_header(info, symbols)


@pytest.mark.parametrize("source", [
    "DFUNCTION() void Free();",
    "DSTRUCT() struct FActions { GENERATED_BODY() DFUNCTION() void Run(); };",
    declaration("template<class T> DFUNCTION() void Run();"),
    "class AUnreflected { DFUNCTION() void Run(); };",
])
def test_unattached_and_struct_functions_rejected(tmp_path, source):
    with pytest.raises(ValueError, match="DFUNCTION"):
        parse(tmp_path, source)


def test_export_resolves_implicit_virtual_override():
    from durin_header_tool.extractors.export_symbol_extractor import resolve_module_export_info
    child = ExportedSymbolInfo(Kind="class", ShortName="AChild", Namespace="",
        QualifiedName="AChild", Header="Public/Child.h", API="FIXTURE_API", BaseQualifiedName="ABase",
        Functions=[{"Name": "Run", "Virtual": False, "Line": 5}])
    exported = resolve_module_export_info("Fixture", {"Child": {"AChild": child}}, {"ABase": base_symbol()})
    assert exported.Symbols["AChild"].Functions[0]["Virtual"]
