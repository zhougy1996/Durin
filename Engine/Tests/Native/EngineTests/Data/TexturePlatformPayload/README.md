# Texture Platform-Payload Logical Fixtures

These fixtures freeze TXPL schema version 1 and texture key schema version 1.
Tests build native payload bytes directly from the values below. They never
invoke a source decoder, panorama projector, BC compressor, RHI, or window.

Every byte sequence shown for a BC subresource is already-compressed opaque
test data. It is not intended to describe a meaningful rendered block.

## `Texture2D`

- Dimension: `Texture2D(1)`.
- Builder/schema/platform/profile: `2/1/Win64(1)/Game(1)`.
- Format: `BC1_UNORM_SRGB(2)`.
- Base dimensions: `4x4`; one slice and three mips (`4x4`, `2x2`, `1x1`).
- Every mip row pitch is `8`.
- Mip bytes are `00 11 22 33 44 55 66 77`,
  `10 11 12 13 14 15 16 17`, and `20 21 22 23 24 25 26 27`.
- Source hash: low `0x0123456789abcdef`, high `0xfedcba9876543210`.
- Settings: usage `Color(0)`, sRGB `1`, compression quality `Normal(1)`,
  alpha mode `Average(0)`, maximum resolution `0`, alpha threshold `0.5`
  (`0x3f000000`).

## `SixFaceCube`

- Dimension: `TextureCube(2)`.
- Builder/schema/projection/platform/profile: `1/1/1/Win64(1)/Game(1)`.
- Format: `BC1_UNORM_SRGB(2)`.
- Base dimensions: `4x4`; six slices and three mips per slice.
- Every row pitch is `8`.
- Face base bytes in PositiveX, NegativeX, PositiveY, NegativeY, PositiveZ,
  NegativeZ order are hexadecimal `10`, `20`, `30`, `40`, `50`, and `60`.
  For each slice, mip 0 is eight repetitions of its base byte, mip 1 is eight
  repetitions of base plus hexadecimal `01`, and mip 2 is eight repetitions of
  base plus hexadecimal `02`.
- Exact-source hashes use `(low, high)` pairs `(1, 101)`, `(2, 102)`,
  `(3, 103)`, `(4, 104)`, `(5, 105)`, `(6, 106)` in the same face order.
- Source layout is `SixFaces(0)` and sRGB is `1`.

Changing any one face hash or swapping two hashes must change the key.
Changing only the order in which a caller supplies already-labeled faces must
not change it after the caller normalizes them into the frozen face order.

## `PanoramaDerivedCube`

The TXPL fields and subresource bytes are identical to `SixFaceCube`; this
proves that source layout belongs to the key rather than the readable platform
schema. Its key inputs differ:

- Source layout: `EquirectangularPanorama(1)`.
- Panorama source hash: low `0x0f1e2d3c4b5a6978`, high
  `0x8796a5b4c3d2e1f0`.
- Requested face dimension: `4`.
- Exposure EV: `1.0` (`0x3f800000`).
- sRGB: `1`.
- Builder/schema/projection/platform/profile: `1/1/1/Win64(1)/Game(1)`.

## Malformed derivations

Readers and key tests derive:

- every truncation length from zero through the final TXPL byte;
- bad magic, schema, builder policy, platform, profile, dimension, format,
  header size, record size, and checksum;
- nonzero reserved header, record, gap, or trailing-padding bytes;
- zero or excess slices, mips, records, dimensions, and stored bytes;
- duplicate, missing, unsorted, or out-of-range slice/mip coordinates;
- an `offset + stored size` overflow, overlap, misalignment, and out-of-file
  range;
- wrong row pitch and stored size for the selected BC block layout;
- an incomplete mip chain, wrong dimension progression, and missing `1x1` tail;
- cube faces with unequal dimensions, row pitches, byte counts, or mip counts;
- Texture2D with six slices and TextureCube with one slice;
- base dimension and complete payload one unit above their limits;
- NaN, infinity, negative zero, and out-of-range float key inputs;
- key changes for every semantic setting, builder/schema/projection version,
  target platform/profile, source hash, cube face, face order, and layout.
