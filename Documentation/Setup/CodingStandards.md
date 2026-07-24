# C++ Coding Standards

These conventions apply to repository-owned C++ source. Generated code and third-party code follow their generators or upstream projects and are not migration targets.

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

## Incremental Adoption

New and materially modified code must follow these conventions immediately. Do not mix unrelated repository-wide formatting or annotation cleanup into a functional change.

Handle existing violations through a dedicated active plan under `Documentation/Plans/`. Inventory the affected files first, then split migration stages by one convention and a bounded module or dependency layer. Each stage must leave the repository buildable, use focused validation appropriate to the touched modules, and land as an independent commit. Complete a full `all` build after the final stage or earlier when a cross-module export change requires it.
