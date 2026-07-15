# Build And Run

This is the operational guide for configuring, building, and running Durin locally.

## Prerequisites

- Run `.\Setup.bat` on Windows before the first configure in a worktree. On other hosts, use `python Engine/Scripts/Bootstrap/initialize_agent_config.py` when a local Agent build override is needed.
- Put CMake on `PATH` when possible. Machine-specific command or environment overrides belong only in `.agents/build-config.json`.
- The Agent build driver initializes the Visual Studio environment automatically for the Windows MSVC profile.

## Configure

Common presets:

- `cmake --preset Win64-Debug-DurinEditor`
- `cmake --preset Win64-Debug-DurinEditor-FastConfigure`
- `cmake --preset Win64-Debug-DurinEditor-Tests`
- `cmake --preset Win64-Debug-DurinEditor-Agent`
- `cmake --preset Win64-Debug-DurinGame`
- `cmake --preset Win64-Shipping-DurinGame`

`Win64-Debug-DurinEditor-FastConfigure` disables precompiled headers for a faster IDE reload/code-model configure pass. Use the normal `Win64-Debug-DurinEditor` preset for full editor builds.

Main build trees stay profile-specific under `Build/`, for example:

- `Build/Win64-Debug-DurinEditor`
- `Build/Win64-Debug-DurinEditor-Tests`
- `Build/Win64-Debug-DurinEditor-Agent`

Use the `*-Tests` preset when native test targets are needed. Normal editor/game presets keep `BUILD_TESTING=OFF`.

Agents must use `Win64-Debug-DurinEditor-Agent` for editor builds and automated validation. It has the same build options as the human-oriented `Win64-Debug-DurinEditor-Tests` preset, plus `DURIN_BUILD_IDENTIFIER=Agent`. Its build tree is `Build/Win64-Debug-DurinEditor-Agent`, and its outputs stay under `Engine/Binaries/Win64/Debug-Agent/` instead of overwriting human build outputs under `Engine/Binaries/Win64/Debug/`.

Agents invoke this workflow through the cross-platform Python driver. Registered profiles in `Engine/Scripts/Build/AgentBuildProfiles.json` bind a host, required commands, and toolchain environment provider to an isolated CMake preset and test output. The driver refuses unregistered presets. Environment providers may locate toolchain-bundled commands such as Visual Studio's Ninja without adding machine paths to the tracked profile. Windows users may use the PowerShell compatibility wrapper:

```powershell
python Engine/Scripts/Build/agent_build.py Configure
python Engine/Scripts/Build/agent_build.py Build --target LevelEditor
python Engine/Scripts/Build/agent_build.py Test --target CoreTests --filter FJsonDocumentTests.*
& "Engine/Scripts/Build/AgentBuild.ps1" Build -Target LevelEditor
```

Profile selection precedence is `--profile`, `DURIN_AGENT_BUILD_PROFILE`, `defaultBuildProfile` in the local config, then the current host's unique default profile. CMake selection precedence is `--cmake`, `DURIN_CMAKE_COMMAND` (or legacy `DURIN_CMAKE_PATH`), `cmakeCommand` in the local config, then `cmake` on `PATH`.

### Build Parallelism

Agent builds select jobs in this order: `--jobs`, `DURIN_AGENT_JOBS`, `jobs` in `.agents/build-config.json`, then automatic detection. Automatic detection reserves two host CPU threads and clamps the result to 1–256 jobs. Normally, omit `--jobs`/`-Jobs` and leave the machine-local `jobs` value at `0`; use a positive local value only when that machine needs a persistent limit.

For direct CMake builds, pass `--parallel` without a count so the native build tool chooses its default parallelism. To impose a local limit, use `--parallel <count>`, or omit `--parallel` and set `CMAKE_BUILD_PARALLEL_LEVEL`; shared documentation should not prescribe a machine-specific count.

The Agent config is optional. Empty command/profile strings and `jobs: 0` keep automatic discovery enabled. `environmentSetup.script` is only needed when the registered profile cannot initialize its toolchain environment automatically.

## Build

Prefer building only the target needed for the current change instead of `--target all`.

```powershell
cmake --build Build/Win64-Debug-DurinEditor --target DurinLauncher --parallel
cmake --build Build/Win64-Debug-DurinEditor --target RenderCore --parallel
cmake --build Build/Win64-Debug-DurinEditor-Tests --target CoreTests --parallel
python Engine/Scripts/Build/agent_build.py Build --target CoreTests
```

### Runtime Smoke Tests

A runtime smoke test must only be performed after a successful full build of the same Agent Build Profile. Partial and single-target builds are useful for iteration, but they do not establish that the executable and all runtime-loaded modules are mutually up to date.

Use the registered Agent build driver and build the `all` target before launching the editor:

```powershell
python Engine/Scripts/Build/agent_build.py Build --target all
& "Engine/Binaries/Win64/Debug-Agent/Runtime/DurinEditor/DurinEditor.exe"
```

Agents are authorized to execute the editor executable produced under the selected Agent Build Profile's isolated output directory for smoke validation. Do not launch a human-owned executable under `Engine/Binaries/Win64/Debug/` or another non-Agent output as part of automated validation.

## Run And Output Layout

Run the editor from:

- `Engine/Binaries/Win64/Debug/Runtime/DurinEditor/DurinEditor.exe`

Key output locations:

- Runtime launcher and module DLLs: `Engine/Binaries/<Platform>/<Config>/Runtime/<Profile>/`
- Shared runtime third-party DLLs: `Engine/Binaries/<Platform>/<Config>/ThirdParty/`
- Native test executables: `Engine/Binaries/<Platform>/<Config>/Tests/<Profile>/Bin/`

When `DURIN_BUILD_IDENTIFIER` is set, `<Config>` becomes `<Config>-<Identifier>`. For example, the Agent test preset writes runtime binaries to `Engine/Binaries/Win64/Debug-Agent/Runtime/DurinEditor/`.

The launcher target is `DurinLauncher`, but the executable name matches the active profile such as `DurinEditor.exe` or `DurinGame.exe`.

Runtime path discovery assumes the executable remains inside the repository-relative output layout above.

## Runtime DLL Troubleshooting

- Shared third-party runtime DLLs are deployed to `Engine/Binaries/<Platform>/<Config>/ThirdParty/`.
- `RenderCore` delay-loads `slang.dll`; if startup fails, check both `ThirdParty/` and the active runtime directory first.
- Moving only the built runtime tree away from the repository root is not supported by the current path logic.

## Related Docs

- `Documentation/Setup/ThirdPartyBootstrap.md`
- `Documentation/Setup/NativeTests.md`
- `Documentation/Architecture/Profiles.md`
- `Documentation/Architecture/RuntimeArchitecture.md`
