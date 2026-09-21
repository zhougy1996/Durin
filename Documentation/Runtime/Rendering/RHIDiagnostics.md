# RHI Diagnostics

Summary: RHI and Vulkan validation return typed errors; presentation boundaries format diagnostics.
Modules: RHI, VulkanRHI, RenderCore

## Validation results

`TRHIResult<T>` is an alias for `std::expected<T, FRHIError>`;
`FRHIOperationResult` names its `void` specialization for validation without a
value. Success is represented by the expected value, not an error sentinel.
`FRHIError` retains a discriminated union of validation domains and numeric
locations for batch and binding failures. Read `error()` only after failure.

Pipeline key builders return the validated key as the expected value. Other
data output parameters retain their operation-specific publication behavior;
for example, binding visitors can have visited a valid prefix before failure.

`FormatRHIError` formats errors at assertions, logs, and other presentation
boundaries. Semantic tests assert codes and context, not English substrings.
Do not add a general message constructor or use formatted text for branching.

## Creation and executor failures

`FRHICreationError` retains the recovery classification, failure source, and
optional native status. Both synchronous operation results and asynchronous
pipeline publications preserve it. `RHICreateTexture` and `RHICreateBuffer`
return nullable resources and accept an optional error output pointer. Ordinary
callers only inspect the resource; Vulkan factories log recoverable failures
locally. Recovery callers retain the complete error, including native status,
through RDG rollback and retry suppression. Cache exhaustion is classified explicitly;
Vulkan candidate failures retain their native result code. Device and invariant
failures remain terminal exceptions, outside the recoverable creation contract.

`FRHIThreadWorkResult` carries a typed thread error. Unknown exceptions have a
code; opaque exception-boundary text uses `FromExternalException`, which retains
at most 4096 bytes. Thread state, synchronous results, and statistics snapshots
retain the typed error; only presentation callers format it. Initialization
rollback failures are logged separately and do not replace or extend the primary
initialization error. Validation-layer callback messages remain external diagnostics.

## Vulkan configuration

Instance negotiation and device evaluation retain structured rejection lists,
including requirement identifiers and numeric version or limit context.
Swapchain selection returns `TVulkanResult<FVulkanSwapchainConfiguration>`,
an alias of `std::expected<T, FVulkanError>`, and publishes a configuration only
on success. The infallible instance
extension request builder returns data directly. `FormatVulkanError` and
`FormatVulkanErrors` are presentation adapters, including the existing startup
exception boundary; text emptiness does not determine operation success.
