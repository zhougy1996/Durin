# Native Function Reflection

Summary: Define generated native function descriptors, inheritance, and checked no-argument invocation.

Modules: CoreDObject

Last reviewed: 2026-10-15

## Authoring And Scope

`DFUNCTION()` on an ordinary method in a reflected `DCLASS` registers a native
callable. Its class requires `GENERATED_BODY()`. The supported signature is a
concrete, non-static, non-const, unqualified `void Method()`. Public, protected,
private, virtual, `final`, and `noexcept` methods are supported. Parameters (even
defaulted), return values, ref qualifiers, pure-virtual annotated declarations,
templates, variadics, declaring-class overloads, free/static functions, and
`DSTRUCT` methods are rejected. The definition must link normally.

```cpp
DFUNCTION(CallInEditor, Category = "Light", DisplayName = "Normalize Settings",
    ToolTip = "Clamp authored light settings to supported values.")
auto NormalizeSettings() -> void;
```

`CallInEditor` opts into the
[Details action adapter](../../Editor/Architecture/EditorFunctionActions.md).
It grants no scripting, remote, console, or network exposure, does not remove
the declaration from game builds, and adds no editor module dependency. Typed
presentation metadata comprises `Category`, `DisplayName`, and `ToolTip` with
quoted strings. Category defaults to `Actions`; omitted display names use
`StringUtils::HumanizeName`. Unknown/duplicate specifiers fail generation.
Metadata uses flat macro arguments.

## Ownership And Invocation

`DClass` owns immutable local `FFunction` descriptors independently of
`DStructBase::ChildProperties`. Functions do not enter property serialization,
default comparison, parameter storage, or GC property schemas. Each descriptor
owns its presentation strings and refers to its declaring class, name, flags,
and native invoker. Its signature is currently
always `EFunctionSignature::VoidNoArgs`.

`DurinCodeGen::FClassParams` accepts a trailing function pointer/count pair.
Generated statics helpers make type-correct casts and ordinary member calls;
the existing generated friend grants access to non-public methods. There is no
pointer-to-member erasure, VM, or `ProcessEvent` ABI. Registration validates
local names and inherited virtual compatibility before publishing the list.

`FFunction::InvokeVoidNoArgs` checks target liveness, class compatibility, and
invoker availability. `EFunctionInvokeError::None` means the native call returned;
it does not convey domain success from a `void` body. Runtime invocation opens
no transactions and applies no editor exposure policy. Native exceptions are
not a recoverable action result or rollback mechanism.

## Inheritance And Lookup

`FindFunctionByName` returns the nearest local descriptor, optionally searching
ancestors. `ForEachFunction` returns one effective descriptor per reflected slot,
ordered by root declaring qualified class name, then method name. Excluding
ancestors enumerates local descriptors only. A slot's session identity is its
root declaring qualified class name and method name; `GetRootFunction()` returns
that root descriptor.

Root lookup resolves the parent class's same-name descriptor when queried after
registration publication. It does not cache a possibly absent parent descriptor
during local registration; derived-before-base registration within a module batch
therefore preserves the same slot identity. It introduces no eager parent-helper
calls or additional class-registration dependency cycles.

A base action is inherited without another annotation. Unannotated native
virtual overrides preserve the slot and metadata and execute by ordinary virtual
dispatch. Generated calls never explicitly qualify the base implementation.
Annotated compatible virtual overrides replace the effective descriptor; their
annotation is complete, so omitted presentation uses defaults and omitting
`CallInEditor` removes the derived button. Non-virtual same-name hiding and
overloads against a reflected ancestor name are rejected in configured reflected
headers, including unannotated hiding. Diagnostics identify both locations.

Descriptors and executable invokers follow the owning class/module lifetime.
Consumers must not retain them after module retirement. Function identities are
session-only, never serialized. Persistent bindings, reload, arguments, return
storage, scripting, and RPC require separate contracts.

## Related Documentation

- [Reflection generation](../../Development/Tooling/ReflectionGeneration.md)
- [Generated reflection system](ReflectionSystem.md)
