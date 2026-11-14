# Unified Error Collection

Summary: Small owning diagnostics preserve failure identity and context across module and operation boundaries.
Modules: Core

`Misc/UnifiedError.h` defines `Durin::UnifiedError::FError`. An error owns a
nonempty domain and code, an optional message, and ordered key/value context.
`FErrorId` declares a domain/code pair; `Is` and equality compare identity only.
Messages and context are evidence for presentation, not branch conditions.
An empty message is still an error. Absence is represented by `std::optional`
or by the operation's success state; `FError` has no success/default value.

Copies share data allocated and destroyed in Core. `PushContext` detaches a
shared copy before appending, so an outer operation cannot change a sibling's
diagnostic. Domain names, codes, messages and context are owned, including
diagnostics produced by a plugin that subsequently unloads. Getter references
and spans borrow from the error; do not retain them across mutation or destruction.
Independent copies can be passed between tasks; concurrent mutation of the same
error object requires synchronization.

Keep native domain states authoritative. A write result still describes write
effects, a Cook result still describes Cook admission, and a streamable handle
still describes request completion. An optional `FError` retains the original
cause when adapting between these domains. Do not infer status from text or
flatten a cause before the receiving operation has made its domain decision.
`ToString` and the formatter render domain/code even when the message is empty.

Ordinary [asset loads](../Assets/Packages/AssetPackages.md) return nullable
pointers, with an optional diagnostic output for consumers that need the cause.
Internal transactional/status operations may use `std::expected<T, FError>` to
propagate failures. The diagnostic type does not require that return convention
for every API and does not introduce a global error enumeration, logging policy,
exception system, or operation recovery metadata.
