# Water surface validation fixture

## Setup

Copy this directory to `Build/WaterSurfaceValidation/Project`, create its `Content` directory and temporarily add its `WaterSurfaceFixture.dproject` path to `Durin.dworkspace`. The fixture needs a project root outside Engine. Restore the original workspace descriptor after validation.

With the host DevTool launcher, build `WaterSurfaceFixturePluginBuild` and the required probe for each tested preset (`Win64-Debug-DurinEditor` or `Win64-Debug-DurinGame`). Copy the probe executable beside that inventory's DLLs in `Build/<preset>/PluginBuild/WaterSurfaceFixture/Debug/Runtime`. Add the matching Engine Runtime and ThirdParty directories to the process search path.

## Select a check

Run `WaterSurfaceFeatureProbe <absolute-fixture.dproject> <mode> [argument]`.

| Change | Check |
| --- | --- |
| Generator, grid or CPU query | `queries` in Editor and Game. Checks inversion, bounds, normals and Float32 agreement. |
| Gerstner expression or default material | `material` in Editor. Compares the saved material graph with CPU samples. |
| Component or wave asset persistence | `persist` in Editor, then `load` in a fresh Editor process. Covers flat, default and shared waves. |
| Cook dependencies or rendering | Follow the Cook and Vulkan sequence below. |

For startup changes, run `WaterSurfaceStartupProbe <absolute-fixture.dproject> enabled`. Set `Water` to `false` in the disposable project's `Configs/Plugins.json`, run `disabled` in a fresh process, then restore the config.

## Cook and Vulkan

1. Run `persist` and build `WaterSurfaceFixtureGameReceipt` with the Game preset.
2. From the checkout root, cook the saved Level:

   `.\DevTool.bat cook --project Build/WaterSurfaceValidation/Project/WaterSurfaceFixture.dproject --target-receipt Build/Win64-Debug-DurinGame/TargetReceipts/WaterSurfaceFixture/Debug/TargetReceipt.json --root /Game/WaterSurfaceLevel --output-dir Build/WaterSurfaceValidation/Cooked`
3. Run the Game feature probe with `load-cooked <absolute-cook-directory>`. This checks the preset and compiled material in a fresh process.
4. Run `.\DevTool.bat test WaterSurfaceRenderingVulkanTests` in Editor, then with `--preset Win64-Debug-DurinGame`. Editor prepares the independent rendering proof. Game also reads the component Cook from step 2. For custom locations, set `DURIN_WATER_PROOF_DIR` and `DURIN_WATER_COOK_DIR` respectively.

For a source-free workspace delivery, place the Game `WorkspaceContentProbe` beside its Runtime binaries and pass the absolute project descriptor, Cook directory and logical object paths. Use export `--verify` for Game startup and rendering.

## Regenerate source assets

Use the Editor feature probe. Pass an absolute destination filename to `author-wave-preset` (DefaultWaterWaves), `author-waves` (GerstnerWaves) or `author-material` (DefaultWater). The wave function author reads its existing public ports; the material author loads that function.

`author-demo-level` writes and reopens `/Game/Levels/SingleLayerWaterDemo` in the fixture. Place the Editor's AssetForgeBuiltins, MeshBuilder and TextureCompressor DLLs and dependencies beside the probe, then copy the verified Level to `Sandbox/Content/Levels`.
