# Skybox Convention Test Cube

The six PNG files form one 128 x 128 RGBA8 cubemap using the face and
top-left-origin orientation documented in
`Documentation/Architecture/CubeTextures.md`.

Each face has a unique center color and labels its center face plus the world
direction reached at its top, right, bottom, and left edges. The files are
intentionally simple LDR inputs for asset-import, shader-sampling, and rendered
orientation tests.

| Array layer | File |
| ---: | --- |
| 0 | `PositiveX.png` |
| 1 | `NegativeX.png` |
| 2 | `PositiveY.png` |
| 3 | `NegativeY.png` |
| 4 | `PositiveZ.png` |
| 5 | `NegativeZ.png` |

