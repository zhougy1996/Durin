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


@pytest.mark.usefixtures("reflection_fixture")
class TestReflectionAnnotation:
    @pytest.mark.parametrize(
        ("source", "diagnostic"),
        [
            (
                'DPROPERTY(LegacyNames = OldName) int Value;',
                "LegacyNames requires a quoted semicolon-separated list",
            ),
            (
                'DPROPERTY(LegacyNames = "Owner::OldName") int Value;',
                "LegacyNames entries require unqualified C++ identifiers",
            ),
            (
                'DPROPERTY(LegacyNames = "OldName;OldName") int Value;',
                "duplicate LegacyNames entry 'OldName'",
            ),
            (
                'DPROPERTY(LegacyNames = "OldName", LegacyNames = "OlderName") int Value;',
                "duplicate LegacyNames metadata",
            ),
        ],
    )
    def test_invalid_property_legacy_names_have_deterministic_diagnostics(self, source, diagnostic):
        with pytest.raises(ValueError, match=re.escape(diagnostic)):
            make_dht_parse_source(source)

    @pytest.mark.parametrize(
        ("source", "diagnostic"),
        [
            (
                'DENUM(DisplayName = "First", DisplayName = "Second") enum E { A };',
                "duplicate DisplayName metadata",
            ),
            (
                'DENUM(ToolTip = "No") enum E { A };',
                "unsupported metadata key 'ToolTip'",
            ),
            (
                'DENUM(PersistentName = "Legacy::E") enum E { A };',
                "unsupported metadata key 'PersistentName'",
            ),
            (
                'DENUM(DisplayName = Bare) enum E { A };',
                "DisplayName requires a quoted string",
            ),
            (
                'DENUM(DisplayName = "Broken) enum E { A };',
                "missing closing ')'",
            ),
            (
                'DENUM(LegacyNames = Legacy::E) enum E { A };',
                "LegacyNames requires a quoted semicolon-separated list",
            ),
            (
                'DENUM(LegacyNames = "Legacy::E;Legacy::E") enum E { A };',
                "duplicate LegacyNames entry 'Legacy::E'",
            ),
            (
                'DENUM(DisplayName = "Missing"',
                "missing closing ')'",
            ),
            (
                'enum E { A DMETA(DisplayName = "First", DisplayName = "Second") };',
                "duplicate DisplayName metadata",
            ),
        ],
        ids=[
            "duplicate-enum-metadata",
            "unsupported-enum-metadata",
            "persistent-name-is-unsupported",
            "unquoted-enum-display-name",
            "unterminated-enum-string",
            "legacy-names-require-list",
            "duplicate-legacy-name",
            "unterminated-enum-annotation",
            "duplicate-enumerator-metadata",
        ],
    )
    def test_invalid_enum_metadata_has_deterministic_diagnostics(self, source, diagnostic):
        with pytest.raises(ValueError, match=re.escape(diagnostic)):
            make_dht_parse_source(source)


    @pytest.mark.parametrize(
        ("source", "diagnostic"),
        [
            ("DCLASS(Abstract, Abstract) class FItem {};", "duplicate Abstract class specifier"),
            (
                "DCLASS(NoClassDefaultObject, NoClassDefaultObject) class FItem {};",
                "duplicate NoClassDefaultObject class specifier",
            ),
            ("DCLASS(Transient) class FItem {};", "unsupported class specifier 'Transient'"),
            (
                'DCLASS(Abstract = "true") class FItem {};',
                "unsupported class metadata key 'Abstract'",
            ),
            (
                'DCLASS(DisplayName = Bare) class FItem {};',
                "DisplayName requires a quoted string",
            ),
        ],
    )
    def test_invalid_class_specifiers_have_deterministic_diagnostics(self, source, diagnostic):
        with pytest.raises(ValueError, match=re.escape(diagnostic)):
            make_dht_parse_source(source)


    def test_annotation_names_in_comments_and_strings_are_ignored(self):
        source = '''// DENUM(Unknown = "comment")
const char* Text = "DMETA(Unknown = \\"string\\")";
/* DMETA(DisplayName = Bare) */
'''
        assert make_dht_parse_source(source) == source


    def test_translation_unit_skips_function_bodies(self):
        index = mock.Mock()
        index.parse.return_value = mock.sentinel.translation_unit
        with (
            mock.patch.object(clang.cindex.Index, "create", return_value=index),
            mock.patch("durin_header_tool.parser.clang_context._clang_args", return_value=[]),
        ):
            translation_unit, dmeta_uses = _parse_translation_unit(
                "Fixture",
                "Public/FixtureTypes.h",
                Path("FixtureTypes.h"),
                '#include "Ordinary.h"\nDCLASS() class FItem {};\n',
                export_mode=False,
            )

        assert translation_unit is mock.sentinel.translation_unit
        assert dmeta_uses == {}
        parse_args = index.parse.call_args.kwargs["args"]
        assert not any(argument.startswith("-I") for argument in parse_args)
        unsaved_files = index.parse.call_args.kwargs["unsaved_files"]
        assert len(unsaved_files) == 2
        assert "Ordinary.h" not in unsaved_files[0][1]
        assert unsaved_files[0][1].count("\n") == 2
        assert (
            index.parse.call_args.kwargs["options"]
            == clang.cindex.TranslationUnit.PARSE_SKIP_FUNCTION_BODIES
        )


    def test_ordinary_include_content_is_not_a_semantic_input(self):
        header = "Public/Hermetic.h"
        header_path = self.module_dir / header
        included_path = header_path.parent / "Ordinary.h"
        header_path.write_text(
            '''#include "Ordinary.h"
namespace Fixture
{
    DCLASS()
    class AHermetic : public Durin::DObject
    {
        GENERATED_BODY()
        DPROPERTY()
        float Value = 0.0f;
    };
}
''',
            encoding="utf-8",
        )
        included_path.write_text("using IncludedMeaning = int;\n", encoding="utf-8")
        config = DurinModuleConfig(
            module_name="Fixture",
            module_dir=self.module_dir,
            reflect_headers=[header],
        )

        def extract_outputs():
            header_info = parse_reflection_header("Fixture", header, exported_symbols=self.symbols)
            return (
                _extract_header_export_symbols_impl("Fixture", header),
                generate_header_content(header_info),
                generate_cpp_content(header_info, self.symbols),
            )

        with (
            mock.patch.object(configs, "get_module_config", return_value=config),
            mock.patch.object(configs, "collect_all_dependent_modules", return_value=set()),
        ):
            expected = extract_outputs()
            included_path.write_text("this is deliberately invalid C++\n", encoding="utf-8")
            assert extract_outputs() == expected
            included_path.unlink()
            assert extract_outputs() == expected


    def test_missing_include_alias_has_deterministic_diagnostic(self):
        header = "Public/UnsupportedAlias.h"
        header_path = self.module_dir / header
        header_path.write_text(
            '''#include "Alias.h"
namespace Fixture
{
    DSTRUCT()
    struct FUnsupportedAlias
    {
        GENERATED_BODY()
        DPROPERTY()
        IncludedAlias Value;
    };
}
''',
            encoding="utf-8",
        )
        config = DurinModuleConfig(
            module_name="Fixture",
            module_dir=self.module_dir,
            reflect_headers=[header],
        )
        with (
            mock.patch.object(configs, "get_module_config", return_value=config),
            mock.patch.object(configs, "collect_all_dependent_modules", return_value=set()),
            pytest.raises(ValueError, match="unsupported non-hermetic type spelling 'IncludedAlias'"),
        ):
            parse_reflection_header("Fixture", header, exported_symbols=self.symbols)


    def test_unknown_conditional_macro_has_deterministic_diagnostic(self):
        header = "Public/UnsupportedConditional.h"
        header_path = self.module_dir / header
        header_path.write_text(
            "#if INCLUDED_FEATURE\nDCLASS() class FConditional {};\n#endif\n",
            encoding="utf-8",
        )
        config = DurinModuleConfig(
            module_name="Fixture",
            module_dir=self.module_dir,
            reflect_headers=[header],
        )
        with (
            mock.patch.object(configs, "get_module_config", return_value=config),
            pytest.raises(ValueError, match="unsupported non-hermetic macro dependency 'INCLUDED_FEATURE'"),
        ):
            parse_reflection_header("Fixture", header)

    def test_target_predefines_are_truthful_and_non_overlapping(self):
        from durin_header_tool.parser.clang_context import _clang_args

        config = DurinModuleConfig(module_name="Fixture", module_dir=self.module_dir)
        with (
            mock.patch.object(configs, "get_module_config", return_value=config),
            mock.patch.object(configs, "collect_all_dependent_modules", return_value=set()),
            mock.patch.object(configs, "ARCH", "Win64"),
        ):
            win64 = _clang_args("Fixture", False)
        with (
            mock.patch.object(configs, "get_module_config", return_value=config),
            mock.patch.object(configs, "collect_all_dependent_modules", return_value=set()),
            mock.patch.object(configs, "ARCH", "MacOS"),
        ):
            macos = _clang_args("Fixture", False)

        assert "-D_WIN32=1" in win64
        assert "-D_MSC_VER=1930" in win64
        assert "--target=x86_64-pc-windows-msvc" in win64
        assert "-D__APPLE__=1" not in win64
        assert "-D__APPLE__=1" in macos
        assert "-D__arm64__=1" in macos
        assert "--target=arm64-apple-macos" in macos
        assert "-D_WIN32=1" not in macos
        assert "-D_MSC_VER=1930" not in macos

    def test_macos_target_selects_apple_platform_branch(self):
        header = "Public/MacOnly.h"
        (self.module_dir / header).write_text(
            "#if defined(__APPLE__) && !defined(_WIN32)\n"
            "DCLASS() class FMacOnly {};\n"
            "#endif\n",
            encoding="utf-8",
        )
        config = DurinModuleConfig(
            module_name="Fixture", module_dir=self.module_dir, reflect_headers=[header]
        )
        with (
            mock.patch.object(configs, "get_module_config", return_value=config),
            mock.patch.object(configs, "collect_all_dependent_modules", return_value=set()),
            mock.patch.object(configs, "ARCH", "MacOS"),
        ):
            result = parse_reflection_header("Fixture", header)

        assert [item.short_name for item in result.classes] == ["FMacOnly"]


    def test_dmeta_outside_reflected_enum_is_rejected(self):
        header = "Public/Misplaced.h"
        header_path = self.module_dir / header
        header_path.write_text(
            '''namespace Fixture
{
    enum EPlain
    {
        Value DMETA(DisplayName = "Visible")
    };
}
''',
            encoding="utf-8",
        )
        config = DurinModuleConfig(
            module_name="Fixture",
            module_dir=self.module_dir,
            reflect_headers=[header],
        )
        with (
            mock.patch.object(configs, "get_module_config", return_value=config),
            mock.patch.object(configs, "collect_all_dependent_modules", return_value=set()),
            mock.patch.object(utils, "get_module_dht_output_dir", return_value=self.dht_output_dir),
        ):
            with pytest.raises(ValueError, match="DMETA at line 5, column 15: "
                "annotation is only valid on an enumerator in a reflected enum"):
                with mock.patch(
                    "clang.cindex.Cursor.walk_preorder",
                    side_effect=AssertionError("DMETA validation must not walk the entire translation unit"),
                ):
                    parse_reflection_header("Fixture", header)


    def test_dmeta_usage_is_matched_by_source_occurrence(self):
        header = "Public/MixedMetadata.h"
        header_path = self.module_dir / header
        header_path.write_text(
            '''namespace Fixture
{
    DENUM()
    enum EReflected
    {
        Accepted DMETA(DisplayName = "Same")
    };

    enum EPlain
    {
        Rejected DMETA(DisplayName = "Same")
    };
}
''',
            encoding="utf-8",
        )
        config = DurinModuleConfig(
            module_name="Fixture",
            module_dir=self.module_dir,
            reflect_headers=[header],
        )
        with (
            mock.patch.object(configs, "get_module_config", return_value=config),
            mock.patch.object(configs, "collect_all_dependent_modules", return_value=set()),
            mock.patch.object(utils, "get_module_dht_output_dir", return_value=self.dht_output_dir),
        ):
            with pytest.raises(ValueError, match="DMETA at line 11, column 18"):
                parse_reflection_header("Fixture", header)
