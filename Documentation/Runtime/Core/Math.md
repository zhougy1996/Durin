# Core Math

Summary: Define Durin math types, operation semantics, and the GLM integration boundary.

Modules: Core

Last reviewed: 2026-08-20

Durin owns math names and operation semantics in the Core module while GLM
continues to provide the underlying value storage and inline implementation.
Repository code uses Durin aliases and `Durin::Math` so a future backend or
value-type change has one explicit boundary.

## Header Ownership

| Header | Use |
| --- | --- |
| `Math/MathFwd.h` | Forward declarations and Durin aliases for declarations that do not require complete types |
| `Math/Vector.h` | Complete vector, quaternion, and matrix aliases plus vector/quaternion constants |
| `Math/Operations.h` | Vector, quaternion, angle, and matrix algorithms in `Durin::Math` |
| `Math/Transform.h` | `FTransform` storage, composition, and matrix conversion |
| `Math/TransformDecomposition.h` | Checked matrix-to-transform decomposition |
| `Math/DurinMath.h` | Core math umbrella when a consumer needs several math facilities |

Include the narrowest header that owns the required API. Public declarations
should spell Durin types, including `FMatrix4f` for float 4x4 matrices, rather
than naming GLM types directly.

## Value Types and ABI

`FReal` is `double`. `FVector2`, `FVector3`, `FVector4`, `FQuat`, and `FMatrix`
use double-precision GLM storage. `FQuatd` is an explicit spelling of the same
double quaternion while `FQuatf` is the distinct float quaternion. The `f`,
`d`, and `i` vector aliases preserve their named scalar precision, and
`FMatrix4f` aliases the existing float 4x4 GLM matrix used by import and
shader-layout boundaries.

These are aliases, not engine-owned storage types. Existing constructors,
operators, component members, column indexing, size, alignment, reflection
identity, and serialized representation therefore retain their current GLM
behavior. Replacing that storage or ABI requires a separate plan; the facade
does not imply that replacement has occurred.

At the reflection boundary, explicit `FVector2d`, `FVector3d`, and `FVector4d`
source spellings resolve to the established `FVector2`, `FVector3`, and
`FVector4` descriptors. `FQuatd` similarly resolves to `FQuat`. `FQuatf` owns
a separate `(w, x, y, z)` float-component schema. `FMatrix4f` owns a
column-major schema named `Column0` through `Column3`, each an `FVector4f`.
Their reflected defaults are the identity quaternion and identity matrix, and
serialization walks those fields rather than copying ABI padding or raw bytes.

## Operation Surface

`Durin::Math` owns these caller-facing operation families:

- Finite and vector algebra: `IsFinite`, `Dot`, `LengthSquared`, `Length`,
  `Cross`, `Normalize`, `TryNormalize`, and `NormalizeOr`.
- Component-wise vector operations: `Abs`, `Min`, `Max`, `Clamp`, and the
  unclamped `Lerp`.
- Angles and constants: `Pi`, `HalfPi`, `TwoPi`, `DegreesToRadians`, and
  `RadiansToDegrees`, preserving float or double precision.
- Quaternion operations: explicit degree/radian axis-angle and Euler
  constructors, Euler conversions, matrix conversion, inverse, vector
  rotation, and sign-independent rotation equivalence.
- Matrix operations: double-precision determinant, unchecked and checked
  inverse, transpose, transposed float conversion, and double-precision
  translation, rotation, and scale matrix construction.

Ordinary arithmetic operators and `.x/.y/.z/.w` access remain valid through
the aliases. Scalar operations without Durin-specific semantics remain under
the standard library rather than receiving cosmetic wrappers.

## Failure and Exceptional Values

`Normalize` is an unchecked algebraic operation. Its vector or quaternion input
must be finite and have non-zero length. Quaternion `Inverse` requires a finite,
non-zero quaternion; matrix `Inverse` requires a finite, nonsingular matrix.
The facade does not sanitize a violated precondition, and backend NaN or
infinity results may propagate.

Use the checked forms for authored or otherwise untrusted values:

- `TryNormalize` rejects a non-finite input or threshold, a negative threshold,
  squared length less than or equal to the threshold, and a non-finite result.
  Its output is unchanged on failure. The default squared-length threshold is
  `kSmallNumber` for float values and `kDoubleSmallNumber` for double values.
- `NormalizeOr` uses the same checks and returns the caller-supplied fallback
  verbatim on failure.
- `TryInverse` rejects a non-finite matrix or threshold, a negative threshold,
  a non-finite determinant, absolute determinant less than or equal to the
  threshold, and a non-finite inverse. Its output is unchanged on failure.
- `IsFinite` is true only when every component is finite. Signed zero is finite
  but still fails normalization because its squared length is zero.

Component-wise functions do not replace NaN or infinity. Callers that require
portable exceptional-value behavior validate inputs first.

## Angles, Quaternions, and Transforms

Angle-bearing constructors and conversions state `Degrees` or `Radians` in
their names. Axis-angle constructors require a finite, normalized axis.
Positive rotation retains the existing right-handed convention.

Quaternion composition remains `Parent * Relative`, and a vector is rotated as
`Quaternion * Vector`. `AreRotationsEquivalent` normalizes both inputs and uses
the absolute dot product, so `q` and `-q` represent the same rotation. Invalid
or non-normalizable inputs are not equivalent. `FQuatConstants::Identity` is
the explicit `(w, x, y, z) = (1, 0, 0, 0)` identity.

Durin uses +X forward, +Y right, and +Z up. Matrices use column-vector
multiplication and `[column][row]` indexing. `FTransform::ToMatrix` composes
translation, rotation, and scale as `T * R * S`. `TransposeToFloat` combines
double-to-float narrowing with transposition; renderer callers use it explicitly
at the CPU-to-shader boundary.

## Direct GLM Boundary

Production callers use Durin aliases and `Durin::Math` for covered operations.
Direct GLM use is restricted to:

- alias declarations and Core math backend implementation;
- bounded third-party or shader-layout interoperability not represented by the
  current facade; and
- native tests that intentionally provide an independent reference result.

Every exception is recorded with exact include and symbol counts, an owner, and
a rationale in
`Tools/Architecture/direct_glm_allowlist.json`. The companion
`Tools/Architecture/check_direct_glm.py` check rejects a new file, symbol,
include, changed count, or stale entry. Completed migration must not leave a
`migration-debt` or `deferred-plan` entry.

Build and test execution follows [Build and Run](../../Development/Build/BuildAndRun.md)
and [Native Tests](../../Development/Build/NativeTests.md).
