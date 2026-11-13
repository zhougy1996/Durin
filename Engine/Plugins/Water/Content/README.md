# Water plugin content

`Materials/DefaultWater.dasset` is the authored default material graph, mounted
as `/Plugins/Water/Materials/DefaultWater.DefaultWater`. It contains no compiled
platform programs. Editor uses the normal material compiler; Game loads the
ordinary cooked material package from its Cook generation.

Saved water components serialize the default soft reference explicitly so Cook
can discover it even when the value matches the class default. Projects that
spawn water only at runtime must retain this material using an explicit Cook
root or the existing asset policy.

The [validation fixture](../../../Tests/Fixtures/WaterSurface/README.md) includes
an explicit source authoring command and Cook verification instructions.
