# Assertion Semantics and Side-Effect Audit Plan

Summary: Define optimized-development assertion semantics, decouple assertion reporting from module logging macros, and prevent required C++ behavior from disappearing with checks.

Last reviewed: 2026-08-08

Status: Archived
Completed: 2026-08-08

## Current Status

Stage 4 completed on 2026-08-08, and the completed implementation was squashed
and rebased onto `dev` baseline `21019339`. The path-free
`DevTool audit assertions` command is now the enforcing presubmit: it loads the
versioned exact allowlist from `Tools/DurinDevTool/config`, rejects stale
entries, and fails on any unreviewed finding. Checked-in fixtures prove that an
unreviewed side-effecting `check` is rejected while an intentional `verify` is
intrinsically classified. Lasting assertion semantics and audit operation now
live in the C++ standards and build documentation rather than this plan.

The final inventory covers 670 assertion invocations in 110 files and 402
conservative findings: 380 reviewed observational or diagnostic records have
exact rationales, 19 records are enforced `require` runtime contracts, and 3
are intentional `verify` operations. Nothing is unreviewed. The tracked report
and two fresh complete scans are byte-identical with SHA-256
`8A072DB678C1E07201216A6F31892A3A6195ACD88AAC80BB35C28B0E8C5DD0D5`.

The post-rebase audit also reviewed four task-lifetime assertions introduced on
`dev`. Counter underflow and duplicate terminal-lifetime charging are
unrecoverable in every configuration, so those checks now use `require`
instead of disappearing in Shipping. The remaining moved task assertions are
pure observations whose exact allowlist identities were updated to their new
source locations.

Evaluation-count and failure-policy evidence passed in all three native
configurations. Debug and Release each passed the complete 1,131-test aggregate;
Shipping passed its complete 544-test aggregate, including the unconditional
`requiref(false)` death test. Shipping skips only configuration-inapplicable
ordinary-`check` death tests and the editor-only offline-compression Vulkan
case. Editor-only import and offline-build test sources now have exact,
rationale-bearing runtime exclusions, while all remaining runtime cases stay
registered. The DurinDevTool suite passed all 299 tests.

Full `all` builds passed for Debug Editor, Release Editor, Release Profiling
Editor, and Shipping Game. The public-header/shared-PCH fixture compiled in the
native aggregates without `MODULE_NAME`; `EnvironmentLightingBake` also now
receives the common configuration definitions. A Release Profiling Editor run
remained alive for 85 seconds and crossed automatic GC: the recorded collection
kept objects `120 -> 120`, marked all 120, swept none, and logged no assertion,
fatal error, or `FName::ToString` failure.

The plan is complete. A separately scoped build-system follow-up may isolate
test-only module exports from non-test preset output directories; today those
presets intentionally share final directories, so the final qualification
restored each test preset before its aggregate run. No assertion-semantics or
side-effect-audit question remains open.

## Goal

Give Debug, Release, and Shipping deliberate assertion semantics; make
assertion reporting usable from public headers and shared PCH compilation; and
ensure repository-owned C++ retains all required behavior when an assertion is
compiled out.

## Scope

- Keep the public configuration names `Debug`, `Release`, and `Shipping` while
  documenting Release as an optimized development build rather than the
  packaged distribution configuration.
- Enable `check` and `checkf` in Debug and Release and compile them out in
  Shipping.
- Add `verify` and `verifyf`, which evaluate their condition exactly once in
  every configuration and report failure only when `DO_CHECK` is enabled.
- Add `require` and `requiref` for unrecoverable contracts that evaluate once,
  report failure, and terminate in every configuration, including Shipping.
- Select and add a Debug-only spelling for expensive observational assertions.
- Route formatted and unformatted assertion failures through one
  assertion-owned interface that captures expression text and source location
  without depending on `MODULE_NAME` or public logging macros.
- Audit repository-owned `.h`, `.hpp`, `.inl`, `.cpp`, and C++
  scaffolding/template sources that emit supported assertion macros.
- Add deterministic scanning, classified findings, regression coverage, and a
  presubmit/build entrypoint for assertion side effects.

## Non-Goals

- Renaming Release to Development or adding a new CMake configuration.
- Replacing Durin assertion macros with the standard `<cassert>` `assert`,
  whose behavior is controlled separately by `NDEBUG`.
- Making ordinary `check`, `checkf`, or Debug-only checks execute in Shipping.
- Designing recoverable error-policy abstractions; `require` and `requiref`
  cover only unrecoverable contracts where continuing is invalid.
- Automatically converting every side-effecting assertion to `verify`; owners
  must determine whether failure can safely be ignored in Shipping.
- Auditing third-party, generated build-output, dependency, or binary sources.
- General static analysis unrelated to assertion-elision behavior.

## Design Decisions and Invariants

- Configuration names remain unchanged. Debug favors full diagnostics,
  Release combines optimization with development assertions, and Shipping is
  the diagnostic-culling distribution configuration.
- `DO_CHECK` is true in Debug and Release and false in Shipping. Boolean build
  switches are tested by value rather than mere macro presence.
- A disabled `check` or `checkf` is a structure-safe no-op and does not evaluate
  its condition, format arguments, or other diagnostic expressions.
- `verify` and `verifyf` evaluate their condition exactly once in all
  configurations. With checks enabled they use the same failure path as
  `check`; in Shipping they discard the result without reporting failure.
  `verifyf` format arguments remain diagnostic-only and are not evaluated in
  Shipping.
- Required work normally executes in an ordinary statement before an
  assertion. `verify` is reserved for an intentional operation whose execution
  is required and whose false result is safe to ignore when checks are
  disabled; it is not a shorthand for missing Shipping error handling.
- Assertion conditions and diagnostic arguments must not hide unrelated
  mutation, resource acquisition, task or callback execution, traversal,
  synchronization, or lifecycle transitions.
- `check` and `checkf` differ only by the optional formatted explanation. Both
  capture the failed expression and source location and enter one assertion
  failure policy.
- The assertion failure path has a stable assertion-owned identity and remains
  callable before logger initialization and during logger shutdown. It may
  integrate with the logger internally, but its public interface does not
  expand `DURIN_LOG`, `DURIN_FATAL`, or require `MODULE_NAME`.
- Assertion failure reports before debugger break or termination according to
  one documented policy; failure behavior is not selected accidentally by
  choosing formatted versus unformatted syntax.
- The scanner is conservative and deterministic. It may require human
  classification of a pure call, but may not suppress a call based only on
  spelling or `const`.
- Findings use stable repository-relative path and source-location identity.
  Allowlist entries are narrow, source-owned, and contain a rationale; wildcard
  file or directory exclusions are unsupported.
- Repairs preserve required execution count and do not evaluate a failed
  operation twice.

## Current Foundations and Gaps

- `Misc/Build.h` currently defines `DO_CHECK` only for Debug, and
  `AssertionMacros.h` tests it with `#ifdef`; Release therefore removes all
  assertion expressions.
- `AssertionMacros.h` currently sends only `checkf` through `DURIN_FATAL`.
  Unformatted `check` breaks directly, so their failure paths and diagnostic
  guarantees differ.
- `DURIN_FATAL` expands through `MODULE_NAME`. Ordinary module targets define
  that identity, but shared PCH targets deliberately apply only common runtime
  definitions, preventing assertion-bearing headers from being self-contained
  in every supported compilation context.
- No `verify`, `verifyf`, or agreed Debug-only expensive-check spelling exists.
- The previous GC and launch-loop repairs demonstrate that required traversal,
  cancellation, and waiting have already been hidden inside assertions.
- Targeted text search can identify candidates, but balanced macro arguments,
  nested expressions, macro expansion, overloads, and multiline source require
  syntax-aware classification for an enforceable gate.
- There is no check-disabled native-test configuration dedicated to Shipping
  behavioral parity and no tracked finding baseline.

## Implementation Stages

### Stage 0: Freeze the configuration and assertion contract

Dependencies: the motivating GC repair and its Release Profiling evidence.

- [x] Record the `Debug`/`Release`/`Shipping` meanings in the owning build
  documentation without introducing or renaming a configuration.
- [x] Freeze `DO_CHECK=1` for Debug and Release and `DO_CHECK=0` for Shipping,
  including preset, generated-target, program, test, and shared-PCH behavior.
- [x] Select one repository spelling for Debug-only expensive checks, including
  formatted and unformatted variants, without occupying the standard `assert`
  name.
- [x] Freeze exact evaluation, formatting, reporting, break, termination, and
  pre-initialization behavior for `require`, `requiref`, `check`, `checkf`,
  `verify`, `verifyf`, and the Debug-only checks.
- [x] Define when a false Shipping result is safe to ignore with `verify` and
  when explicit runtime error handling is mandatory.
- [x] Define source roots, extensions, macro spellings, generated-template
  policy, and third-party/build-output exclusions for the audit.

#### Acceptance Gate

- The owning build and C++ contracts describe every supported assertion macro
  in every configuration, the contract contains no unresolved failure-policy
  choice, and Release remains an existing CMake configuration.

### Stage 1: Unify assertion infrastructure and configuration behavior

Dependencies: Stage 0 contract.

- [x] Add the assertion-owned failure interface with expression text, optional
  formatted context, and source location, independent of `MODULE_NAME` and
  public logging macros.
- [x] Route `check` and `checkf` through the same failure policy and make all
  disabled expansions structure-safe.
- [x] Enable checks in Debug and Release, add `verify`/`verifyf`, and add the
  selected Debug-only expensive-check macros.
- [x] Add `require`/`requiref` as always-evaluated, always-enforced runtime
  contracts after owner approval during Stage 3.
- [x] Ensure conditions execute at most once and Shipping never evaluates
  disabled check conditions or formatted diagnostic arguments.
- [x] Add compile fixtures that exercise formatted assertions from a public
  header, the Core shared PCH, and a consumer module with no assertion-level
  dependence on module identity.
- [x] Add focused runtime tests for enabled failure reporting and configuration
  evaluation counts, including false `verify` results with checks disabled.

#### Acceptance Gate

- Compile fixtures pass with and without PCH, Debug and Release report through
  the same assertion path, Shipping evaluation-count tests match the frozen
  contract, and `checkf` can appear in a public header without `MODULE_NAME`.

### Stage 2: Implement the side-effect scanner and initial inventory

Dependencies: Stage 1 macro set and Stage 0 source contract.

- [x] Select and record the repository-owned syntax frontend and its handling
  of incomplete translation units, conditional compilation, and macro
  expansions.
- [x] Add a DurinDevTool-owned scanner entrypoint that detects direct calls,
  assignments, increments/decrements, allocation, deallocation, coroutine
  transitions, comma expressions, callbacks, traversals, and potentially
  overloaded operations within assertion conditions.
- [x] Analyze macro definitions and scaffolding templates separately from
  ordinary invocations.
- [x] Emit deterministic human-readable and machine-readable findings with
  macro, source location, construct kind, classification, and allowlist
  disposition.
- [x] Add fixtures for multiline and nested macros, templates, lambdas,
  preprocessor branches, false positives, parse failures, and path ordering.
- [x] Produce the initial inventory and classify every finding as required
  behavior, observational query, diagnostic-only work, intentional `verify`
  operation, unsafe ignored failure, or scanner limitation.
- [x] Record owning modules and validation targets for all findings requiring
  code changes.

#### Acceptance Gate

- Two consecutive runs produce byte-identical sorted findings, malformed or
  unparsed in-scope source fails visibly, fixtures pass, and every initial
  candidate has an owner and classification.

### Stage 3: Repair and migrate findings by subsystem

Dependencies: Stage 2 classified inventory.

- [x] Move required operations before observational assertions or into an
  enforced `require` contract, evaluate once, and preserve diagnostics.
- [x] Use `verify` only where execution is intentionally required and ignoring
  a false result in Shipping satisfies the Stage 0 contract.
- [x] Replace unsafe ignored failures with explicit runtime control flow or
  record them as separately bounded failure-policy work; do not hide them in a
  mechanical macro migration.
- [x] Move expensive pure checks to the Debug-only spelling only when measured
  or owner-reviewed cost justifies excluding them from optimized Release.
- [x] Add focused tests for GC traversal, task cancellation and waiting,
  registration, callbacks, resource publication, and every additional repaired
  behavior category.
- [x] Run check-enabled Debug/Release coverage and check-disabled
  Shipping-equivalent coverage for each affected subsystem.
- [x] Reduce the allowlist to reviewed observational or diagnostic-only
  expressions with source-local rationale.

#### Acceptance Gate

- No finding remains unclassified, required operations execute exactly once in
  all configurations, ignored Shipping failures are explicitly proven safe,
  and affected subsystem tests pass in their mapped configurations.

### Stage 4: Enforce parity and close the audit

Dependencies: Stage 3 repairs.

- [x] Add the scanner to presubmit/CI validation with zero unreviewed findings.
- [x] Move lasting assertion and side-effect rules into the owning C++ and
  build documentation and update scaffolding/templates that demonstrate
  assertions.
- [x] Run the complete native aggregate, full editor builds in the required
  configurations, Release Profiling lifecycle smoke across at least one
  automatic GC interval, and applicable Shipping validation according to the
  owning build instructions.
- [x] Record final findings, allowlist rationale, evaluation-count evidence,
  PCH/header evidence, and separately scoped follow-ups in this plan.
- [x] Mark the plan complete only after all acceptance gates pass and lasting
  contracts live outside the plan.

#### Acceptance Gate

- Presubmit rejects a newly added unreviewed side-effecting check fixture,
  permits a correctly classified `verify` fixture, all accepted allowlist
  entries are narrow and reviewed, complete validation passes, and Release
  Profiling survives automatic GC without reclaiming container-referenced live
  objects.

## Validation Matrix

| Boundary | Required evidence |
| --- | --- |
| Configuration identity | Existing Debug, Release, and Shipping names and output routing remain intact. |
| Configuration semantics | Debug and Release enable checks; Shipping disables checks but evaluates `verify` conditions once. |
| Failure path | `check` and `checkf` share reporting, break, and termination behavior with optional formatted context as their only distinction. |
| Header/PCH independence | A public header containing `checkf` compiles without `MODULE_NAME`, including shared-PCH compilation. |
| Disabled evaluation | Shipping does not evaluate `check` conditions or diagnostic format arguments. |
| Required evaluation | `verify` conditions and repaired ordinary statements execute exactly once in every configuration. |
| Source discovery | All repository-owned C++ and scaffolding roots are included; third-party and build outputs are excluded. |
| Macro parsing | Multiline, nested, templated, conditional, and lambda-containing assertions produce deterministic findings. |
| Side-effect detection | Calls, mutation, allocation, callbacks, traversal, and overloaded operations reach review. |
| Failure safety | A false result ignored in Shipping is owner-reviewed as safe; unsafe failures use explicit control flow. |
| GC regression | Reflected array and map references survive automatic collection in Release Profiling and Shipping-equivalent coverage. |
| Enforcement | A new unreviewed side-effecting assertion fails repository validation. |

## Definition of Done

- Release remains named Release and is documented and tested as an optimized
  development configuration with assertions enabled.
- All supported assertion macros have stable, configuration-specific evaluation
  and failure semantics independent of `NDEBUG`.
- Formatted and unformatted assertion failures use one path that works from
  public headers and shared PCH compilation without module identity.
- Every in-scope assertion condition is observational, an intentional and safe
  `verify` operation, or explicitly repaired.
- Required behavior and unsafe failure handling no longer depend on
  `DO_CHECK`.
- Deterministic tooling and CI prevent new unreviewed assertion side effects.
- Stable build and C++ documentation owns the final contract and the plan
  records complete validation evidence.

## Deferred Follow-ups

- Renaming Release to Development or adding a custom Development configuration.
- Recoverable error-policy redesigns that are larger than assertion migration.
- Third-party assertion auditing and upstream patch management.
- Purity annotations or whole-program effect inference beyond the conservative
  scanner.
- Isolating test-only module exports from ordinary build-preset output
  directories so switching between them never requires restoring the test
  preset's shared binaries.

## Related Documentation

- [C++ Coding Standards](../../../Development/Standards/CodingStandards.md)
- [Build and Run](../../../Development/Build/BuildAndRun.md)
- [Runtime Variants](../../../Development/Build/RuntimeVariants.md)
- [Native Tests](../../../Development/Build/NativeTests.md)
- [Runtime Lifecycle](../../../Runtime/Core/RuntimeLifecycle.md)

## Related Code

- `Engine/Source/Runtime/Core/Public/Misc/Build.h`
- `Engine/Source/Runtime/Core/Public/Misc/AssertionMacros.h`
- `Engine/Source/Runtime/Core/Public/Logging/LogMacros.h`
- `Engine/Source/Runtime/Core/Public/Logging/Logger.h`
- `CMake/Project/SharedPCH.cmake`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/GCReferenceSchema.cpp`
- `Engine/Source/Runtime/Launch/Private/LaunchEngineLoop.cpp`
- `Engine/Tests/Native/CoreDObjectTests/Private/ReflectionTypeTests.cpp`
- `Tools/DurinDevTool/`
