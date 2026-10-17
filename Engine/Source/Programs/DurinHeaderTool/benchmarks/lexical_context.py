"""Measure uncached header generation; compare output digests across revisions."""
import argparse
import hashlib
import json
import platform
import statistics
import sys
import time
from contextlib import ExitStack
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from durin_header_tool import config as configs
from durin_header_tool.generators.module_reflection_files_generator import _generate_reflection_output_impl
from durin_header_tool.parser.cpp_source_scanner import CppSourceScanner
from durin_header_tool.parser import annotation_rewriter, clang_context, property_parser, reflection_ast_helpers
from durin_header_tool.parser.source_context import HeaderSourceContext
from durin_header_tool.resolver.reflection_resolver import load_available_symbols


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--compare", type=Path)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--module", default="Engine")
    parser.add_argument("--header", action="append")
    args = parser.parse_args()
    if args.rounds < 1:
        parser.error("--rounds must be positive")
    if args.compare and args.compare.resolve() == args.output.resolve():
        parser.error("--output must not overwrite the --compare baseline")
    configs.ARCH = "Win64"
    configs.RUNTIME_VARIANT = "DurinEditor"
    configs.init_configs()
    symbols = load_available_symbols(args.module)
    headers = args.header or ["Public/Materials/MaterialExpressions.h", "Public/StaticMesh/StaticMesh.h",
                              "Public/Components/PointLightComponent.h"]
    # Warm native loading and filesystem caches. Generation itself never reads
    # phase bundles or publishes outputs, so each timed round parses the header.
    for header in headers:
        _generate_reflection_output_impl(args.module, header, symbols)
    original = CppSourceScanner.__init__
    original_context_init = HeaderSourceContext.__post_init__
    samples = []
    for header in headers:
        for iteration in range(args.rounds):
            counts = {"scanners": 0, "scanned_characters": 0,
                      "eager_line_tables": 0, "eager_line_characters": 0,
                      "nonidentical_context_checks": 0, "context_comparison_characters": 0}

            def counted(self, source):
                counts["scanners"] += 1
                counts["scanned_characters"] += len(source)
                original(self, source)

            def counted_context_init(self):
                original_context_init(self)
                if "lines" in vars(self):
                    counts["eager_line_tables"] += 1
                    counts["eager_line_characters"] += len(self.source)

            def count_checks(function):
                def checked(source, context=None):
                    if context is not None and source is not context.source:
                        counts["nonidentical_context_checks"] += 1
                        counts["context_comparison_characters"] += len(source)
                    return function(source, context)
                return checked

            with ExitStack() as patches:
                patches.enter_context(mock.patch.object(CppSourceScanner, "__init__", counted))
                patches.enter_context(mock.patch.object(HeaderSourceContext, "__post_init__", counted_context_init))
                for module in (annotation_rewriter, clang_context, property_parser, reflection_ast_helpers):
                    patches.enter_context(mock.patch.object(module, "source_context_for",
                        count_checks(module.source_context_for)))
                start = time.perf_counter()
                result = _generate_reflection_output_impl(args.module, header, symbols)
                seconds = time.perf_counter() - start
            samples.append({"header": header, "iteration": iteration, "seconds": seconds, **counts,
                            "header_sha256": hashlib.sha256(result.generated_header.encode("utf-8")).hexdigest(),
                            "source_sha256": hashlib.sha256(result.generated_source.encode("utf-8")).hexdigest(),
                            "dependencies_sha256": hashlib.sha256(json.dumps(result.resolved_symbol_dependencies,
                                sort_keys=True).encode("utf-8")).hexdigest()})
        current = [sample for sample in samples if sample["header"] == header]
        print(f"{header}: median {statistics.median(s['seconds'] for s in current):.6f}s, "
              f"{current[0]['scanners']} scanners", flush=True)
    report = {"python": sys.version, "platform": platform.platform(), "module": args.module,
              "rounds": args.rounds, "arch": configs.ARCH, "runtime_variant": configs.RUNTIME_VARIANT,
              "samples": samples}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2), encoding="utf-8")
    if args.compare:
        previous = json.loads(args.compare.read_text(encoding="utf-8"))
        for header in headers:
            digests = {(sample["header_sha256"], sample["source_sha256"], sample["dependencies_sha256"])
                       for sample in [*previous["samples"], *samples] if sample["header"] == header}
            if len(digests) != 1 or not any(s["header"] == header for s in previous["samples"]):
                raise RuntimeError(f"Generated output or dependencies changed: {header}")
        print("All generated header, source and dependency digests match.")


if __name__ == "__main__":
    main()
