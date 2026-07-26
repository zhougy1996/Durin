# Legacy source-reference fixtures

These DAST v2 packages freeze the source-provenance layouts that predate named
source libraries. They must remain byte-for-byte legacy inputs: do not regenerate
them after `FSourceLocation` serialization is introduced.

| Fixture | Package owner used for migration | Legacy layout |
| --- | --- | --- |
| `LegacyProjectStaticMesh.dasset` | Project | `FStaticMeshSourceImportData::SourcePath` |
| `LegacyEngineStaticMesh.dasset` | Engine | `FStaticMeshSourceImportData::SourcePath` |
| `LegacyProjectTexture2D.dasset` | Project | `FTexture2DSourceImportData::Source.SourcePath` |
| `LegacyProjectTextureCubeSixFaces.dasset` | Project | Six `FTextureSourceFile::SourcePath` values |
| `LegacyProjectTextureCubePanorama.dasset` | Project | `FTextureCubeSourceImportData::Panorama.SourcePath` |

The StaticMesh and Texture2D packages are copies of repository-owned packages
captured on 2026-07-27. The two TextureCube packages were produced on the same
date by the pre-source-library `EngineTests` import paths from their checked-in
six-face and panorama inputs.

`Engine/SourceAssets` and `Project/SourceAssets` contain byte-identical source
snapshots at the legacy paths recorded by the packages. Migration tests use
these copies to verify persisted hashes without depending on mutable repository
content outside the fixture directory.
