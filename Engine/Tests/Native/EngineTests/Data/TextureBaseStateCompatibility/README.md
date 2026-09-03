# Texture Base-State Compatibility Fixtures

These DAST v9 packages were captured before `FTextureSource`,
`AssetImportData`, and cooked bulk storage moved from `DTexture2D`,
`DTextureCube`, and `DVolumeTexture` into `DTexture`.

`Authored/` contains one package for each family. The 2D and Cube source
payloads are inline; the 65x65x65 R8 Volume source is stored in the checked-in
`.dbulk` companion. Every asset has owned import metadata with an asset-relative
synthetic source hint and nondefault family build settings.

`CookedPackages/Game/` contains the corresponding source-free Win64 Game packages.
Their native `PlatformData` identities remain declared by the concrete family.
The Volume TXPL payload is external while the smaller 2D and Cube payloads are
inline.

Tests always copy these read-only inputs into their process sandbox before
mounting or loading them. Do not regenerate them from the post-move schema.
