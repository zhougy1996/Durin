import sys
import gc
import weakref
from collections import Counter
from contextlib import ExitStack
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

import pytest

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from durin_header_tool import config as configs
from durin_header_tool.config.module_config import DurinModuleConfig
from durin_header_tool.parser import annotation_rewriter, clang_context, property_parser, reflection_ast_helpers
from durin_header_tool.parser import reflection_parser
from durin_header_tool.parser.cpp_source_scanner import CppSourceScanner
from durin_header_tool.parser.reflection_ast_helpers import _cursor_source_line_range, _scan_generated_body_line
from durin_header_tool.parser.reflection_parser import parse_reflection_header
from durin_header_tool.parser.source_context import HeaderSourceContext, source_context_for
from durin_header_tool.writers.reflection_source_writer import generate_cpp_content, generate_header_content


def cursor(line, column=1):
    return SimpleNamespace(extent=SimpleNamespace(start=SimpleNamespace(line=line, column=column)))


def parse(tmp_path, source):
    (tmp_path / "Public").mkdir(exist_ok=True)
    (tmp_path / "Public/Contexts.h").write_text(source, encoding="utf-8")
    config = DurinModuleConfig(module_name="Fixture", module_dir=tmp_path, reflect_headers=["Public/Contexts.h"])
    with (mock.patch.object(configs, "get_module_config", return_value=config),
          mock.patch.object(configs, "collect_all_dependent_modules", return_value=set())):
        return parse_reflection_header("Fixture", "Public/Contexts.h")


def test_type_ranges_and_body_markers_share_one_lexical_snapshot():
    source = '''/* GENERATED_BODY() { } */
const char* Text = R"raw({ GENERATED_BODY() })raw";
class FFirst
{
    // GENERATED_BODY()
    GENERATED_BODY()
    void Helper() { int Value = 1'000; const char* Brace = "}"; }
};
class FSecond
{
    GENERATED_BODY()
};
'''
    context = HeaderSourceContext(source)
    with mock.patch.object(context.scanner, "find_matching_brace", wraps=context.scanner.find_matching_brace) as match:
        assert _cursor_source_line_range(source, cursor(3), context=context) == (3, 8)
        assert _scan_generated_body_line(source, cursor(3), context=context) == 6
        assert _scan_generated_body_line(source, cursor(3), context=context) == 6
        assert match.call_count == 1
    assert _scan_generated_body_line(source, cursor(9), context=context) == 11
    assert _cursor_source_line_range(source, cursor(0), context=context) == (0, 0)
    assert _scan_generated_body_line(source, cursor(100), context=context) == 0


def test_changed_source_cannot_reuse_an_old_snapshot():
    context = HeaderSourceContext("class FOld { GENERATED_BODY() };")
    assert source_context_for(context.source, context) is context
    changed = "class FNew { GENERATED_BODY() };"
    with pytest.raises(ValueError, match="different source snapshot"):
        _scan_generated_body_line(changed, cursor(1), context=context)
    with pytest.raises(ValueError, match="different source snapshot"):
        property_parser.HeaderPropertyContext.from_source(changed, source_context=context)
    with pytest.raises(ValueError, match="different source snapshot"):
        annotation_rewriter._make_dht_parse_source(changed, context=context)


def test_line_table_is_built_only_on_first_access():
    context = HeaderSourceContext("class FType {\nGENERATED_BODY()\n};\n")
    assert context.scope_line_range(1, 1) == (1, 3)
    assert context.generated_body_line(1, 3) == 2
    assert "lines" not in vars(context)
    assert context.lines == ("class FType {", "GENERATED_BODY()", "};")
    assert context.lines is context.lines
    assert vars(context)["lines"] is context.lines


def test_identical_source_needs_no_content_comparison():
    class IdentityOnlySource(str):
        def __ne__(self, other):
            raise AssertionError("identical source must not compare its contents")

    source = IdentityOnlySource("class FType { GENERATED_BODY() };")
    context = HeaderSourceContext(source)
    assert source_context_for(source, context) is context
    assert _scan_generated_body_line(source, cursor(1), context=context) == 1


def test_generated_body_validates_context_once():
    source = "class FType { GENERATED_BODY() };"
    context = HeaderSourceContext(source)
    with mock.patch.object(reflection_ast_helpers, "source_context_for", wraps=source_context_for) as checked:
        assert _scan_generated_body_line(source, cursor(1), context=context) == 1
    checked.assert_called_once_with(source, context)


@pytest.mark.parametrize("include", ["", '#include "Unused.h"\n'])
def test_rewriting_reuses_input_context_until_includes_change_source(include):
    body = "// DCLASS(Comment) GENERATED_BODY()\nint Value;\n"
    source = include + body
    context = HeaderSourceContext(source)
    original = CppSourceScanner.__init__
    scans = []

    def counted(self, text):
        scans.append(text)
        original(self, text)

    with mock.patch.object(CppSourceScanner, "__init__", counted):
        rewritten, metadata = annotation_rewriter._make_dht_parse_source(source, context=context)
    assert rewritten == ("\n" if include else "") + body
    assert metadata == {}
    assert scans == ([rewritten] if include else [])
    assert "lines" not in vars(context)
    if not include:
        assert rewritten is source


def test_equal_input_is_normalized_to_the_existing_snapshot():
    source = "// DCLASS(Comment)\nint Value;\n"
    equal_copy = source.encode("utf-8").decode("utf-8")
    assert equal_copy is not source
    context = HeaderSourceContext(source)
    with mock.patch.object(annotation_rewriter, "HeaderSourceContext", side_effect=AssertionError("unexpected rescan")):
        rewritten, _ = annotation_rewriter._make_dht_parse_source(equal_copy, context=context)
    assert rewritten is source


@pytest.mark.parametrize("mask", ["none", "unchanged", "inactive"])
def test_parse_preserves_snapshot_identity_at_all_context_checks(tmp_path, mask):
    source = """\nDCLASS()
class FType {
    GENERATED_BODY()
    DPROPERTY()
    int32 Value;
};
"""
    if mask == "inactive":
        source += "#if 0\nDCLASS()\nclass FInactive { GENERATED_BODY() };\n#endif\n"

    def checked(text, context=None):
        if context is not None:
            assert text is context.source
        return source_context_for(text, context)

    with ExitStack() as patches:
        for module in (annotation_rewriter, clang_context, property_parser, reflection_ast_helpers):
            patches.enter_context(mock.patch.object(module, "source_context_for", checked))
        if mask == "unchanged":
            # Blanking an already-empty skipped line produces equal text with a
            # different identity; it must still use the original snapshot.
            patches.enter_context(mock.patch.object(reflection_parser, "_skipped_source_lines", return_value={1}))
        result = parse(tmp_path, source)
    assert [item.short_name for item in result.classes] == ["FType"]
    assert result.classes[0].generated_body_line == 4
    assert [item.name for item in result.classes[0].properties] == ["Value"]


def test_whole_header_scans_do_not_grow_with_reflected_type_count(tmp_path):
    original = CppSourceScanner.__init__
    totals = []
    for count in (1, 24):
        source = "\n".join(f"""DCLASS()
class FType{index} {{
    GENERATED_BODY()
    DPROPERTY()
    int32 Value;
    DFUNCTION() void Run();
}};""" for index in range(count))
        scanned = Counter()

        def counted(self, text):
            scanned[text] += 1
            original(self, text)

        with mock.patch.object(CppSourceScanner, "__init__", counted):
            info = parse(tmp_path, source)
        assert len(info.classes) == count
        assert all([p.name for p in item.properties] == ["Value"] for item in info.classes)
        assert all([f.name for f in item.functions] == ["Run"] for item in info.classes)
        totals.append(sum(calls for text, calls in scanned.items() if len(text) >= len(source)))
    assert totals[0] == totals[1]
    assert totals[1] <= 10  # Original snapshot plus the fixed annotation rewrite passes.


def test_cached_and_fresh_contexts_generate_identical_outputs(tmp_path):
    source = '''#if DURIN_WITH_EDITORONLY_DATA
DCLASS()
class FEditorType {
    GENERATED_BODY()
    DPROPERTY()
    int32 EditorValue;
};
#else
DCLASS()
class FGameType {
    GENERATED_BODY()
    DFUNCTION() void GameAction();
};
#endif
DCLASS()
class FCommon {
    GENERATED_BODY()
    DFUNCTION() int Read(int Value = 1'000);
};
'''
    for variant, expected in (("DurinEditor", "FEditorType"), ("DurinGame", "FGameType")):
        with mock.patch.object(configs, "RUNTIME_VARIANT", variant):
            shared = parse(tmp_path, source)
            with ExitStack() as patches:
                for module in (annotation_rewriter, clang_context, property_parser, reflection_ast_helpers):
                    patches.enter_context(mock.patch.object(module, "source_context_for",
                        side_effect=lambda text, context=None: HeaderSourceContext(text)))
                fresh = parse(tmp_path, source)
        assert [item.short_name for item in shared.classes] == [expected, "FCommon"]
        assert shared == fresh
        assert generate_header_content(shared) == generate_header_content(fresh)
        assert generate_cpp_content(shared, {}) == generate_cpp_content(fresh, {})


def test_repeated_parses_of_the_same_path_have_no_shared_state(tmp_path):
    first = parse(tmp_path, "DCLASS()\nclass FOld { GENERATED_BODY() DFUNCTION() void Before(); };")
    second = parse(tmp_path, "\n\nDCLASS()\nclass FNew { GENERATED_BODY() DFUNCTION() void After(); };")
    assert first.classes[0].short_name == "FOld"
    assert second.classes[0].short_name == "FNew"
    assert first.classes[0].functions[0].name == "Before"
    assert second.classes[0].functions[0].name == "After"
    assert second.classes[0].generated_body_line == first.classes[0].generated_body_line + 2


def test_rewriting_rescans_only_when_the_source_snapshot_changes():
    source = "// DCLASS(Comment)\nDFUNCTION() void Run();\n"
    scans = Counter()
    original = CppSourceScanner.__init__

    def counted(self, text):
        scans[text] += 1
        original(self, text)

    with mock.patch.object(CppSourceScanner, "__init__", counted):
        rewritten = annotation_rewriter.make_dht_parse_source(source)
    assert "// DCLASS(Comment)" in rewritten
    assert 'annotate("DFUNCTION")' in rewritten
    assert scans[source] == 1
    assert scans[rewritten] == 1


def test_header_context_does_not_survive_its_parse(tmp_path):
    contexts = []

    def record(source):
        context = HeaderSourceContext(source)
        contexts.append(weakref.ref(context))
        return context

    with mock.patch.object(reflection_parser, "HeaderSourceContext", side_effect=record):
        parse(tmp_path, "DCLASS()\nclass FTemporary { GENERATED_BODY() DFUNCTION() void Run(); };")
    gc.collect()  # The recursive AST visitor has a closure cycle.
    assert contexts and all(context() is None for context in contexts)
