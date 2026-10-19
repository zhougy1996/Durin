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


@pytest.mark.parametrize("numeric_declaration", [
    "int Plain = 1'000;",
    "void Helper() { int Local = 1'000; }",
    "DFUNCTION() int Run(int Value = 1'000);",
])
def test_digit_separators_do_not_hide_later_reflection_markers(tmp_path, numeric_declaration):
    source = '''
    DCLASS()
    class AActions {
        GENERATED_BODY()
        ''' + numeric_declaration + '''
        DFUNCTION() void Next();
        DPROPERTY()
        int32 Later;
    };
    DENUM() enum class EAfter : int { Ready DMETA(DisplayName = "Ready") = 1 };
    DSTRUCT()
    struct FAfter {
        GENERATED_BODY()
        DPROPERTY()
        int32 Value;
    };
    DCLASS()
    class AAfter {
        GENERATED_BODY()
        DFUNCTION() void Last();
    };
    '''
    info = parse(tmp_path, source)
    assert [c.short_name for c in info.classes] == ["AActions", "AAfter"]
    first = info.classes[0]
    assert [f.name for f in first.functions] == (["Run", "Next"] if "DFUNCTION" in numeric_declaration else ["Next"])
    assert [p.name for p in first.properties] == ["Later"]
    assert [f.name for f in info.classes[1].functions] == ["Last"]
    assert info.classes[1].generated_body_line > 0
    assert [e.short_name for e in info.enums] == ["EAfter"]
    assert [s.short_name for s in info.structs] == ["FAfter"]
    assert [p.name for p in info.structs[0].properties] == ["Value"]
    generated = generate_cpp_content(info, {})
    assert "Invoke_Next" in generated and "Invoke_Last" in generated
    if "DFUNCTION" in numeric_declaration:
        assert first.functions[0].parameters[0].default_value == "1000LL"
        assert "static_cast<::int32>(1000LL)" in generated


def test_private_virtual_noexcept_function_generation(tmp_path):
    info = parse(tmp_path, declaration('private: DFUNCTION(CallInEditor, DisplayName="Do, It") virtual void Run() noexcept;'))
    function = info.classes[0].functions[0]
    assert function.is_virtual and function.call_in_editor
    assert function.display_name == "Do, It"
    cpp = generate_cpp_content(info, {})
    assert "(static_cast<AActions&>(Target).*Method_Run)();" in cpp
    assert ").AActions::Run(" not in cpp  # Preserve native virtual dispatch.
    assert "FFunctionParams" in cpp
    assert "static void Invoke_Run(Durin::DObject& Target, Durin::FFunctionFrame&, void*)" in cpp


@pytest.mark.parametrize("method", [
    "DFUNCTION() void Run(int = 0);", "DFUNCTION() static void Run();",
    "DFUNCTION() void Run() &;", "DFUNCTION() virtual void Run() = 0;",
    "DFUNCTION() void Run(...);", "DFUNCTION() template<class T> void Run();",
    "DFUNCTION() void Run(); DFUNCTION() void Run(int Value);", "DFUNCTION() int Value;",
    "DFUNCTION(RPC) void Run();", "DFUNCTION(CallInEditor, CallInEditor) void Run();",
    "DFUNCTION(Category=1) void Run();",
    "DFUNCTION() void operator()();", "DFUNCTION() void Run() volatile;",
    "DFUNCTION() void Run() = delete;", "DFUNCTION() consteval void Run() {}",
])
def test_unsupported_function_rejected(tmp_path, method):
    with pytest.raises(ValueError, match="DFUNCTION|function|overload"):
        parse(tmp_path, declaration(method))


def void_signature():
    return {"Const": False, "Return": "Void", "Parameters": []}


@pytest.mark.parametrize("expression", ["17", "MissingFromInclude", "Factory<int, int>(1, 2)",
                                        "([] { return 3; })()", "int{4}"])
def test_default_expression_does_not_change_signature(tmp_path, expression):
    info = parse(tmp_path, declaration(f"DFUNCTION() int Run(int Value = {expression});"))
    function = info.classes[0].functions[0]
    assert function.parameters[0].name == "Value"
    assert function.parameters[0].kind == "Int32"
    assert function.signature == {"Const": False, "Return": "Int32",
                                  "Parameters": [{"Type": "Int32", "Passing": "Value"}]}


@pytest.mark.parametrize("overload", ["void Run(int);", "void Run(int*);", "void Run() const;",
                                       "template<class T> void Run(T);", "void Run(int Value = 3);"])
def test_one_reflected_entry_can_have_ordinary_overloads(tmp_path, overload):
    info = parse(tmp_path, declaration("DFUNCTION() void Run(); " + overload))
    assert len(info.classes[0].functions) == 1
    cpp = generate_cpp_content(info, {})
    assert "static_cast<void (AActions::*)()>(&AActions::Run)" in cpp
    assert ".*Method_Run" in cpp


def test_const_only_reflected_overloads_remain_rejected(tmp_path):
    with pytest.raises(ValueError, match="overloads"):
        parse(tmp_path, declaration("DFUNCTION() int Run(); DFUNCTION() int Run() const;"))


def test_method_pointer_preserves_top_level_const_return(tmp_path):
    info = parse(tmp_path, declaration("DFUNCTION() const int Run(int Value = 0);"))
    assert "static_cast<::int32 const (AActions::*)(::int32)>" in generate_cpp_content(info, {})


def test_const_reference_and_nested_default_declarations(tmp_path):
    info = parse(tmp_path, declaration(
        'DFUNCTION() auto Run(const std::string& Text = "a,b={}", int Count = Unknown(1, 2)) const -> int;'))
    assert [p.passing for p in info.classes[0].functions[0].parameters] == ["ConstReference", "Value"]


def base_symbol(virtual=True):
    return ExportedSymbolInfo(Kind="class", ShortName="ABase", Namespace="",
        QualifiedName="ABase", Header="Public/Base.h", API="FIXTURE_API",
        Functions=[{"Name": "Run", "Virtual": virtual, "Final": False, "Signature": void_signature(), "Line": 7}])


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


def test_unannotated_override_default_is_not_signature_identity(tmp_path):
    base = base_symbol()
    base.Functions[0]["Signature"]["Parameters"] = [{"Type": "Int32", "Passing": "Value"}]
    info = parse(tmp_path,
        "DCLASS() class AActions : public ABase { GENERATED_BODY() void Run(int Value = Missing()) override; };",
        {"ABase": base})
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
        Functions=[{"Name": "Run", "Virtual": False, "Final": False, "Signature": void_signature(), "Line": 5}])
    exported = resolve_module_export_info("Fixture", {"Child": {"AChild": child}}, {"ABase": base_symbol()})
    assert exported.Symbols["AChild"].Functions[0]["Virtual"]


@pytest.mark.parametrize("method", [
    "void Run(...) override;", "void Run() volatile override;",
    "void Run() const override;", "void Run() & override;",
    "void Run() override = delete;", "template<class T> void Run();",
])
def test_unannotated_incompatible_override_uses_the_same_signature_contract(tmp_path, method):
    with pytest.raises(ValueError, match="hides reflected ancestor"):
        parse(tmp_path, "DCLASS() class AActions : public ABase { GENERATED_BODY() " + method + " };",
              {"ABase": base_symbol()})


@pytest.mark.parametrize("annotation", ["", "DFUNCTION() "])
def test_cross_header_final_override_is_rejected(tmp_path, annotation):
    symbol = base_symbol()
    symbol.Functions[0]["Final"] = True
    with pytest.raises(ValueError, match=r"final reflected ancestor ABase::Run.*Base.h:7"):
        parse(tmp_path,
              "DCLASS() class AActions : public ABase { GENERATED_BODY() "
              + annotation + "void Run() override; };", {"ABase": symbol})


def test_final_signature_and_native_flags_are_generated_and_exported(tmp_path):
    from durin_header_tool.extractors.export_symbol_extractor import extract_header_export_symbols
    body = declaration("DFUNCTION() virtual void Run() final;")
    info = parse(tmp_path, body)
    function = info.classes[0].functions[0]
    assert function.is_final and function.signature == void_signature()
    cpp = generate_cpp_content(info, {})
    assert "Durin::EFunctionFlags::Native" in cpp
    assert "Durin::EFunctionFlags::Final" in cpp
    assert '"Run", nullptr, Durin::EFunctionFlags::Native' in cpp
    config = DurinModuleConfig(module_name="Fixture", module_dir=tmp_path,
                               reflect_headers=["Public/Functions.h"])
    with (mock.patch.object(configs, "get_module_config", return_value=config),
          mock.patch.object(configs, "collect_all_dependent_modules", return_value=set())):
        exported = extract_header_export_symbols("Fixture", "Public/Functions.h")
    summary = exported["AActions"].Functions[0]
    assert summary["Final"] and summary["Signature"] == void_signature()


@pytest.mark.parametrize("key,value", [("Signature", "Other"), ("Final", 1)])
def test_export_rejects_invalid_signature_and_final_fields(key, value):
    data = base_symbol().to_json()
    data["Functions"][0][key] = value
    with pytest.raises(ValueError, match="function"):
        ExportedSymbolInfo.from_json(data)


@pytest.mark.parametrize("cpp,kind", [
    ("bool", "Bool"), ("Durin::int8", "Int8"), ("Durin::int16", "Int16"),
    ("Durin::int32", "Int32"), ("Durin::int64", "Int64"),
    ("Durin::uint8", "UInt8"), ("Durin::uint16", "UInt16"),
    ("Durin::uint32", "UInt32"), ("Durin::uint64", "UInt64"),
    ("float", "Float"), ("double", "Double"), ("std::string", "String"),
    ("Durin::FName", "Name"), ("Durin::FGuid", "Guid"),
])
def test_supported_value_parameter_and_return(tmp_path, cpp, kind):
    info = parse(tmp_path, declaration(f"DFUNCTION() {cpp} Echo({cpp} Value);"))
    function = info.classes[0].functions[0]
    assert function.signature == {"Const": False, "Return": kind,
                                  "Parameters": [{"Type": kind, "Passing": "Value"}]}
    generated = generate_cpp_content(info, {})
    assert "MakeFunctionLayoutParams<FParams_Echo>" in generated
    assert "ReturnValue{}" in generated
    assert "STRUCT_OFFSET(FParams_Echo, P0)" in generated


def test_const_input_output_and_string_return_generation(tmp_path):
    info = parse(tmp_path, declaration(
        "DFUNCTION() auto Combine(int Value, const std::string& Prefix, std::string& Output) const -> std::string;"))
    function = info.classes[0].functions[0]
    assert function.is_const and function.return_kind == "String"
    assert [p.passing for p in function.parameters] == ["Value", "ConstReference", "Reference"]
    generated = generate_cpp_content(info, {})
    assert "(static_cast<const AActions&>(Target).*Method_Combine)(" in generated
    assert "*static_cast<std::string*>(Result) =" in generated
    assert "EFunctionFlags::Const" in generated


@pytest.mark.parametrize("method", [
    "DFUNCTION() void Run(int* Value);", "DFUNCTION() int& Run();",
    "DFUNCTION() void Run(std::string&& Value);", "DFUNCTION() void Run(int Values[2]);",
    "DFUNCTION() void Run(std::vector<bool> Values);", "DFUNCTION() Mystery Run();",
    "DFUNCTION() void Run(int);",
])
def test_unsupported_parameter_ownership_or_type_is_rejected(tmp_path, method):
    with pytest.raises(ValueError, match="DFUNCTION"):
        parse(tmp_path, declaration(method))


@pytest.mark.parametrize("annotation", ["", "DFUNCTION() "])
def test_parameterized_override_uses_exported_signature(tmp_path, annotation):
    symbol = base_symbol()
    symbol.Functions[0]["Signature"] = {"Const": True, "Return": "String", "Parameters": [
        {"Type": "Int32", "Passing": "Reference"}]}
    info = parse(tmp_path, "DCLASS() class AActions : public ABase { GENERATED_BODY() "
                 + annotation + "std::string Run(int& Renamed) const override; };", {"ABase": symbol})
    if annotation:
        assert info.classes[0].functions[0].signature == symbol.Functions[0]["Signature"]
    with pytest.raises(ValueError, match="hides reflected ancestor"):
        parse(tmp_path, "DCLASS() class AActions : public ABase { GENERATED_BODY() "
              "std::string Run(int Renamed) const override; };", {"ABase": symbol})


def test_parameterized_export_roundtrip_and_dependency_snapshot(tmp_path):
    symbol = base_symbol()
    symbol.Functions[0]["Signature"]["Parameters"] = [{"Type": "String", "Passing": "ConstReference"}]
    assert ExportedSymbolInfo.from_json(symbol.to_json()) == symbol
    info = parse(tmp_path,
        "DCLASS() class AActions : public ABase { GENERATED_BODY() void Run(const std::string& Value) override; };",
        {"ABase": symbol})
    before = resolved_symbol_dependencies_for_header(info, {"ABase": symbol})
    symbol.Functions[0]["Signature"]["Parameters"][0]["Passing"] = "Reference"
    assert before != resolved_symbol_dependencies_for_header(info, {"ABase": symbol})


@pytest.mark.parametrize("cpp,expected", [
    ("AActions*", {"Kind": "Object", "Name": "AActions"}),
    ("Durin::TObjectPtr<AActions>", {"Kind": "ObjectPtr", "Name": "AActions"}),
    ("FPayload", {"Kind": "Struct", "Name": "FPayload"}),
    ("std::vector<AActions*>", {"Kind": "Array", "Inner": {"Kind": "Object", "Name": "AActions"}}),
    ("std::unordered_map<std::string, std::vector<FPayload>>",
     {"Kind": "Map", "Key": "String", "Value": {"Kind": "Array", "Inner": {"Kind": "Struct", "Name": "FPayload"}}}),
])
def test_composite_parameters_return_and_generated_storage(tmp_path, cpp, expected):
    source = "DSTRUCT() struct FPayload { GENERATED_BODY() DPROPERTY() std::string Text; }; "
    const_input = cpp + " const&" if cpp.endswith("*") else "const " + cpp + "&"
    info = parse(tmp_path, source + declaration(f"DFUNCTION() {cpp} Echo({const_input} Input, {cpp}& Output);"))
    function = info.classes[0].functions[0]
    assert function.return_kind == expected
    assert [p.kind for p in function.parameters] == [expected, expected]
    assert [p.passing for p in function.parameters] == ["ConstReference", "Reference"]
    cpp = generate_cpp_content(info, {})
    assert "TNativePropertyParams<" in cpp and "ReturnValue{}" in cpp


@pytest.mark.parametrize("value", [
    "std::vector<bool>", "std::vector<int, Allocator>", "std::unordered_map<AActions*, int>",
    "std::unordered_map<std::string, int, Hash>", "const AActions*", "AActions**",
    "std::vector<const AActions*>", "std::vector<std::vector<std::vector<std::vector<std::vector<int>>>>>",
    "Durin::FSoftObjectPtr", "Durin::FWeakObjectPtr",
])
def test_unsupported_composite_forms_are_rejected(tmp_path, value):
    with pytest.raises(ValueError, match="DFUNCTION"):
        parse(tmp_path, declaration(f"DFUNCTION() void Run({value} Value);"))


def test_struct_requires_reflected_identity(tmp_path):
    with pytest.raises(ValueError, match="DFUNCTION"):
        parse(tmp_path, "struct FPayload { int Value; }; " + declaration("DFUNCTION() FPayload Run(FPayload Value);"))


def test_four_container_levels_allow_object_wrapper_leaf(tmp_path):
    value = "Durin::TObjectPtr<AActions>"
    for _ in range(4):
        value = f"std::vector<{value}>"
    info = parse(tmp_path, declaration(f"DFUNCTION() {value} Run(const {value}& Input);"))
    schema = info.classes[0].functions[0].return_kind
    for _ in range(4):
        assert schema["Kind"] == "Array"
        schema = schema["Inner"]
    assert schema == {"Kind": "ObjectPtr", "Name": "AActions"}


def test_composite_export_resolves_types_across_headers_and_namespaces(tmp_path):
    from durin_header_tool.extractors.export_symbol_extractor import extract_header_export_symbols, resolve_module_export_info
    from durin_header_tool.model.function_signature import validate_function_signature
    header = tmp_path / "Public/Functions.h"
    header.parent.mkdir()
    header.write_text("namespace Sample { DCLASS() class AActions { GENERATED_BODY() "
                      "DFUNCTION() virtual std::vector<FPayload> Run(const FPayload& Input); }; }", encoding="utf-8")
    config = DurinModuleConfig(module_name="Fixture", module_dir=tmp_path, reflect_headers=["Public/Functions.h"])
    payload = ExportedSymbolInfo(Kind="struct", ShortName="FPayload", Namespace="Sample", QualifiedName="Sample::FPayload",
                                 Header="Public/Payload.h", API="FIXTURE_API")
    with (mock.patch.object(configs, "get_module_config", return_value=config),
          mock.patch.object(configs, "collect_all_dependent_modules", return_value=set())):
        raw = extract_header_export_symbols("Fixture", "Public/Functions.h")
    assert raw["Sample::AActions"].Functions[0]["Signature"]["Return"]["Inner"]["Kind"] == "Named"
    exported = resolve_module_export_info("Fixture", {"Public/Functions.h": raw}, {payload.QualifiedName: payload})
    base = exported.Symbols["Sample::AActions"]
    assert base.Functions[0]["Signature"]["Return"]["Inner"]["Name"] == payload.QualifiedName
    assert ExportedSymbolInfo.from_json(base.to_json()) == base
    signature = base.Functions[0]["Signature"]
    assert validate_function_signature(signature)
    info = parse(tmp_path, "namespace Sample { DCLASS() class ADerived : public AActions { GENERATED_BODY() "
                 "DFUNCTION() std::vector<FPayload> Run(const FPayload& Renamed) override; }; }",
                 {base.QualifiedName: base, payload.QualifiedName: payload})
    assert info.classes[0].functions[0].signature == signature
    dependencies = resolved_symbol_dependencies_for_header(info, {base.QualifiedName: base, payload.QualifiedName: payload})
    assert payload.QualifiedName in dependencies
    with pytest.raises(ValueError, match="DFUNCTION"):
        resolve_module_export_info("Fixture", {"Public/Functions.h": raw}, {})


@pytest.mark.parametrize("value", [
    {"Kind": "Array", "Inner": "Void"}, {"Kind": "Object", "Name": "Bad*"},
    {"Kind": "Map", "Key": {"Kind": "Object", "Name": "A"}, "Value": "Int32"},
    {"Kind": "Struct", "Name": "Payload", "Extra": True},
])
def test_export_rejects_invalid_recursive_function_schema(value):
    data = base_symbol().to_json()
    data["Functions"][0]["Signature"]["Return"] = value
    with pytest.raises(ValueError, match="function"):
        ExportedSymbolInfo.from_json(data)


@pytest.mark.parametrize("cpp,kind", [
    ("EMode", "Enum"), ("Durin::TSoftObjectPtr<AActions>", "SoftObject"),
    ("Durin::TWeakObjectPtr<AActions>", "WeakObject"),
])
def test_enum_and_nonowning_reference_signatures(tmp_path, cpp, kind):
    source = "DENUM() enum class EMode : unsigned char { Zero = 0, High = 255 }; "
    info = parse(tmp_path, source + declaration(f"DFUNCTION() {cpp} Echo({cpp} Input, const {cpp}& Other, {cpp}& Output);"))
    function = info.classes[0].functions[0]
    expected = {"Kind": kind, "Name": "EMode" if kind == "Enum" else "AActions"}
    assert function.return_kind == expected
    assert [p.kind for p in function.parameters] == [expected] * 3
    assert [p.passing for p in function.parameters] == ["Value", "ConstReference", "Reference"]
    generated = generate_cpp_content(info, {})
    assert "TNativePropertyParams<" in generated and "ReturnValue{}" in generated
    if kind == "Enum":
        assert "FNativeEnumRegistration" in generated
        assert "NativeEnumUnderlyingType<EMode>()" in generated


@pytest.mark.parametrize("wrapper", ["TSoftObjectPtr", "TWeakObjectPtr"])
def test_nonowning_references_nested_under_enum_map_key(tmp_path, wrapper):
    source = "DENUM() enum class EMode : int { Negative = -2 }; "
    cpp = f"std::unordered_map<EMode, std::vector<Durin::{wrapper}<AActions>>>"
    info = parse(tmp_path, source + declaration(f"DFUNCTION() {cpp} Run(const {cpp}& Input);"))
    schema = info.classes[0].functions[0].return_kind
    assert schema["Key"] == {"Kind": "Enum", "Name": "EMode"}
    assert schema["Value"]["Inner"] == {"Kind": "SoftObject" if wrapper == "TSoftObjectPtr" else "WeakObject", "Name": "AActions"}


@pytest.mark.parametrize("value", [
    "Durin::TSoftObjectPtr<const AActions>", "Durin::TWeakObjectPtr<AActions*>",
    "Durin::TSoftObjectPtr<int>", "Durin::TWeakObjectPtr<FPayload>",
    "std::unordered_map<Durin::TSoftObjectPtr<AActions>, int>",
    "std::unordered_map<Durin::TWeakObjectPtr<AActions>, int>",
    "std::unordered_map<FPayload, int>",
])
def test_reference_targets_and_map_keys_require_supported_reflected_types(tmp_path, value):
    source = "DSTRUCT() struct FPayload { GENERATED_BODY() }; "
    with pytest.raises(ValueError, match="DFUNCTION"):
        parse(tmp_path, source + declaration(f"DFUNCTION() {value} Run({value} Input);"))


def test_unreflected_enum_is_rejected(tmp_path):
    with pytest.raises(ValueError, match="DFUNCTION"):
        parse(tmp_path, "enum class EMode : int { One = 1 }; " + declaration("DFUNCTION() EMode Run(EMode Input);"))


@pytest.mark.parametrize("cpp", ["EMode", "Durin::TSoftObjectPtr<AActions>", "Durin::TWeakObjectPtr<AActions>"])
def test_enum_reference_export_and_cross_header_overrides(tmp_path, cpp):
    from durin_header_tool.extractors.export_symbol_extractor import extract_header_export_symbols, resolve_module_export_info
    header = tmp_path / "Public/Functions.h"
    header.parent.mkdir()
    header.write_text("namespace Sample { DCLASS() class AActions { GENERATED_BODY() "
                      f"DFUNCTION() virtual {cpp} Run(const {cpp}& Input); " + "}; }", encoding="utf-8")
    mode = ExportedSymbolInfo(Kind="enum", ShortName="EMode", Namespace="Sample", QualifiedName="Sample::EMode",
        Header="Public/Mode.h", API="FIXTURE_API", IsScoped=True, UnderlyingType="unsigned char", UnderlyingKind="UInt8", UnderlyingSize=1)
    config = DurinModuleConfig(module_name="Fixture", module_dir=tmp_path, reflect_headers=["Public/Functions.h"])
    with (mock.patch.object(configs, "get_module_config", return_value=config),
          mock.patch.object(configs, "collect_all_dependent_modules", return_value=set())):
        raw = extract_header_export_symbols("Fixture", "Public/Functions.h")
    exported = resolve_module_export_info("Fixture", {"Public/Functions.h": raw}, {mode.QualifiedName: mode})
    base = exported.Symbols["Sample::AActions"]
    symbols = {base.QualifiedName: base, mode.QualifiedName: mode}
    assert ExportedSymbolInfo.from_json(base.to_json()) == base
    info = parse(tmp_path, "namespace Sample { DCLASS() class ADerived : public AActions { GENERATED_BODY() "
        f"DFUNCTION() {cpp} Run(const {cpp}& Renamed) override; " + "}; }", symbols)
    assert info.classes[0].functions[0].signature == base.Functions[0]["Signature"]
    dependencies = resolved_symbol_dependencies_for_header(info, symbols)
    assert (mode.QualifiedName if cpp == "EMode" else base.QualifiedName) in dependencies


@pytest.mark.parametrize("cpp, expression, expected", [
    ("bool", "false", "false"), ("int8", "-128", "-128LL"),
    ("int8", "128", None), ("uint8", "-1", None), ("int32", "-1U", None),
    ("int64", "-0xffffffff", None), ("int64", "-0xffffffffL", None),
    ("int64", "-0xffffffffLL", "-4294967295LL"),
    ("int64", "-9223372036854775807LL", "-9223372036854775807LL"),
    ("uint64", "18446744073709551615ULL", "18446744073709551615ULL"),
    ("uint64", "18446744073709551616ULL", None), ("int", "0xff", "255LL"),
    ("float", "1.25f", "1.25f"), ("double", "-2.5", "-2.5"),
    ("float", "1e100", None), ("double", "1e999", None),
    ("std::string", '"a\\n" "b"', '"a\\n" "b"'),
    ("std::string", 'MakeString("x")', None),
    ("std::vector<int>", "{1, 2}", None), ("Durin::FName", 'Durin::FName("x")', None),
    ("int", "FromStrippedInclude", None), ("int", "Factory<int, int>(1, 2)", None),
    ("int&", "NativeGlobal", None), ("AActions*", "nullptr", "nullptr"),
    ("AActions*", "GetObject()", None),
])
def test_reflected_default_literal_subset(tmp_path, cpp, expression, expected):
    info = parse(tmp_path, declaration(f"DFUNCTION() void Run({cpp} Value = {expression});"))
    parameter = info.classes[0].functions[0].parameters[0]
    assert parameter.default_expression is not None
    assert parameter.default_value == expected
    generated = generate_cpp_content(info, {})
    assert ("EFunctionDefaultState::NativeOnly" in generated) == (expected is None)
    assert "Defaults_Run" in generated


def test_enum_default_uses_exact_reflected_type(tmp_path):
    source = "DENUM() enum class EMode : uint8 { Ready = 7 }; "
    info = parse(tmp_path, source + declaration("DFUNCTION() void Run(EMode Value = EMode::Ready);"))
    assert info.classes[0].functions[0].parameters[0].default_value == "::EMode::Ready"


def test_default_only_export_edit_invalidates_dependents_without_signature_change(tmp_path):
    from durin_header_tool.extractors.export_symbol_extractor import extract_header_export_symbols
    header = tmp_path / "Public/Functions.h"
    header.parent.mkdir()
    config = DurinModuleConfig(module_name="Fixture", module_dir=tmp_path, reflect_headers=["Public/Functions.h"])
    results = []
    for value in (7, 9):
        header.write_text(declaration(f"DFUNCTION() virtual int Run(int Value = {value});"), encoding="utf-8")
        with (mock.patch.object(configs, "get_module_config", return_value=config),
              mock.patch.object(configs, "collect_all_dependent_modules", return_value=set())):
            results.append(extract_header_export_symbols("Fixture", "Public/Functions.h")["AActions"])
    assert results[0].Functions[0]["Signature"] == results[1].Functions[0]["Signature"]
    assert results[0].Functions[0]["Defaults"] == ["7"]
    assert ExportedSymbolInfo.from_json(results[1].to_json()) == results[1]
    derived = parse(tmp_path, "DCLASS() class ADerived : public AActions { GENERATED_BODY() };",
                    {"AActions": results[0]})
    assert (resolved_symbol_dependencies_for_header(derived, {"AActions": results[0]})
            != resolved_symbol_dependencies_for_header(derived, {"AActions": results[1]}))
    corrupt = results[0].to_json()
    corrupt["Functions"][0]["Defaults"] = [1]
    with pytest.raises(ValueError, match="defaults"):
        ExportedSymbolInfo.from_json(corrupt)


def test_export_preserves_external_enum_defaults_without_enumerator_declarations(tmp_path):
    from durin_header_tool.extractors.export_symbol_extractor import extract_header_export_symbols
    header = tmp_path / "Public/Functions.h"
    header.parent.mkdir()
    header.write_text('''
        namespace Sample {
        enum class EMode : uint8;
        DCLASS() class AActions { GENERATED_BODY()
            DFUNCTION() void Run(const std::string& Tag = "a,b", EMode Mode = EMode::Ready);
        };
        }
    ''', encoding="utf-8")
    config = DurinModuleConfig(module_name="Fixture", module_dir=tmp_path, reflect_headers=["Public/Functions.h"])
    with (mock.patch.object(configs, "get_module_config", return_value=config),
          mock.patch.object(configs, "collect_all_dependent_modules", return_value=set())):
        symbol = extract_header_export_symbols("Fixture", "Public/Functions.h")["Sample::AActions"]
    assert symbol.Functions[0]["Defaults"] == ['"a,b"', 'EMode :: Ready']
    assert ExportedSymbolInfo.from_json(symbol.to_json(), allow_unresolved_functions=True) == symbol


def test_unresolved_named_types_are_private_to_export_phase_cache():
    from durin_header_tool.cache.phase_state import ExportPhaseState
    from durin_header_tool.model.function_signature import function_cpp_type
    symbol = base_symbol()
    symbol.Functions[0]["Signature"]["Return"] = {
        "Kind": "Map", "Key": {"Kind": "Named", "Name": "EMode"}, "Value": "Int32"}
    state = ExportPhaseState(module="Fixture", tool_fingerprint="tool", native_libclang_fingerprint="clang",
                            platform="Win64", runtime_variant="DurinEditor", context_digest="context",
                            raw_symbols_by_header={"Public/Base.h": {symbol.QualifiedName: symbol}})
    assert ExportPhaseState.from_json(state.to_json()) == state
    with pytest.raises(ValueError, match="function"):
        ExportedSymbolInfo.from_json(symbol.to_json())
    with pytest.raises(ValueError, match="Unresolved"):
        function_cpp_type(symbol.Functions[0]["Signature"]["Return"])


@pytest.mark.parametrize("kind", ["struct", "enum"])
def test_export_resolves_named_map_keys_before_validation(kind):
    from durin_header_tool.extractors.export_symbol_extractor import resolve_module_export_info
    symbol = base_symbol()
    symbol.Functions[0]["Signature"]["Return"] = {
        "Kind": "Map", "Key": {"Kind": "Named", "Name": "KeyType"}, "Value": "Int32"}
    key = ExportedSymbolInfo(Kind=kind, ShortName="KeyType", Namespace="", QualifiedName="KeyType", Header="Public/Key.h", API="FIXTURE_API")
    if kind == "struct":
        with pytest.raises(ValueError, match="DFUNCTION"):
            resolve_module_export_info("Fixture", {"Public/Base.h": {symbol.QualifiedName: symbol}}, {"KeyType": key})
    else:
        exported = resolve_module_export_info("Fixture", {"Public/Base.h": {symbol.QualifiedName: symbol}}, {"KeyType": key})
        result = exported.Symbols[symbol.QualifiedName].Functions[0]["Signature"]["Return"]
        assert result["Key"] == {"Kind": "Enum", "Name": "KeyType"}
