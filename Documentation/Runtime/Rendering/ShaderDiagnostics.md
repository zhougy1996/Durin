# Shader Diagnostics

Summary: Shader validation, compilation, and cooked-library operations return structured errors; presentation boundaries format them.
Modules: RenderCore, ShaderBuild, Engine, Launch, Renderer, MonaImGui, TextureEditor

## Validation results

`FShaderOperationResult` owns an `FShaderError`. `EShaderError::None` means
success; the result has no independent success flag or error output parameter.
Parameter binding, pipeline reflection merging, ShaderMap initialization, and
MaterialShaderMap construction preserve errors through nested calls. Compiled
payload (`DSHD`) encoding and decoding use the same result contract; ShaderBuild
DDC consumers format rejection details only when logging.

`FShaderCompilerOutput` also derives success from its error code. A default
output reports `CompilationNotStarted`; successful compilation and decoding
explicitly publish a success code. Partial compiled stages remain unusable when
the output carries an error.

Errors retain shader and parameter identities, expected and actual values,
binding locations, and conflicting push-constant ranges. Validators do not
construct presentation text. `FormatShaderError` formats diagnostics at logging,
assertion, and existing string-based adapter boundaries.

## Output publication

Parameter binding clears its output before validation and may retain a valid
prefix on failure. Pipeline layout construction clears its output and publishes
the merged layout only after validation. ShaderMap initialization resets failed
maps. MaterialShaderMap construction clears its output and publishes the complete
candidate only after validation. Changing the error transport does not alter
these contracts. Payload encoding and decoding clear failed output and publish
only complete values; the binary schema and cache keys are unchanged.

## Compiler and provider boundaries

ShaderBuild validation, source manifests, fingerprints, generated imports, Cook
input capture, and provider calls preserve typed results. The provider interface
version is 5. Missing providers, failed visitors, nested captures, and cancellation
have explicit codes. Modular-feature invocation failures also retain their status
and matching-provider count.

`FShaderError::FromSlang` retains the compiler phase, optional native status, and
at most 4096 bytes of opaque compiler diagnostics. Filesystem errors preserve
`std::error_code`; file reads preserve `FFileIoError`. The existing Core
fingerprint interface still supplies text, so `FromFileFingerprint` retains its
path and a bounded diagnostic at that adapter. None of these strings determine
success or error classification.

Material and Cook framework adapters format the structured result only where
their existing contracts require text.

## Cooked requests and libraries

Registration returns an operation result and writes its owning registration to
the data output. Failure clears that output. Retirement after inventory freeze
fails without discarding the registration handle. Inventory, runtime identity,
library encoding, opening, and loading propagate the original structured cause,
including compiled-payload validation errors.

Library and payload schemas, identities, and cache keys are unchanged. Existing
clear-on-failure and candidate-publication behavior remains in force.

## Validation

Semantic tests assert error codes, context, and output publication. English
text assertions belong to diagnostic formatting tests. Relevant CPU targets are
`RenderShaderContractTests`, `RenderShaderCacheTests`, `RenderShaderServiceTests`,
`RenderShaderCookedLibraryTests`, and `RenderShaderCookIntegrationTests`. Shared
API migrations also require the workspace `all` build and affected consumer tests.
