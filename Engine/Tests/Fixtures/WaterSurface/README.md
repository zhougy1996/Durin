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
`Build/<preset>/PluginBuild/WaterSurfaceFixture/Debug/Runtime`. Make the
matching Engine Runtime and ThirdParty binaries available in the process
search path. Run with the absolute
fixture project filename followed by `enabled`. Then save `Water:false` in the
disposable project's `Configs/Plugins.json` and run `disabled` in a fresh process without rebuilding.
Restore its original startup configuration before further positive runs. The probe rejects
early loading before Providers and checks that the reflected water actor is
present only for the enabled process.

The feature probe supports:

- `queries`: inverse height/normal queries, outside/invalid inputs, finite
  differences, Float32 material agreement through one hour, finite grid error,
  parameter updates, world transforms, and seeded wave asset generation.
  Available in both variants.
- `persist`: Editor-only creation, save, unload and reopen of a Level containing
  manual water and two actors referencing a separate shared Water Waves asset.
- `load`: load that saved Level and verify water composition, parameters, shared
  asset identity and queries in either runtime variant.
- `material`: Editor-only comparison of the saved source material graph against
  the CPU waves, queries, normals and bounds.
- `author-waves OUTPUT`: regenerate the Gerstner function with its existing public ports.
- `author-material OUTPUT`: Editor-only regeneration of the source material.
- `author-demo-level`: Editor-only creation, save and reopen of
  `/Game/Levels/SingleLayerWaterDemo`. Run in the disposable fixture project
  to regenerate the Sandbox demo. Make the Editor's AssetForgeBuiltins,
  MeshBuilder and TextureCompressor DLLs and their dependencies available
  beside the probe.
- `load-cooked COOK_DIR`: load the saved Level and accepted Water platform
  program from an ordinary Cook generation without authored registry scanning.

The plugin default is an ordinary source `DMaterial` asset. To regenerate the
committed authored graph, run the Editor probe with `author-material` and the
absolute `Engine/Plugins/Water/Content/Materials/DefaultWater.dasset` filename.
The command references the authored GerstnerWaves function in the Water plugin.
This explicit authoring operation writes source only; neither builds nor Cook
rewrite plugin content.

Run `persist` first and build `WaterSurfaceFixtureGameReceipt` in Game. Use
the normal host DevTool Cook command with the fixture `--project`,
`--target-receipt Build/Win64-Debug-DurinGame/TargetReceipts/WaterSurfaceFixture/Debug/TargetReceipt.json`,
`--root /Game/WaterSurfaceLevel`, and
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
including Lit output, animation replay, settings changes, shared wave asset
edits and undo, unregister/register, and Editor hit proxies. Without the fixture,
the target retains the independent proof and does not link or build Water.

For workspace Cook closure, build `WorkspaceContentProbe` in Game and copy it
beside the corresponding source-free delivery Runtime binaries. It deliberately
has no Water link, so ordinary project CDO initialization needs no plugin mount.
Run with the absolute project descriptor, Cook directory and one or more complete
logical object paths. It opens only the cooked catalog, loads every requested
object, requires an accepted platform program for each material, and rejects a
loaded ShaderBuild service. Game compiles authored graph fields out. Use the
ordinary export `--verify` additionally for bounded real Game startup/rendering;
this probe only qualifies loading. Add non-default examples as explicit Cook
roots rather than inferring reachability from successful default-Level startup.
