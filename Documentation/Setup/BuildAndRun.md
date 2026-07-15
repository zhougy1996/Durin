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
- `cmake --preset Win64-Debug-DurinGame`
- `cmake --preset Win64-Shipping-DurinGame`

`Win64-Debug-DurinEditor-FastConfigure` disables precompiled headers for a faster IDE reload/code-model configure pass. Use the normal `Win64-Debug-DurinEditor` preset for full editor builds.

Main build trees stay profile-specific under `Build/`, for example:

- `Build/Win64-Debug-DurinEditor`
- `Build/Win64-Debug-DurinEditor-Tests`

Use the `*-Tests` preset when native test targets are needed. Normal editor/game presets keep `BUILD_TESTING=OFF`.

Agents run in dedicated worktrees and use the regular `Win64-Debug-DurinEditor-Tests` preset for editor builds and automated validation. Worktree-local `Build/`, `Engine/Binaries/`, and `Engine/Intermediate/` directories provide the isolation, so Agent builds use the same paths and artifacts that an IDE opened on that worktree expects.

Agents invoke this workflow through the cross-platform Python driver. Registered profiles in `Engine/Scripts/Build/AgentBuildProfiles.json` bind a host, required commands, and toolchain environment provider to a CMake preset and test output. The driver refuses unregistered presets. Environment providers may locate toolchain-bundled commands such as Visual Studio's Ninja without adding machine paths to the tracked profile. Windows Agents must use the root-level batch wrapper from PowerShell or Command Prompt; it fixes `VSLANG=1033` before Visual Studio environment initialization, forwards all arguments, and returns the driver's exit code:

```powershell
.\BuildTool Configure
.\BuildTool Build --target LevelEditor
.\BuildTool Clean
.\BuildTool Rebuild --target all
.\BuildTool Test --target CoreTests --filter FJsonDocumentTests.*
```

On non-Windows hosts, use `python Engine/Scripts/Build/agent_build.py <arguments>`. Direct Python invocation on Windows bypasses the wrapper-owned MSVC language guarantee and is not a supported Agent entrypoint.

When invoking `BuildTool.bat` from another batch file, use `call BuildTool.bat <arguments>` so execution returns to the calling script.

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
.\BuildTool Build --target CoreTests
```

The Agent driver serializes operations that use the same registered build profile. If another Configure, Build, Clean, Rebuild, or Test operation already owns that profile's build tree, a second invocation fails with information about the active operation. Different registered Agent profiles retain independent locks.

### Interrupted Build Recovery

Long builds must be allowed to continue under one command invocation. Observe progress by waiting on that invocation; do not set a short command timeout merely to regain control and then launch another build.

If a build is interrupted by a timeout, cancellation, terminal closure, or agent termination:

1. Confirm that the old `cmake`, `ninja`, compiler, and linker process tree has exited. Do not start recovery while it may still be writing outputs.
2. Run a Rebuild of the complete profile:

   ```powershell
   .\BuildTool Rebuild --target all
   ```

3. Only launch the editor after that command succeeds.

The driver leaves an interruption marker when its child process does not return normally. While that marker exists, ordinary Build and Test operations are rejected. `Rebuild` cleans the existing Agent build tree when possible, performs a fresh configure, builds the requested target (`all` by default), and clears the marker only after success. `Clean` and `Configure` may still be used for diagnosis, but they do not clear a pre-existing interruption marker.

DHT writes are committed atomically. Locks are isolated first by build identifier, platform, and profile, then scoped to project metadata or an individual module. Export and reflection generation for the same module remain serialized, while independent modules can run concurrently under Ninja. Project preparation also takes the affected module locks because it may clean disabled module directories. Debug and Release presets intentionally share configuration-independent generated metadata inside one worktree, while identifier-specific workflows use independent intermediate roots and locks.

Existing Agent worktrees need no migration step after switching from the former `*-Agent` preset. The next driver Build or Test action configures the Tests preset automatically when needed, and obsolete ignored `Debug-Agent` artifacts are no longer read.

### Runtime Smoke Tests

A runtime smoke test must only be performed after a successful full build of the same Agent Build Profile. Partial and single-target builds are useful only for development iteration; they do not establish that the executable and all runtime-loaded modules are mutually up to date.

Use the registered Agent build driver and build the `all` target before launching the editor:

```powershell
.\BuildTool Build --target all
& "Engine/Binaries/Win64/Debug/Runtime/DurinEditor/DurinEditor.exe"
```

Agents are authorized to execute the editor executable produced in their dedicated worktree for smoke validation. A human IDE may open that same worktree for debugging after the Agent build has finished, but IDE and Agent build operations must not run concurrently on the shared build tree.

### IDE Debugging In An Agent Worktree

Open the Agent worktree root in a separate IDE window and select `Win64-Debug-DurinEditor` for the IDE code model. IDE Configure and CMake Reload use their own `Build/Win64-Debug-DurinEditor` tree and are allowed when no Agent build is active. Build, Clean, Rebuild, and Test remain driver-owned operations and must use `BuildTool.bat`; disable IDE build-before-launch steps. The IDE can then debug the exact source revision and the Agent-produced `Engine/Binaries/Win64/Debug/Runtime/DurinEditor/DurinEditor.exe` without source remapping. Set `VSLANG=1033` in the IDE CMake profile as well so its Configure and dependency scanning use the same MSVC language.

## Run And Output Layout

Run the editor from:

- `Engine/Binaries/Win64/Debug/Runtime/DurinEditor/DurinEditor.exe`

Key output locations:

- Runtime launcher and module DLLs: `Engine/Binaries/<Platform>/<Config>/Runtime/<Profile>/`
- Shared runtime third-party DLLs: `Engine/Binaries/<Platform>/<Config>/ThirdParty/`
- Native test executables: `Engine/Binaries/<Platform>/<Config>/Tests/<Profile>/Bin/`

When `DURIN_BUILD_IDENTIFIER` is set by a specialized workflow, `<Config>` becomes `<Config>-<Identifier>`.

DHT metadata is configuration-independent. It uses `Engine/Intermediate/Build/<Platform>/<Profile>/` when the identifier is empty and `Engine/Intermediate/Build-<Identifier>/<Platform>/<Profile>/` when set. Normal Debug and Release presets share `Engine/Intermediate/Build/Win64/DurinEditor/` inside a worktree.

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
