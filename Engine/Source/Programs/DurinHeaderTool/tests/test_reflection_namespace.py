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


class TestNamespaceAwareResolution:
    @staticmethod
    def _symbol(qualified_name: str, kind: str = "class") -> ExportedSymbolInfo:
        namespace, short_name = qualified_name.rsplit("::", 1) if "::" in qualified_name else ("", qualified_name)
        return ExportedSymbolInfo(
            Kind=kind, ShortName=short_name, Namespace=namespace,
            QualifiedName=qualified_name,
            Header=f"{short_name}.h", API="FIXTURE_API",
        )

    @pytest.fixture
    def symbols(self):
        values = (
            self._symbol("Durin::AActor"), self._symbol("Other::AActor"),
            self._symbol("Durin::Gameplay::APawn"),
            self._symbol("Durin::Gameplay::FData", "struct"),
            self._symbol("Other::FData", "struct"),
            self._symbol("Durin::Gameplay::EMode", "enum"),
            self._symbol("Other::EMode", "enum"),
        )
        return {symbol.QualifiedName: symbol for symbol in values}

    @pytest.mark.parametrize(
        ("spelling", "namespace", "expected"),
        [
            ("AActor", "Durin::Gameplay", "Durin::AActor"),
            ("APawn", "Durin::Gameplay", "Durin::Gameplay::APawn"),
            ("Gameplay::APawn", "Durin::Sandbox", "Durin::Gameplay::APawn"),
            ("Durin::Gameplay::APawn", "Durin::Sandbox", "Durin::Gameplay::APawn"),
            ("::Durin::Gameplay::APawn", "Other", "Durin::Gameplay::APawn"),
        ],
    )
    def test_lexical_candidate_chain(self, symbols, spelling, namespace, expected):
        assert resolve_symbol(
            spelling, symbols, declaring_namespace=namespace, kinds=("class",)
        ).qualified_name == expected

    def test_unrelated_unique_name_is_not_visible(self, symbols):
        symbol = self._symbol("Unrelated::AUnique")
        symbols[symbol.QualifiedName] = symbol
        resolution = resolve_symbol(
            "AUnique", symbols, declaring_namespace="Durin::Gameplay", kinds=("class",)
        )
        assert not resolution.resolved
        assert resolution.attempted_names == (
            "Durin::Gameplay::AUnique", "Durin::AUnique", "AUnique"
        )
        assert resolution.candidates == ("Unrelated::AUnique",)

    def test_kind_filtering_and_order_independence(self, symbols):
        wrong_kind = self._symbol("Durin::Gameplay::AActor", "struct")
        symbols[wrong_kind.QualifiedName] = wrong_kind
        expected = resolve_symbol(
            "AActor", symbols, declaring_namespace="Durin::Gameplay", kinds=("class",)
        )
        actual = resolve_symbol(
            "AActor", dict(reversed(tuple(symbols.items()))),
            declaring_namespace="Durin::Gameplay", kinds=("class",),
        )
        assert actual == expected
        assert actual.qualified_name == "Durin::AActor"

    def test_nested_container_inherits_declaring_namespace(self, symbols):
        prop = _make_property_from_spelling(
            "Values", "std::unordered_map<EMode, std::vector<FData>>", symbols,
            declaring_namespace="Durin::Gameplay",
        )
        assert prop.key.referenced_enum_type == "Durin::Gameplay::EMode"
        assert prop.value.inner.referenced_struct_type == "Durin::Gameplay::FData"

    def test_unresolved_diagnostic_is_source_qualified_and_sorted(self, symbols):
        from durin_header_tool.model.reflection_info import ReflectedClassInfo, ReflectedHeaderInfo

        class_info = ReflectedClassInfo(
            short_name="ADerived", namespace="Durin::Gameplay",
            qualified_name="Durin::Gameplay::ADerived",
            header="Derived.h", api="FIXTURE_API", base_qualified_name="AMissing",
        )
        header = ReflectedHeaderInfo(
            "Fixture", "Derived.h", Path("Derived.h"), "Derived.h", "FID",
            classes=[class_info],
        )
        with pytest.raises(ValueError) as raised:
            resolve_header_symbols(header, symbols)
        assert str(raised.value) == (
            "Derived.h: reflected class 'Durin::Gameplay::ADerived' base has unresolved "
            "reflected type spelling 'AMissing' from namespace 'Durin::Gameplay' "
            "(allowed kinds: class); lexical lookup: Durin::Gameplay::AMissing, "
            "Durin::AMissing, AMissing; candidates: <none>"
        )


@pytest.mark.usefixtures("reflection_fixture")
class TestReflectionNamespaceIntegration:
    def test_namespace_scoped_symbols_preserve_underscores_inline_and_global_scope(self):
        header = "Public/NamespaceScopedSymbols.h"
        source = '''DSTRUCT()
struct F_Global_Value
{
    GENERATED_BODY()
};

namespace A_B
{
    DCLASS()
    class C
    {
        GENERATED_BODY()
    };
}

namespace A
{
    DCLASS()
    class B_C
    {
        GENERATED_BODY()
    };
}

namespace Root_Name
{
    inline namespace V_1
    {
        DSTRUCT()
        struct F_Row_Value
        {
            GENERATED_BODY()
        };

        DENUM()
        enum class E_Mode_Value : int { First_Value };
    }
}
'''
        (self.module_dir / header).write_text(source, encoding="utf-8")
        config = DurinModuleConfig(
            module_name="Fixture", module_dir=self.module_dir, reflect_headers=[header]
        )
        with (
            mock.patch.object(configs, "get_module_config", return_value=config),
            mock.patch.object(configs, "collect_all_dependent_modules", return_value=set()),
        ):
            header_info = parse_reflection_header("Fixture", header)

        classes = {item.qualified_name: item for item in header_info.classes}
        assert classes["A_B::C"].generated_helper_reference == "::A_B::Z_Construct_DClass_C"
        assert classes["A::B_C"].generated_helper_reference == "::A::Z_Construct_DClass_B_C"
        assert classes["A_B::C"].generated_helper_reference != classes["A::B_C"].generated_helper_reference

        global_struct = next(item for item in header_info.structs if item.short_name == "F_Global_Value")
        inline_struct = next(item for item in header_info.structs if item.short_name == "F_Row_Value")
        assert global_struct.generated_helper_reference == "::Z_Construct_DStruct_F_Global_Value"
        assert [(segment.name, segment.is_inline) for segment in inline_struct.namespace_path] == [
            ("Root_Name", False), ("V_1", True)
        ]
        assert inline_struct.generated_helper_reference == (
            "::Root_Name::V_1::Z_Construct_DStruct_F_Row_Value"
        )

        generated_header = generate_header_content(header_info)
        generated_source = generate_cpp_content(header_info, {})
        assert "inline namespace V_1" in generated_header
        assert "inline namespace V_1" in generated_source
        assert "Z_Construct_DClass_A_B_C" not in generated_header + generated_source

    def test_anonymous_namespace_reflection_is_rejected_before_generation(self):
        header = "Public/AnonymousNamespaceSymbol.h"
        (self.module_dir / header).write_text(
            '''namespace
{
    DSTRUCT()
    struct FHidden
    {
        GENERATED_BODY()
    };
}
''',
            encoding="utf-8",
        )
        config = DurinModuleConfig(
            module_name="Fixture", module_dir=self.module_dir, reflect_headers=[header]
        )
        with (
            mock.patch.object(configs, "get_module_config", return_value=config),
            mock.patch.object(configs, "collect_all_dependent_modules", return_value=set()),
            pytest.raises(ValueError, match="cannot be declared in an anonymous namespace"),
        ):
            parse_reflection_header("Fixture", header)

    def test_module_export_resolves_cold_same_module_base(self):
        base = ExportedSymbolInfo(
            Kind="class", ShortName="ABase", Namespace="Fixture", QualifiedName="Fixture::ABase",
            Header="Public/Base.h", API="FIXTURE_API",
        )
        derived = ExportedSymbolInfo(
            Kind="class", ShortName="ADerived", Namespace="Fixture", QualifiedName="Fixture::ADerived",
            Header="Public/Derived.h", API="FIXTURE_API",
            BaseQualifiedName="ABase",
        )
        first = resolve_module_export_info(
            "Fixture",
            {"Public/Derived.h": {derived.QualifiedName: derived}, "Public/Base.h": {base.QualifiedName: base}},
            {},
        )
        second = resolve_module_export_info(
            "Fixture",
            {"Public/Base.h": {base.QualifiedName: base}, "Public/Derived.h": {derived.QualifiedName: derived}},
            {},
        )

        assert first == second
        assert first.Symbols[derived.QualifiedName].BaseQualifiedName == base.QualifiedName


    def test_same_short_name_in_different_namespaces_uses_local_class_source(self):
        header = "Public/DuplicateShortNames.h"
        source = '''namespace Alpha
{
    DCLASS()
    class FItem
    {
        GENERATED_BODY()

        DPROPERTY()
        int32 First;
    };
}

namespace Beta
{
    DCLASS()
    class FItem
    {
        GENERATED_BODY()

        DPROPERTY()
        float Second;
    };
}
'''
        header_path = self.module_dir / header
        header_path.write_text(source, encoding="utf-8")
        config = DurinModuleConfig(
            module_name="Fixture",
            module_dir=self.module_dir,
            reflect_headers=[header],
        )
        generated_body_lines = [
            line_number
            for line_number, line in enumerate(source.splitlines(), start=1)
            if "GENERATED_BODY" in line
        ]

        with (
            mock.patch.object(configs, "get_module_config", return_value=config),
            mock.patch.object(configs, "collect_all_dependent_modules", return_value=set()),
            mock.patch.object(utils, "get_module_dht_output_dir", return_value=self.dht_output_dir),
            mock.patch("durin_header_tool.parser.reflection_parser._make_property", return_value=None),
        ):
            header_info = parse_reflection_header("Fixture", header)

        classes = {class_info.qualified_name: class_info for class_info in header_info.classes}
        assert classes["Alpha::FItem"].generated_body_line == generated_body_lines[0]
        assert classes["Beta::FItem"].generated_body_line == generated_body_lines[1]
        assert [prop.name for prop in classes["Alpha::FItem"].properties] == ["First"]
        assert [prop.name for prop in classes["Beta::FItem"].properties] == ["Second"]
