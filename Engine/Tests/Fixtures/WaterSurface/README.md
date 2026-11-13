# Water surface validation fixture

Copy this directory to `Build/WaterSurfaceValidation/Project` and temporarily add
`Build/WaterSurfaceValidation/Project/WaterSurfaceFixture.dproject` to
`Durin.dworkspace`. Create its `Content` directory. The fixture must be outside
the Engine project directory because workspace project roots cannot overlap.
Restore the exact original workspace descriptor after validation.

Use the host DevTool launcher to build `WaterSurfaceFixturePluginBuild`,
`WaterSurfaceStartupProbe` and `WaterSurfaceFeatureProbe` in both
`Win64-Debug-DurinEditor` and `Win64-Debug-DurinGame`. The ordinary workspace
projects remain unmodified and do not opt into Water.

Copy each probe beside the corresponding inventory's Runtime binaries under
`Build/<preset>/PluginBuild/WaterSurfaceFixture/Debug/Runtime`. The startup probe
also needs that variant's Projects library beside it. Run with the absolute
fixture project filename followed by `enabled`. Then save `Water:false` in the
disposable project and run `disabled` in a fresh process without rebuilding.
Restore its original descriptor before further positive runs. The probe rejects
early loading before Providers and checks that the reflected water actor is
present only for the enabled process.

The feature probe supports:

- `queries`: inverse height/normal queries, outside/invalid inputs, finite
  differences, Float32 material agreement through one hour, finite grid error,
  parameter updates, and world transforms. Available in both variants.
- `persist`: Editor-only creation, save, unload and reopen of a Level containing
  a water actor with nondefault dimensions/elevation/waves.
- `load`: load that saved Level and verify water composition, parameters and
  queries in either runtime variant.
- `material`: Editor-only comparison of the saved source material graph against
  the CPU waves, queries, normals and bounds.
- `author-material OUTPUT`: Editor-only regeneration of the source material.
- `load-cooked COOK_DIR`: load the saved Level and accepted Water platform
  program from an ordinary Cook generation without authored registry scanning.

The plugin default is an ordinary source `DMaterial` asset. To regenerate the
committed authored graph, run the Editor probe with `author-material` and the
absolute `Engine/Plugins/Water/Content/Materials/DefaultWater.dasset` filename.
This explicit authoring operation writes source only; neither builds nor Cook
rewrite plugin content.

Run `persist` first, then use the normal host DevTool Cook command with the
fixture `--project`, `--root /Game/WaterSurfaceLevel`, and
`--output-dir Build/WaterSurfaceValidation/Cooked`. Build `DurinAssetTool` if
needed. The Cook must retain the Water material through the component's saved
soft reference, publish its cooked package and Shader library, and leave source
content unchanged. Repeat Cook to check reuse. Runtime-only water creation
requires an explicit material Cook root or equivalent existing asset policy.

With the fixture selected, run `DevTool test WaterSurfaceRenderingVulkanTests`
in Editor, then the same target with `--preset Win64-Debug-DurinGame`. Editor
prepares the proof's material programs and global Shader library under ignored
`Build/WaterSurfaceValidation/Proof`; the component Game scene consumes the normal Cook generation.
`DURIN_WATER_PROOF_DIR` selects another proof directory;
`DURIN_WATER_COOK_DIR` selects another component Cook directory. The
target covers the independent one-wave path and the actual plugin component,
including Lit output, animation replay, settings changes, unregister/register,
and Editor hit proxies. Without the fixture, the target retains the independent
proof and does not link or build Water.
