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
- `resources <absolute-output-content-directory>`: Editor-only generation of
  the Low/High and ES3_1/SM5/SM6 Game material family through Engine's ordinary
  material compiler. For the plugin default, use `Engine/Plugins/Water/Content`.
  This explicit authoring command writes the resource family; surface
  registration never writes plugin resources. The isolated compiler process
  also needs `slang-compiler.dll` and `slang-glslang.dll` beside it; copy the same
  companions deployed in the Editor native-test Bin directory.

With the fixture selected, run `DevTool test WaterSurfaceRenderingVulkanTests`
in Editor, then the same target with `--preset Win64-Debug-DurinGame`. Editor
prepares the proof's material programs and global Shader library under ignored
`Build/WaterSurfaceValidation/Proof`; Game only consumes those resources.
`DURIN_WATER_PROOF_DIR` can select another absolute artifact directory. The
target covers the independent one-wave path and the actual plugin component,
including Lit output, animation replay, settings changes, unregister/register,
and Editor hit proxies. Without the fixture, the target retains the independent
proof and does not link or build Water.
