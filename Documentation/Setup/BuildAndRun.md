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

Agents must use `Win64-Debug-DurinEditor-Agent` for editor builds and automated validation. It has the same build options as the human-oriented `Win64-Debug-DurinEditor-Tests` preset, plus `DURIN_BUILD_IDENTIFIER=Agent`. Its build tree is `Build/Win64-Debug-DurinEditor-Agent`, binary outputs stay under `Engine/Binaries/Win64/Debug-Agent/`, and DHT metadata stays under `Engine/Intermediate/Build/Win64/Debug-Agent/DurinEditor/` instead of overwriting human build outputs.

Agents invoke this workflow through the cross-platform Python driver. Registered profiles in `Engine/Scripts/Build/AgentBuildProfiles.json` bind a host, required commands, and toolchain environment provider to an isolated CMake preset and test output. The driver refuses unregistered presets. Environment providers may locate toolchain-bundled commands such as Visual Studio's Ninja without adding machine paths to the tracked profile. Windows users may use the PowerShell compatibility wrapper:

```powershell
python Engine/Scripts/Build/agent_build.py Configure
python Engine/Scripts/Build/agent_build.py Build --target LevelEditor
python Engine/Scripts/Build/agent_build.py Clean
python Engine/Scripts/Build/agent_build.py Rebuild --target all
python Engine/Scripts/Build/agent_build.py Test --target CoreTests --filter FJsonDocumentTests.*
& "Engine/Scripts/Build/AgentBuild.ps1" Build -Target LevelEditor
```

Profile selection precedence is `--profile`, `DURIN_AGENT_BUILD_PROFILE`, `defaultBuildProfile` in the local config, then the current host's unique default profile. CMake selection precedence is `--cmake`, `DURIN_CMAKE_COMMAND` (or legacy `DURIN_CMAKE_PATH`), `cmakeCommand` in the local config, then `cmake` on `PATH`.

### Build Parallelism

Agent builds select jobs in this order: `--jobs`, `DURIN_AGENT_JOBS`, `jobs` in `.agents/build-config.json`, then automatic detection. Automatic detection reserves two host CPU threads and clamps the result to 1–256 jobs. Normally, omit `--jobs`/`-Jobs` and leave the machine-local `jobs` value at `0`; use a positive local value only when that machine needs a persistent limit.

For direct CMake builds, pass `--parallel` without a count so the native build tool chooses its default parallelism. To impose a local limit, use `--parallel <count>`, or omit `--parallel` and set `CMAKE_BUILD_PARALLEL_LEVEL`; shared documentation should not prescribe a machine-specific count.

The Agent config is optional. Empty command/profile strings and `jobs: 0` keep automatic discovery enabled. `environmentSetup.script` is only needed when the registered profile cannot initialize its toolchain environment automatically.

## Build

During development iteration, prefer building only the target needed for the current change instead of `--target all`. Before any editor runtime validation, build the `all` target with the same Agent Build Profile so the executable and every runtime-loaded module are mutually up to date.

```powershell
cmake --build Build/Win64-Debug-DurinEditor --target DurinLauncher --parallel
cmake --build Build/Win64-Debug-DurinEditor --target RenderCore --parallel
cmake --build Build/Win64-Debug-DurinEditor-Tests --target CoreTests --parallel
python Engine/Scripts/Build/agent_build.py Build --target CoreTests
```

The Agent driver serializes operations that use the same registered build profile. If another Configure, Build, Clean, Rebuild, or Test operation already owns that profile's build tree, a second invocation fails with information about the active operation. Different registered Agent profiles retain independent locks.

### Interrupted Build Recovery

Long builds must be allowed to continue under one command invocation. Observe progress by waiting on that invocation; do not set a short command timeout merely to regain control and then launch another build.

If a build is interrupted by a timeout, cancellation, terminal closure, or agent termination:

1. Confirm that the old `cmake`, `ninja`, compiler, and linker process tree has exited. Do not start recovery while it may still be writing outputs.
2. Run a Rebuild of the complete profile:

   ```powershell
   python Engine/Scripts/Build/agent_build.py Rebuild --target all
   ```

3. Only launch the editor after that command succeeds.

The driver leaves an interruption marker when its child process does not return normally. While that marker exists, ordinary Build and Test operations are rejected. `Rebuild` cleans the existing Agent build tree when possible, performs a fresh configure, builds the requested target (`all` by default), and clears the marker only after success. `Clean` and `Configure` may still be used for diagnosis, but they do not clear a pre-existing interruption marker.

DHT writes are committed atomically and serialized by platform, output configuration, and profile. Debug, Release, and identifier-specific presets use independent intermediate directories and locks, so the Agent preset no longer shares generated metadata with human Debug or Release presets.

After upgrading an existing Agent worktree to the identifier-aware DHT layout, run `python Engine/Scripts/Build/agent_build.py Rebuild --target all` once. This creates the isolated metadata tree; the build workflow does not delete or migrate files from the human-owned empty-identifier directory.

### Runtime Smoke Tests

A runtime smoke test must only be performed after a successful full build of the same Agent Build Profile. Partial and single-target builds are useful only for development iteration; they do not establish that the executable and all runtime-loaded modules are mutually up to date.

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

DHT metadata uses `Engine/Intermediate/Build/<Platform>/<Config>/<Profile>/` when the identifier is empty. When set, the identifier is appended to the configuration exactly like binary outputs: `Engine/Intermediate/Build/<Platform>/<Config>-<Identifier>/<Profile>/`. The Agent preset therefore uses `Engine/Intermediate/Build/Win64/Debug-Agent/DurinEditor/`.

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
