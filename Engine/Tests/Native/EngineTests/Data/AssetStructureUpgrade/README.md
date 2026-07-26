# Asset Structure Upgrade Fixtures

`LegacyStaticMeshLevel.dasset` is a frozen copy of the historical
`/Game/Levels/NewLevel` package. It contains the removed
`DStaticMeshComponent::Material` and `Materials` fields with empty values.

`UnknownNewerLevel.dasset` is derived from that same package by renaming the
serialized `DLevel::Actors` field to the equal-length unknown name `Future`.
It represents content authored by an unrecognized newer schema while retaining
the rest of the real level object graph.

The mesh package and source asset are copied alongside the levels because the
fixture level depends on `/Game/Models/Mesh_Teapot`.

Regenerate these checked-in binaries from the repository root with:

```powershell
python Engine/Tests/Native/EngineTests/Data/AssetStructureUpgrade/generate_fixtures.py
```
