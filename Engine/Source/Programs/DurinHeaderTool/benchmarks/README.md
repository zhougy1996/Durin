# DHT lexical context measurement

Run `lexical_context.py` from the checkout root using the repository Python
environment. Existing Win64/DurinEditor module exports must be available. The
script loads symbols once, warms native loading and each selected header, then
runs three serial uncached parse-and-generation samples per header. No build,
module output publication, or phase-cache writes occur. Scanner construction and
input-character counts are collected inside each timed sample.
The script also records eager line-table allocations and context checks whose
source objects differ, including their input-character totals. These counters
capture work that scanner-construction counts alone miss. Lazy line-table
creation is excluded from the eager allocation counter.

```powershell
& .venv/Scripts/python.exe Engine/Source/Programs/DurinHeaderTool/benchmarks/lexical_context.py --output Build/DhtLexicalContext/baseline.json
# Apply the optimization, using the same headers, exports, Python and machine.
& .venv/Scripts/python.exe Engine/Source/Programs/DurinHeaderTool/benchmarks/lexical_context.py --output Build/DhtLexicalContext/optimized-final.json --compare Build/DhtLexicalContext/baseline.json
```

`--header` is repeatable; `--module` selects an Engine-project module and
`--rounds` changes the sample count. Comparison fails if generated header text,
generated source text, or resolved dependency digests differ, including variation
between rounds. Unit tests separately cover shared versus fresh contexts,
conditional branches, edited source, context lifetime, and bounded scanner counts.

## 2026-10-17 measurement

Baseline: revision `090099b51`. Optimized: that revision plus the shared-context
implementation committed with this record. Both used the same source headers and
exports, Windows, the repository Python environment, and Win64/DurinEditor.
The JSON reports retain Python/platform details, all samples, scan volumes, and
SHA-256 digests under ignored `Build/DhtLexicalContext/`. Medians are seconds per
uncached header parse and generation; these are local measurements rather than
whole-build speedup claims.

| Header | Baseline median | Optimized median | Scanners before/after | Characters scanned before/after |
| --- | ---: | ---: | ---: | ---: |
| MaterialExpressions.h | 21.279295 | 0.552953 | 650 / 211 | 20911934 / 303700 |
| StaticMesh.h | 0.355422 | 0.116301 | 20 / 10 | 227665 / 75666 |
| PointLightComponent.h | 0.138039 | 0.057047 | 20 / 10 | 16727 / 5901 |

Raw reports: `Build/DhtLexicalContext/baseline.json` and
`Build/DhtLexicalContext/optimized-final.json`. All generated header, source, and
dependency digests matched for all three headers and all nine timed samples.

## 2026-10-17 follow-up: canonical sources and lazy line tables

Baseline: revision `6f811ce2a`. Optimized: that revision plus the changes committed
with this follow-up. Both use the same extended benchmark script and the same
headers, exports, Python, platform, and target configuration as above. Each run
warms the headers and takes seven samples per header. Reproduce by running the
commands above on these revisions with `--rounds 7`, using separate report paths.

| Header | Equal-copy context checks before/after | Eager line tables before/after | Eager line-table input characters before/after |
| --- | ---: | ---: | ---: |
| MaterialExpressions.h | 343 / 0 | 6 / 0 | 303474 / 0 |
| StaticMesh.h | 0 / 0 | 5 / 0 | 75646 / 0 |
| PointLightComponent.h | 6 / 0 | 5 / 0 | 5487 / 0 |

For MaterialExpressions.h, equal-copy checks previously covered 16,094,246
characters per parse. They now use identical source objects. The scanner counts
and scanned-character totals in the previous table are unchanged for these
include-bearing headers. Include-free headers additionally reuse the scanner
from preprocessor validation for the initial annotation-rewriting pass; regression
tests cover that path, unchanged masking, and changed includes/conditional text.

The first timing comparison and an immediately consecutive baseline/optimized
pair were retained, along with an optimized confirmation run:

| Header | First baseline / optimized median (s) | Confirmation optimized median (s) | Consecutive baseline / optimized median (s) |
| --- | ---: | ---: | ---: |
| MaterialExpressions.h | 0.556324 / 0.625856 | 0.621499 | 0.579600 / 0.627034 |
| StaticMesh.h | 0.095999 / 0.111187 | 0.110666 | 0.101278 / 0.112907 |
| PointLightComponent.h | 0.052538 / 0.068136 | 0.057788 | 0.056991 / 0.167234 |

These wall-clock measurements do not demonstrate a speedup; optimized medians
were slower despite the deterministic reduction in comparisons and eager
allocations. No end-to-end timing improvement is claimed for this follow-up.
Raw samples and output/dependency SHA-256 digests are retained in
`Build/DhtLexicalContext/refinement-{baseline,optimized,confirmation,paired-baseline,paired-optimized}.json`.
All generated header, source, and dependency digests match across all five runs
(105 timed samples in total).

Validation on the optimized source: all DHT Python tests passed (416 passed,
one Windows directory-symlink permission skip). The nine added cases cover lazy
line tables, identical and equal-copy inputs, single context validation, include
rewriting, and unchanged/changed conditional masking. No native build was needed
for these parser-internal Python changes.
