# Material Diagnostics

Summary: Material operations propagate typed errors and context; presentation boundaries format diagnostics.
Modules: Engine, MaterialEditor, Renderer

## Error ownership

`FMaterialError` carries a discriminated error code. Expression, function, IR,
property, cooked payload, compilation, and render validation failures have separate
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
as typed context.
Do not classify or branch on formatted text.

`FormatMaterialError` is the presentation entry point for editor UI, logging,
command output, and adapters to framework interfaces that still require strings.
The existing parameter/layout text lookups are implemented alongside it. Keep
error text out of validators, graph builders, codecs, and lifecycle state.

Shader compiler and shader-environment providers can return opaque external
diagnostics. `FMaterialError::FromExternal` preserves a bounded copy alongside a
provider-specific error code. This is not a general string-error constructor.

Object graph loading, property editing, archive serialization, and Cook framework
callbacks retain their existing signatures. Their material-side adapters format
structured errors at the boundary; they do not require a framework-wide API change.

## Validation

Assert error codes and context in semantic tests, including failed-output
publication behavior. Assertions may format errors for failure reporting.
Text assertions belong to presentation tests, not validation contracts.

Relevant native targets include `MaterialCompilerTests`,
`MaterialCompileLifecycleTests`, `MaterialFunctionTests`, `MaterialRuntimeTests`,
and material editor/cook/package targets selected by `test affected`.
