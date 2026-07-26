# Equirectangular Panorama Fixtures

These 8x4 fixtures use top-left row order. Column pairs make the equatorial
principal axes exact with the documented wrapped, pixel-center bilinear
sampler:

- columns 7 and 0: longitude seam and `-X`
- columns 1 and 2: `-Y`
- columns 3 and 4: `+X`
- columns 5 and 6: `+Y`

At the equator, 45-degree cube edges land halfway between adjacent pairs.
The first and last rows are constant so pole clamping is unambiguous.

## `AnalyticalLDR.tga`

Pixels are RGBA8:

| Row | Columns 0 through 7 |
| ---: | --- |
| 0 | `(255,0,255,255)` repeated (north pole) |
| 1 | red, blue, blue, green, green, yellow, yellow, red |
| 2 | red, blue, blue, green, green, yellow, yellow, red |
| 3 | `(0,255,255,255)` repeated (south pole) |

Here red is `(255,0,0,255)`, blue is `(0,0,255,255)`, green is
`(0,255,0,255)`, and yellow is `(255,255,0,255)`. The file is an uncompressed
32-bit TGA with its top-left-origin descriptor set; import must not flip it.

## `AnalyticalHDR.hdr`

Pixels are linear RGB:

| Row | Columns 0 through 7 |
| ---: | --- |
| 0 | `(4,0.5,0.125)` repeated |
| 1 | A, B, B, C, C, D, D, A |
| 2 | A, B, B, C, C, D, D, A |
| 3 | `(8,4,1)` repeated |

The band values are A=`(0.125,0.25,0.5)`, B=`(0.5,1,2)`,
C=`(1,2,4)`, and D=`(2,4,8)`. Every value is exactly representable by RGBE.
The file uses Radiance `32-bit_rle_rgbe` scanlines and the `-Y 4 +X 8`
orientation.
