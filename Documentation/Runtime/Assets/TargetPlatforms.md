# Target Platform Build Policy

Summary: Resolve explicit tool targets into immutable capabilities while keeping family recipes, cache identity and Cook publication with their owners.

Modules: Core, RHI, TargetPlatform, Engine, ShaderBuild, TextureCompressor

Last reviewed: 2026-10-16

## Identities and dependencies

Core's `Misc/TargetIdentity.h` declares stable platform/profile enum identities,
fallible name parsing, and explicit Cook/Shader adapters. Existing distinct enum
types and disk numbers remain unchanged: invalid/unknown is 0, Win64 is 1,
MacOS is 2; profiles Game and EditorValidation are 1 and 2. Unknown values never
select Win64 implicitly. Generic `FArchiveTarget` remains a Core string value.

Developer `TargetPlatform` depends only on Core and RHI. It is selected for
editor/tools and excluded from DurinGame. Engine consumes it through an optional
private dependency; runtime payload readers retain their own format validation.
RHI owns Shader target/backend/code-format values. Policy resolution does not
load a compiler, inspect the active GPU, acquire DDC sessions, or create tasks.

## Immutable resolution

`ResolveTargetPlatform` validates the platform and returns an immutable
`FTargetPlatformDescription`. Current Win64 and MacOS Shader selections are
Vulkan/SPIR-V and Metal/MSL respectively. Explicit material Shader identities
are validated against that selection by the material compiler. Resolution has
no settings wrapper while each platform has only one supported Shader target.
Requests retain resolved values, not a mutable configuration registry. Explicit material target environments use family limits rather than
the active RHI device's feature/resource limits.

| Operation | Supported targets/profiles |
| --- | --- |
| Texture metadata/key validation | Win64 or MacOS, Game or EditorValidation |
| Texture production | Win64 or MacOS, Game |
| Offline Cook | Win64 or MacOS, Game |
| Collision payload representation | Win64; MacOS remains unsupported |

Known targets, supported representations and local producer availability are
different questions. Providers own compiler/SDK execution failures. Resolution
itself does not require a producer to be executable and does not disable valid
cache reuse. Material owns quality/FeatureLevel/pass legality; texture owns
usage-dependent pixel format selection; physics owns geometry and payload layout.
No platform module depends on these asset types or their builders.

Profile admission belongs to Cook, Shader and texture boundaries. The platform
description exposes `SupportsCollisionPayload()` and its Shader target; it has no
generic operation switch or profile state.

## Cook settings and preparation

`FCookCoordinator` copies its admitted request before callbacks and canonicalizes
the owned material configurations. Empty selection means the six supported
Low/High by ES3_1/SM5/SM6 combinations; unsupported or duplicate configurations
are rejected. The selection is passed through dependency declarations and
package serialization options. Material declares its canonical configuration
projection; texture and StaticMesh declarations retain their own recipe identity.
Discovery evaluates reflected/native class schemas and persisted object dependency
projections for every stored class, including secondary exports and nested objects.
No CDO or independent Cook registration is required. Identical declarations from
different class/object groups coalesce; conflicting values and duplicate identities
within one group fail admission. Missing or non-reevaluable instance projections
require loading/rebuilding. The common publisher is independent of export order;
the generic coordinator does not compose builder versions.

Engine's package capture finishes every eligible texture compilation and prepares
material programs and StaticMesh render/
collision payloads before generic Archive discovery/emission. A bounded scoped
table owns detached BulkData by object handle, package revision and exact Archive
target. Nested captures use independent tables. Preparation observes cancellation,
limits traversal to 65536 objects and retained prepared payloads to 1 GiB, and
rejects package revision changes or newly introduced unprepared objects. Preparation
and capture share transient-subtree, explicit omission and object eligibility
policy. Policy-stripped hard references become null; packages without runtime
exports are omitted normally. Class schemas and CookState v4 projections bind
reuse to the same target and source inputs. Complete field planning
uses capture's selected object list instead of rediscovering descendants.
These are retained-storage limits, not total RSS or transient compiler/recipe
allocation limits. Existing family construction admission and result limits
remain authoritative.

Material and StaticMesh `SerializeCooked` only consume current prepared values;
they initiate no compiler, geometry builder or cache request. Raw saving Archive
calls without preparation fail before writing fields. Existing Engine package-save
entrypoints install preparation automatically, including direct callers outside
the Cook coordinator. Runtime loading needs no preparation table or Developer
service. Cooked-only resaving requires data for the same target and never rebuilds
authored inputs. Its material preparation validates the requested configuration
set, decodes each required existing variant and re-encodes only that selection.
Missing variants fail, including when the default all-variant selection cannot
be satisfied. It does not invoke producers. Material/mesh preparation does not install results into live asset fields or
accepted render programs. Texture completion may apply transient platform data. Ordinary offline input stability remains the
[Cook workflow's](Cooking.md) responsibility.

## Cache and format compatibility

Policy is projected by each family into its existing action/dependency schema.
No global policy hash is added to every key. Material translation excludes Shader
compiler/source-closure identity as before; ShaderBuild owns those inputs in the
Shader action. Physics capture passes its explicit payload target into its
unchanged Win64 action. MacOS collision fails before render/geometry production,
and no cross-platform collision DDC sharing is advertised.

The material `material/configurations` Cook declaration causes safe misses for
old material dependency sets, and changing that selection invalidates affected
material packages and any transitive build dependents. Unrelated texture and
generic packages retain reuse. This integration changes no DDC bucket, record
codec, family action version, DMAT/DSLB/collision payload schema, CMNF or CookState
version. Host paths, availability and scheduling are not content identities.
See the [build protocol](DerivedDataBuild.md) and [Cook contract](Cooking.md).
