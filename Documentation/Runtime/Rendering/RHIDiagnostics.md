# RHI Diagnostics

Summary: RHI and Vulkan validation return typed errors; presentation boundaries format diagnostics.
Modules: RHI, VulkanRHI, RenderCore

## Validation results

Validation interfaces use `std::expected<T, E>` with operation-specific errors.
There is no global RHI validation error union or universal result alias. Texture,
view, upload, and pipeline validation use their own error enums directly. Shader
binding errors retain binding locations and type-mismatch context. Batch
transition and copy errors retain element indices; copy operations combine only
the copy, region, and footprint causes that their diagnostics need.

Error detail stops at the boundary that consumes it. Internal access-shape checks
return `bool`; transition validation reports `InvalidAccess` in its own domain.
Pipeline admission reports `InvalidDescription` rather than publishing internal
validation details through the asynchronous request. Creation recovery errors
remain a separate contract because callers use their classification to recover.

Pipeline `IsValid()` predicates share the checks used by key builders without
constructing or canonicalizing a key. Key builders return a validated key or the
pipeline-specific enum. `GetBufferTextureCopyFootprint` returns the byte count as
`std::expected<uint64, ERHICopyFootprintError>`; no output is published on failure.
Other data output parameters retain their existing publication behavior. Binding visitors can have visited a valid prefix before
failure.

`ToString` overloads format the relevant enums and context structures at
assertions, logs, and other presentation boundaries. Read `error()` only after
failure. Semantic tests assert codes and context, not English substrings. Do not
add a general message constructor or use formatted text for branching.

Validation error types and their `ToString` declarations live next to the owning
interfaces in `RHIResources.h` and `RHIShaderParameters.h`. Implementations are
centralized in `RHIErrorStrings.cpp`. Enum overloads return static diagnostic
descriptions as `std::string_view`; context overloads return an owning
`std::string` containing the description and available context fields.

## Creation and executor failures

`FRHICreationError` is declared in `RHIResources.h` alongside its creation
failure source and `ToString` overload. It retains the recovery classification,
failure source, and optional native status. Its diagnostic implementation lives
in `RHIErrorStrings.cpp`; semantic fingerprinting lives in `RHIResources.cpp`.
Both synchronous operation results and asynchronous pipeline publications
preserve it. `RHITryCreateTexture` and `RHITryCreateBuffer`
return `std::expected<ResourceRef, FRHICreationError>` without logging recoverable
failures. Success contains a non-null resource. RDG consumes these results for
rollback and retry suppression, retaining the complete native status.

`RHICreateTexture` and `RHICreateBuffer` are nullable convenience boundaries:
they log a recoverable failure once and return null. Vulkan's
`TryCreateVulkanResource` translates recoverable exceptions and unexpected null
factory returns into errors; its nullable `CreateVulkanResource` adapter owns
diagnostics for other factories. Recovery callers decide when a failure should
be presented. Device and invariant failures remain terminal exceptions outside
the recoverable contract. Cache exhaustion remains explicitly classified.

`FRHIThreadWorkResult` carries a typed thread error. Unknown exceptions have a
code; opaque exception-boundary text uses `FromExternalException`, which retains
at most 4096 bytes. Thread state, synchronous results, and statistics snapshots
retain the typed error; only presentation callers format it. The `ToString`
overload is declared in `RHIThread.h` and implemented in `RHIErrorStrings.cpp`. Initialization
rollback failures are logged separately and do not replace or extend the primary
initialization error. Validation-layer callback messages remain external diagnostics.

## Vulkan configuration

Instance negotiation retains `FVulkanInstanceNegotiationError` in
`VulkanExtensions.h`, with an operation-specific code, requirement identifier,
and numeric version context. Physical-device evaluation in `VulkanDevice.h`
retains human-readable rejection reasons instead of structured error codes.
An empty rejection list means a candidate is suitable; callers never parse the
text. Evaluation does not log, since rejecting one candidate can be normal.
If all candidates are unsuitable, the startup boundary logs each device and
reason directly, then throws a short initialization failure. There is no
aggregate device-report formatter or duplicated candidate arrays.

Swapchain selection uses `EVulkanSwapchainSelectionError` in `VulkanSwapchain.h`
and returns `std::expected<FVulkanSwapchainConfiguration, EVulkanSwapchainSelectionError>`.
It publishes a configuration only on success. There is no universal Vulkan
error type or result alias. The infallible instance extension request builder
returns data directly.

`ToString` declarations live beside their error types; implementations are
centralized in `VulkanErrorStrings.cpp`. Enum conversions return static
`std::string_view` descriptions and context conversions return `std::string`.
`FormatVulkanInstanceNegotiationErrors` joins instance rejection diagnostics at
the startup exception boundary. Formatted text is not a success indicator.
