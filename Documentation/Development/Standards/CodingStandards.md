# C++ Coding Standards

These conventions apply to repository-owned C++ source. Generated code and third-party code follow their generators or upstream projects and are not migration targets.

## Includes

`CoreStd.h` supplies common STL headers. Add another standard-library header
only when the translation unit requires it.

## Comments

Comments explain intent, contracts, invariants, ownership, or non-obvious tradeoffs. Do not translate the declaration or implementation into prose, narrate control flow, or add comments only to satisfy a coverage target. Prefer a concise comment immediately before the declaration it describes, and keep the comment accurate when the design changes.

### Types

Precede each key class, struct, or enum with a concise comment that states its role and important boundary. Key types include:

- public interfaces;
- module or subsystem entry points;
- ownership and lifetime coordinators;
- reflected types;
- types that define a domain contract or carry state with non-obvious invariants.

Do not merely restate the type name. For reflected types, place the comment before `DCLASS()`, `DSTRUCT()`, or `DENUM()` so it introduces the complete declaration.

```cpp
// Owns the runtime world and coordinates its actor lifetime.
DCLASS()
class DWorld : public DObject
{
	GENERATED_BODY()
};
```

### Functions

Comment a function when callers need information that its name and signature do not convey. This includes non-obvious preconditions or postconditions, ownership or lifetime effects, thread-safety requirements, units or coordinate spaces, failure behavior, externally visible side effects, and intentional performance tradeoffs.

Public or protected API functions require comments when they establish such a contract. Private functions require comments only when their purpose or constraints remain non-obvious after clear naming and decomposition. Simple accessors, self-explanatory operations, and overrides whose behavior is fully defined by the base contract do not need repetitive comments.

### Data Members and Enum Values

Comment a data member when its meaning, valid range, unit, coordinate space, ownership, nullability, lifetime, synchronization rule, cache invalidation rule, or relationship to other state is not evident from its type and name. Comment an enum value when its behavior or distinction from neighboring values is not self-explanatory. A single comment may introduce a closely related group when it states a shared invariant clearly.

For a reflected member, place its comment before `DPROPERTY()` so the comment introduces the complete reflected member unit.

```cpp
// Fixed output aspect ratio used only when AspectRatioMode is Fixed.
DPROPERTY(Edit)
float CustomAspectRatio = 16.0f / 9.0f;
```

Do not add comments such as `// Width` above a member named `Width`. Rename an unclear member before compensating with prose, unless an external schema or compatibility contract fixes the name.

## Reflected Member Spacing

Within a reflected class or struct, treat an optional leading comment, the reflection macro, and the declaration it annotates as one member unit. Leave one blank line between consecutive reflected member units.

```cpp
DPROPERTY(Edit)
float FieldOfView = 60.0f;

DPROPERTY(Edit)
float NearClipPlane = 0.1f;
```

Do not insert a blank line between a reflection macro and its declaration.

## Module Export Macros

Do not place a module export macro such as `ENGINE_API` on a class or struct name. Apply it to each function that must be exported across the module boundary. This includes constructors, destructors, and operators when they cross that boundary. Leave non-exported and inline functions unannotated.

```cpp
class FViewport
{
public:
	ENGINE_API FViewport();
	ENGINE_API ~FViewport();

	ENGINE_API auto Resize(uint32 Width, uint32 Height) -> void;

	auto GetWidth() const -> uint32 { return Width; }
};
```

## Assertions

Durin assertions are statement macros and are independent of the standard
`assert`/`NDEBUG` contract. Use only the following repository spellings:

| Macro | Debug | Release | Shipping |
| --- | --- | --- | --- |
| `require(condition)` | Evaluate once; report and terminate on false | Evaluate once; report and terminate on false | Evaluate once; report and terminate on false |
| `requiref(condition, format, ...)` | As `require`, with formatted context | As `require`, with formatted context | As `require`, with formatted context |
| `check(condition)` | Evaluate once; report on false | Evaluate once; report on false | Do not evaluate |
| `checkf(condition, format, ...)` | As `check`, with formatted context | As `check`, with formatted context | Evaluate neither condition nor format arguments |
| `verify(condition)` | Evaluate once; report on false | Evaluate once; report on false | Evaluate once; discard the result |
| `verifyf(condition, format, ...)` | As `verify`, with formatted context | As `verify`, with formatted context | Evaluate the condition once; do not evaluate format arguments |
| `checkSlow(condition)` | Evaluate once; report on false | Do not evaluate | Do not evaluate |
| `checkfSlow(condition, format, ...)` | As `checkSlow`, with formatted context | Evaluate neither condition nor format arguments | Evaluate neither condition nor format arguments |

Enabled conditions execute at most once. `require` and `requiref` are never
disabled: they express an unrecoverable runtime contract that must succeed
before execution can continue in every configuration. Formatted arguments are diagnostic
only: they execute only after an enabled condition is false. Disabled macros
are structure-safe no-op statements and do not evaluate their arguments.
`checkSlow` and `checkfSlow` are reserved for expensive observational
validation whose cost has been measured or reviewed as unsuitable for
optimized Release builds; they are not a way to hide required work.

All enabled assertion failures capture the condition text and source location,
then use the same assertion-owned failure path. That path synchronously emits a
best-effort diagnostic with optional formatted context, remains usable before
logger initialization and after logger shutdown, and does not require
`MODULE_NAME` or a public logging macro. A formatting or logging failure falls
back to the unformatted condition and source location. After reporting, the
failure path triggers the platform debugger break and, if execution continues,
unconditionally terminates the process. Formatted and unformatted macros have
no failure-policy difference.

Required behavior normally belongs in an ordinary statement before the
assertion, with its result stored once for observation. Use `verify` or
`verifyf` only when all of these are true:

- evaluating the condition performs one intentional operation required in
  every configuration;
- a false result in Shipping can be ignored without invalidating state,
  leaking resources, skipping cleanup or synchronization, or making later code
  assume success; and
- no rollback, retry, return, exception, cancellation, or other control flow is
  required to continue safely.

If a false result is unrecoverable and execution must not continue in any
configuration, use `require` or `requiref`. Recoverable failures still require
ordinary explicit runtime error handling. Assertion
conditions and diagnostic arguments must not hide unrelated mutation,
allocation, resource acquisition, callbacks, traversal, task execution,
synchronization, or lifecycle transitions.

## Incremental Adoption

New and materially modified code must follow these conventions immediately. Do not mix unrelated repository-wide formatting or annotation cleanup into a functional change.

Handle existing violations through a dedicated active plan under `Documentation/Plans/`. Inventory the affected files first, then split migration stages by one convention and a bounded module or dependency layer. Each stage must leave the repository buildable, use focused validation appropriate to the touched modules, and land as an independent commit. Complete a full `all` build after the final stage or earlier when a cross-module export change requires it.
