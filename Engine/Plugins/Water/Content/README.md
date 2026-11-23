# Water plugin content

`Materials/DefaultWater.dasset` is the authored default material graph, mounted
as `/Plugins/Water/Materials/DefaultWater.DefaultWater`. It contains no compiled
platform programs. Editor uses the normal material compiler; Game loads the
ordinary cooked material package from its Cook generation.

The graph uses the Single Layer Water shading model and calls the plugin's
`Materials/GerstnerWaves.dasset` function for world displacement and tangent-space normals. See the [water material settings](../../../../Documentation/Runtime/World/WaterSurfaces.md#material-resources).

The shipped wave preset is **Waves/DefaultWaterWaves.dasset**. New water surfaces reference it by default. Clear **Waves Source** for a flat surface.

Saved water components serialize the default soft reference explicitly so Cook
can discover it even when the value matches the class default. Projects that
spawn water only at runtime must retain this material using an explicit Cook
root or the existing asset policy.

The [validation fixture](../../../Tests/Fixtures/WaterSurface/README.md) includes
an explicit source authoring command and Cook verification instructions.
