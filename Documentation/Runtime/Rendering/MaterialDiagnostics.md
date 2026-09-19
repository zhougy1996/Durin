# Material Diagnostics

Summary: Material operations propagate typed errors and context; presentation boundaries format diagnostics.
Modules: Engine, MaterialEditor, Renderer

## Error ownership

`FMaterialError` carries a discriminated error code. Expression, function, IR,
property, instance storage, cooked payload, compilation, and render validation failures have separate
error domains. Existing layout and parameter validation codes are reused.
An absent code, or an existing domain's `None`, means success.

`FMaterialOperationResult` returns success or an error without an error output
parameter. Existing data output parameters retain their operation-specific
publication behavior: encoders clear failed output, while property resolution and
cooked decoding publish candidates only after complete validation.

`FMaterialProgramDiagnostic` adds category, node/port identity, surface slot,
function path, and call path to the error. Normalization, source generation, and
compilation retain their diagnostic collections. The source-generation convenience
overload returns the full source-generation result.

## Context and formatting

Engine-owned failures must not carry user-facing prose. Preserve parameter identities, expected/actual value types, numeric indices,
archive field paths, and underlying archive, bulk-read, and package-resource status
as typed context. Cooked-program bulk-read rejection additionally owns the complete
`FPackageResourceReadError` in `ResourceCause`, preserving path, range, digest,
stream/task and system-error context without retaining the read buffer.
`FromBulkRead` copies that cause before the temporary read result is released.
Do not classify or branch on formatted text.

`FormatMaterialError` is the presentation entry point for editor UI, logging,
command output, and adapters to framework interfaces that still require strings.
The existing parameter/layout text lookups are implemented alongside it. Keep
error text out of validators, graph builders, codecs, and lifecycle state.

Shader compiler and shader-environment providers can return opaque external
diagnostics. `FMaterialError::FromExternal` preserves a bounded copy alongside a
provider-specific error code. This is not a general string-error constructor.

Object graph admission and object-aware archive serialization retain
`FMaterialObjectValidationCause` through `FObjectValidationResult`. Material instance
storage distinguishes incomplete drafts, invalid or duplicate parameter IDs, and
invalid sampling policies; failures retain the parameter ID and array index.
Package capture preserves the same cause after source repair. Object property-edit
hooks retain `FMaterialError` in `FEnginePropertyEditCause`; pending editor results,
plain Archive and Cook interfaces format explicitly at their string boundaries.

Base-material `SetParameterValue` returns `FMaterialOperationResult`. Invalid or
missing parameter IDs, definition validation, missing authored owners and owner
mismatches preserve parameter identity and fail before publishing values. Editor
parameter sessions format this error into their command message for apply and
cancellation failures.
Instance parameter mutation also returns `FMaterialOperationResult`, separating
missing definitions, declaration/override type conflicts, unreachable parameters
and invalid sampling policies. Type failures retain expected/actual parameter
types and all failures retain the requested parameter ID. Name-based setters return the same typed result and retain the requested name,
lookup/type failures and underlying mutation cause. Texture resolution failures
retain the parameter ID. Synchronous custom transactions still adapt to boolean
contracts.
Scene import formats the instance failure explicitly at its import diagnostic
boundary.

Parameter-expression definition writes return the same typed result. They report
expected/actual definition types, retain parameter-validation errors and reject
unsupported expression owners before updating metadata or values. Shared graph
parameter resolution formats this cause at the MaterialEditor command boundary;
widgets consume the resulting message. Engine validation continues to return
typed errors. See [material graph commands](../../Editor/Architecture/MaterialGraphOperations.md#compact-input-and-texture-authoring)
for the simpler editor status/message contract.

## Validation

Assert error codes and context in semantic tests, including failed-output
publication behavior. Assertions may format errors for failure reporting.
Text assertions belong to presentation tests, not validation contracts.

Relevant native targets include `MaterialCompilerTests`,
`MaterialCompileLifecycleTests`, `MaterialFunctionTests`, `MaterialRuntimeTests`,
and material editor/cook/package targets selected by `test affected`.
