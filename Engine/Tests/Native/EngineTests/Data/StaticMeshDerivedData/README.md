# Static Mesh Derived-Data Fixtures

These fixtures are the canonical logical inputs for DMSH schema version 1.
Stage 2 encodes them into payload bytes and compares deterministic output.
Coordinates, stream elements, slot ids, and ordering are exact.

The schema-version-one Win64 encodings are frozen as:

| Fixture | Stored bytes | XXH3-128 of complete object |
| --- | ---: | --- |
| `SingleSection` | 572 | `ad51dc11ff51b69375608a9877a64f3a` |
| `MultiMaterialStreams` | 856 | `6c1e194288afa6d31ef945773b2c26b4` |

## `SingleSection`

- Bounds: minimum `(0, 0, 0)`, maximum `(1, 1, 0)`.
- Material slots: `{11111111-1111-1111-1111-111111111111, "Default", 0}`.
- LOD 0: three vertices, indices `[0, 1, 2]`, one UV channel, no colors.
- Positions: `(0,0,0)`, `(1,0,0)`, `(0,1,0)`.
- Normals: three copies of `(0,0,1)`.
- Tangents: three copies of `(1,0,0,1)`.
- UV0: `(0,0)`, `(1,0)`, `(0,1)`.
- Section: first index `0`, index count `3`, vertices `0..2`, material slot `0`.

## `MultiMaterialStreams`

- Bounds: minimum `(-1, -1, 0)`, maximum `(1, 1, 0)`.
- Material slots, in order:
  `{22222222-2222-2222-2222-222222222222, "Left", 7}` and
  `{33333333-3333-3333-3333-333333333333, "Right", 3}`.
- LOD 0: four vertices, indices `[0,1,2, 2,1,3]`, four UV channels, colors.
- Positions: `(-1,-1,0)`, `(0,-1,0)`, `(-1,1,0)`, `(1,1,0)`.
- Normals: four copies of `(0,0,1)`.
- Tangents: four copies of `(1,0,0,-1)`.
- UV0: `(0,0)`, `(0.5,0)`, `(0,1)`, `(1,1)`.
- UV1: `(0.1,0.2)`, `(0.3,0.4)`, `(0.5,0.6)`, `(0.7,0.8)`.
- UV2: `(1,1)`, `(1,0)`, `(0,1)`, `(0,0)`.
- UV3: `(0.25,0.25)`, `(0.5,0.25)`, `(0.25,0.5)`, `(0.5,0.5)`.
- Colors: `(1,0,0,1)`, `(0,1,0,1)`, `(0,0,1,1)`, `(1,1,1,0.5)`.
- Sections: indices `0..2`, vertices `0..2`, slot `0`; indices `3..5`,
  vertices `1..3`, slot `1`.

## Malformed payload derivations

Stage 2 derives malformed bytes from the deterministic `SingleSection` encoding:

- every truncation length from zero through the final byte;
- one flipped byte in the checksummed chunk region;
- a chunk range whose `offset + stored size` overflows `uint64`;
- two required chunk ranges that overlap;
- vertex and index counts one greater than their contract limits;
- a stored/uncompressed ratio greater than `64:1`;
- index `3` for the three-vertex fixture;
- one unknown required chunk and one unknown optional chunk;
- schema version `2`, target platform `Unknown`, and invalid enum values.
