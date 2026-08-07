# Assertion Side-Effect Audit Plan

Summary: Detect and remove repository-owned C++ behavior that disappears when `check` and `checkf` compile out of non-Debug builds.

Last reviewed: 2026-08-07

Status: Active
Completed:

## Current Status

The plan is active after a Release Profiling editor crash exposed two GC
container traversals embedded inside `checkf`. Debug executed both traversals,
while Release Profiling compiled them out, failed to mark six live objects, and
later crashed in `FName::ToString`. The immediate GC defect is repaired by
evaluating array and map traversal before asserting their results. No
repository-wide inventory or prevention gate exists yet. The focused compiled
reference-schema test passed, the complete `Win64-Release-DurinEditor-Profiling`
`all` build passed, and an 80-second visible-editor validation crossed the
automatic GC interval with 120 objects retained, zero candidates, and zero
swept objects.

## Goal

Ensure repository-owned C++ has identical required side effects whether
`DO_CHECK` is enabled or disabled. Every `check` and `checkf` condition must be
observational only: compiling it out may remove validation and diagnostic
formatting, but must not remove state mutation, resource acquisition, task or
callback execution, traversal, synchronization, or lifecycle transitions.

## Scope

- Repository-owned `.h`, `.hpp`, `.inl`, `.cpp`, and C++ scaffolding/template
  sources that can emit `check` or `checkf` invocations.
- A deterministic scanner that reports assertion conditions containing calls,
  assignments, increment/decrement, allocation/deallocation, coroutine
  transitions, or overloaded operations that require human classification.
- Review and repair of every finding that owns required behavior.
- Regression coverage comparing check-enabled and check-disabled execution for
  representative lifecycle, container traversal, cancellation, registration,
  and resource-publication paths.
- A presubmit/build entrypoint that prevents newly introduced unreviewed
  assertion side effects.

## Non-Goals

- Changing the public semantics of `check`, `checkf`, `DO_CHECK`, fatal logging,
  or Shipping diagnostics.
- Keeping assertion expressions enabled in Release or Shipping builds.
- Automatically rewriting findings without subsystem-owner review.
- Treating pure queries, comparisons, casts, and diagnostic-only formatting as
  runtime behavior merely because they appear in an assertion condition.
- Auditing third-party, generated build-output, dependency, or binary sources.

## Design Decisions and Invariants

- Required work executes in an ordinary statement before the assertion. The
  assertion observes a stored result or postcondition; it never owns execution.
- The scanner is conservative. It may require classification of a pure call,
  but it must not silently suppress a call based only on spelling or `const`.
- Findings use stable repository-relative path and source-location identity.
  Any allowlist entry records a narrow rationale and an owning source location;
  broad file or directory exclusions are unsupported.
- Scanner output is deterministic and sorted so local and CI results agree.
- Macro definitions and generated-code templates are analyzed separately from
  ordinary invocations so the audit does not confuse assertion implementation
  with assertion use.
- Repairs preserve failure behavior when checks are enabled and preserve
  required behavior when checks are disabled. A failed operation must not be
  evaluated twice.
- The audit covers both direct side effects and callbacks/traversals whose side
  effects occur through a called API.

## Current Foundations and Gaps

- `AssertionMacros.h` intentionally expands `check` and `checkf` to empty macros
  when `DO_CHECK` is disabled, so conditions cannot own required work.
- `LaunchEngineLoop.cpp` previously required the same repair for task
  cancellation and admission-probe waiting in Release Profiling.
- `GCReferenceSchema.cpp` now evaluates `VisitElements` and `VisitEntries`
  before checking their results; the existing compiled-reference-schema test
  exercises nested arrays and maps when checks are enabled.
- Targeted text search can identify candidates, but balanced macro arguments,
  nested expressions, macro expansion, overloads, and multiline source require
  syntax-aware classification for an enforceable gate.
- The repository has no check-disabled native-test configuration dedicated to
  behavioral parity and no tracked finding baseline.

## Implementation Stages

### Stage 0: Freeze the audit contract and inventory candidates

Dependencies: the immediate GC repair and its Release Profiling reproduction
evidence.

- [ ] Define the exact source roots, extensions, macro spellings, generated
  template policy, and third-party/build-output exclusions.
- [ ] Select the repository-owned syntax frontend and record how incomplete
  translation units, conditional compilation, and macro expansions fail.
- [ ] Produce a deterministic initial candidate inventory and classify every
  finding as required behavior, observational query, diagnostic-only work, or
  scanner limitation.
- [ ] Record owning modules and validation targets for required-behavior
  findings before editing them.
- [ ] Freeze the narrow allowlist schema and prohibit wildcard suppressions.

#### Acceptance Gate

- The same checkout produces byte-identical sorted findings on two consecutive
  runs, every candidate has one classification, and scanner parse failures are
  visible failures rather than silently skipped files.

### Stage 1: Implement the assertion-side-effect scanner

Dependencies: Stage 0 contract and classified inventory.

- [ ] Add a DurinDevTool-owned scanner entrypoint using the selected syntax
  frontend and repository path/exclusion contract.
- [ ] Detect direct calls, assignments, increments/decrements, allocation,
  deallocation, coroutine transitions, comma expressions, and overloaded
  operations within assertion conditions.
- [ ] Emit stable human-readable and machine-readable findings with macro,
  source location, construct kind, and allowlist disposition.
- [ ] Add scanner fixtures for multiline/nested macros, templates, lambdas,
  preprocessor branches, false positives, parse failures, and path ordering.
- [ ] Integrate the scanner into the repository documentation/tooling contract
  without requiring a compiler build for source-only validation.

#### Acceptance Gate

- Scanner fixtures pass on supported hosts, the initial inventory is reproduced
  without missing classified findings, and malformed or unparsed in-scope
  source causes an actionable nonzero result.

### Stage 2: Repair required-behavior findings by subsystem

Dependencies: Stage 1 scanner and Stage 0 owner/validation mapping.

- [ ] Move each required operation before its assertion, store its result once,
  and retain the existing enabled-check failure condition and message.
- [ ] Audit surrounding control flow for failures that Release previously
  ignored, recording any required non-assertion recovery as a separate behavior
  change rather than hiding it in the mechanical repair.
- [ ] Add focused tests for GC reference traversal, task cancellation/waiting,
  registration, callbacks, resource publication, and every other repaired
  behavior category represented by findings.
- [ ] Run check-enabled tests and check-disabled Release/Shipping-equivalent
  coverage for each affected module.
- [ ] Reduce the allowlist to reviewed observational or diagnostic-only
  expressions with source-local rationale.

#### Acceptance Gate

- No scanner finding remains unclassified, required operations execute exactly
  once with checks both enabled and disabled, and all affected subsystem tests
  pass in their mapped configurations.

### Stage 3: Enforce parity and close the audit

Dependencies: Stage 2 repairs and validation.

- [ ] Add the scanner to the presubmit/CI validation path with zero unreviewed
  findings permitted.
- [ ] Document the assertion-side-effect rule in the owning C++ standards and
  update scaffolding/templates that demonstrate assertions.
- [ ] Run the complete native aggregate, full editor build, Release Profiling
  lifecycle smoke across at least one automatic GC interval, and the applicable
  Shipping build validation.
- [ ] Record final findings, allowlist rationale, validation evidence, and any
  separately scoped follow-ups in this plan.
- [ ] Mark the plan complete only after long-lived rules live in owning
  documentation and all acceptance gates pass.

#### Acceptance Gate

- Presubmit rejects a newly added side-effecting assertion fixture, all accepted
  allowlist entries are narrow and reviewed, full validation passes, and Release
  Profiling survives automatic GC without reclaiming container-referenced live
  objects.

## Validation Matrix

| Boundary | Required evidence |
| --- | --- |
| Source discovery | All repository-owned C++ and scaffolding roots are included; third-party and build outputs are excluded. |
| Macro parsing | Multiline, nested, templated, conditional, and lambda-containing assertions have deterministic results. |
| Side-effect detection | Calls, mutation, allocation, callbacks, traversal, and overloaded operations reach review. |
| Classification | Every finding is repaired or narrowly allowlisted with rationale and owner. |
| Enabled checks | Existing assertion failure behavior and diagnostic messages remain intact. |
| Disabled checks | Required work executes once when assertion conditions compile out. |
| GC regression | Reflected array/map references survive automatic collection in Release Profiling. |
| Enforcement | A new unreviewed side-effecting assertion fails the repository validation entrypoint. |

## Definition of Done

- Every in-scope assertion condition is observational or explicitly reviewed.
- Required operations no longer depend on `DO_CHECK` for execution.
- Check-enabled and check-disabled regression coverage exercises all repaired
  behavior categories.
- Deterministic tooling prevents new unreviewed assertion side effects.
- Stable C++ documentation owns the rule and the plan records complete evidence.

## Deferred Follow-ups

- General static analysis unrelated to assertion-elision behavior.
- Third-party source auditing and upstream patch management.
- Replacing assertions with recoverable runtime error handling where subsystem
  contracts require a separate failure-policy design.
- Purity annotations or whole-program effect inference beyond the conservative
  assertion scanner.

## Related Documentation

- [C++ Coding Standards](../Development/Standards/CodingStandards.md)
- [Build and Run](../Development/Build/BuildAndRun.md)
- [Native Tests](../Development/Build/NativeTests.md)
- [Runtime Lifecycle](../Runtime/Core/RuntimeLifecycle.md)

## Related Code

- `Engine/Source/Runtime/Core/Public/Misc/AssertionMacros.h`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/GCReferenceSchema.cpp`
- `Engine/Source/Runtime/Launch/Private/LaunchEngineLoop.cpp`
- `Engine/Tests/Native/CoreDObjectTests/Private/ReflectionTypeTests.cpp`
- `Tools/DurinDevTool/`
