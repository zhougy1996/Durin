# Asset Structure Upgrade Fixtures

`LegacyStaticMeshLevel.dasset` is a frozen copy of the historical
`/Game/Levels/NewLevel` package. It contains the removed
`DStaticMeshComponent::Material` and `Materials` fields with empty values.

`UnknownNewerLevel.dasset` is derived from that frozen package by renaming the
serialized `DLevel::Actors` field to the equal-length unknown name `Future`.
It represents content authored by an unrecognized newer schema while retaining
the rest of the real level object graph.

The mesh package and source asset are copied alongside the levels because the
fixture level depends on `/Game/Models/Mesh_Teapot`.

Regenerate the derived unknown-schema fixture and refresh its dependencies from
the repository root with:

```powershell
python Engine/Tests/Native/EngineTests/Data/AssetStructureUpgrade/generate_fixtures.py
```

`LegacyStaticMeshLevel.dasset` is intentionally not regenerated from the live
Sandbox level because that asset now uses the current component schema.
