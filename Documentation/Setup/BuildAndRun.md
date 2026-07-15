# Build And Run

This is the operational guide for configuring, building, testing, and debugging Durin locally.

## Setup

Run `Setup.bat` once in every new Windows checkout or worktree. Build directories and outputs stay local to that checkout; only dependency directories prepared by `Setup.bat` may be shared between worktrees.

Machine-specific CMake, profile, environment, or job overrides belong in `.agents/build-config.json`. Normally, leave them empty and let the build driver detect the Visual Studio environment and parallelism.

## Windows Workflow

A checkout has one source/build writer at a time. An Agent may own the current checkout; a separate worktree is needed only for concurrent Agents, branches, or human editing/build workflows. An IDE may observe and debug an Agent-owned checkout, but it must not build it.

Use the root wrapper for configuration, builds, and tests:

```powershell
.\BuildTool Configure
.\BuildTool Build --target LevelEditor
.\BuildTool Test --target CoreTests --filter FJsonDocumentTests.*
.\BuildTool Clean
.\BuildTool Rebuild --target all
```

`Build` and `Test` configure automatically when needed, so an explicit first `Configure` is optional. Omit `--jobs` to use automatic parallelism; pass `--jobs <count>` only when a local limit is required. From another batch file, use `call BuildTool.bat <arguments>`.

The registered Windows profile uses `Win64-Debug-DurinEditor-Tests`, allowing the same output set to run the editor and native tests. Before launching the editor for a smoke test or final validation, build the complete runtime:

```powershell
.\BuildTool Build --target all
& "Engine/Binaries/Win64/Debug/Runtime/DurinEditor/DurinEditor.exe"
```

On non-Windows hosts, invoke `python Engine/Scripts/Build/agent_build.py <arguments>` directly. Windows callers must use `BuildTool.bat` because it also fixes the MSVC language with `VSLANG=1033`.

## IDE Code Model And Debugging

In CLion, configure the Agent-owned checkout as follows:

1. Select `Win64-Debug-DurinEditor-FastConfigure` as the CMake profile.
2. Set the profile environment to `VSLANG=1033`.
3. Create a Native Application configuration for `Engine/Binaries/Win64/Debug/Runtime/DurinEditor/DurinEditor.exe`.
4. Remove `Build` and all other compilation steps from **Before launch**.
5. Run CMake Configure/Reload only while `BuildTool` is idle.

FastConfigure has its own CMake/Ninja tree and disables PCH artifacts, while still force-including the project PCH headers for the code model. It shares final binaries and generated DHT metadata with the Tests preset, so a separate IDE tree prevents object collisions but does not make concurrent configuration or IDE builds safe.

CMake records MSVC's localized `/showIncludes` prefix during configuration. If raw lines such as `注意: 包含文件:` appear after setting `VSLANG=1033`, reset the IDE CMake cache or remove only `Build/Win64-Debug-DurinEditor-FastConfigure`, then configure it again. Do not remove the driver-owned Tests tree as IDE maintenance.

## Recovery

Do not start a second build while an earlier CMake, Ninja, compiler, or linker process tree may still be running. If a build is cancelled, times out, or its terminal closes, wait for that process tree to exit and run:

```powershell
.\BuildTool Rebuild --target all
```

Use the same recovery after an accidental IDE build. IDE outputs share the final binary directory, and their timestamps can make an incremental Agent build incorrectly report that everything is current. The driver also blocks unsafe incremental Build and Test operations after a detected interruption until a Rebuild succeeds.

## Other Presets

`CMakePresets.json` is the source of truth for configurations not owned by the registered Agent profile, including Release, Shipping, and DurinGame. In a separately human-owned build workflow, list and invoke them with normal CMake commands:

```powershell
cmake --list-presets
cmake --preset Win64-Shipping-DurinGame
cmake --build Build/Win64-Shipping-DurinGame --parallel
```

Do not mix direct CMake build operations with `BuildTool` ownership of the same checkout.

## Output Layout

- Editor: `Engine/Binaries/Win64/Debug/Runtime/DurinEditor/DurinEditor.exe`
- Runtime launcher and modules: `Engine/Binaries/<Platform>/<Config>/Runtime/<Profile>/`
- Third-party runtime DLLs: `Engine/Binaries/<Platform>/<Config>/ThirdParty/`
- Native tests: `Engine/Binaries/<Platform>/<Config>/Tests/<Profile>/Bin/`

The launcher target is `DurinLauncher`, while the executable name follows the active profile. Runtime path discovery assumes the executable remains in this repository-relative layout. If editor startup reports a missing DLL, check the active runtime directory and the shared `ThirdParty` directory.

Build identifiers and DHT intermediate paths are described in `Documentation/Architecture/BuildSystem.md` and `Documentation/Architecture/Profiles.md`.

## Related Docs

- `Documentation/Setup/ThirdPartyBootstrap.md`
- `Documentation/Setup/NativeTests.md`
- `Documentation/Architecture/BuildSystem.md`
- `Documentation/Architecture/Profiles.md`
- `Documentation/Architecture/RuntimeArchitecture.md`
