# RHI Diagnostics

Summary: RHI and Vulkan validation return typed errors; presentation boundaries format diagnostics.
Modules: RHI, VulkanRHI, RenderCore

## Validation results

`FRHIOperationResult` returns success or an `FRHIError`. The error code is a
discriminated union of validation domains: access, bindings, pipelines,
transitions, views, copies, texture creation, and uploads. `std::monostate`
means success. Results do not carry message strings or an independent success
flag. Batch and binding validation retain numeric locations where available.

Data output parameters retain their operation-specific publication behavior;
the result replaces the error output parameter, not the data outputs. In
particular, pipeline keys and swapchain configurations publish only validated
candidates. Binding visitors can have visited a valid prefix before failure.

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
Swapchain selection returns `FVulkanOperationResult`. The infallible instance
extension request builder returns data directly. `FormatVulkanError` and
`FormatVulkanErrors` are presentation adapters, including the existing startup
exception boundary; text emptiness does not determine operation success.
