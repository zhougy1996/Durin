# Build And Run

This is the operational guide for configuring, building, testing, and debugging Durin locally.

## Setup

Install Python 3.10 or newer, Visual Studio 2022 with the **Desktop development with C++** workload and the English language pack, Git, CMake, and the Vulkan SDK. Then run `Setup.bat` once in every new Windows checkout or worktree.

In a normal checkout, `Setup.bat` creates `.venv`, installs the pinned Python dependencies from `requirements.txt` (including the `clang.cindex` bindings and native `libclang` library), creates the local Agent configuration, and prepares all third-party libraries. The operation is idempotent and can be rerun after an interrupted download. In a linked Git worktree it instead links `Engine/External` and `.venv` from the prepared main worktree.

`BuildTool.bat` intentionally requires `.venv`; it will ask you to run `Setup.bat` rather than silently using a different system Python.

Machine-specific CMake, profile, environment, or job overrides belong in `.agents/build-config.json`. Normally, leave them empty and let the build driver detect the Visual Studio environment and parallelism.

## Windows Workflow

A checkout has one source/build writer at a time. An Agent may own the current checkout; a separate worktree is needed only for concurrent Agents, branches, or human editing/build workflows. An IDE may observe and debug an Agent-owned checkout, but it must not build it.

Use the root wrapper for configuration, builds, and tests:

```powershell
.\BuildTool configure
.\BuildTool configure --fresh
.\BuildTool build --target LevelEditor
.\BuildTool run
.\BuildTool test --target CoreTests --filter FJsonDocumentTests.*
.\BuildTool clean
.\BuildTool rebuild --target all
```

Commands are case-insensitive for compatibility, but lowercase is canonical. `build` and `test` configure automatically when needed, so an explicit first `configure` is optional. Omit `--jobs` to use automatic parallelism; pass `--jobs <count>` only when a local limit is required. From another batch file, use `call BuildTool.bat <arguments>`.

An ordinary `configure` preserves the existing CMake cache. Pass `--fresh` to discard it explicitly. `rebuild` and automatic recovery from an unusable or incompatible build tree always fresh-configure before building.

BuildTool separates its resolved context, execution stages, raw CMake/Ninja output, and final result so failures remain identifiable in long logs. Styled output is enabled for interactive terminals. Pass `--plain`, set `NO_COLOR`, or redirect the output to select stable text-only output without ANSI sequences:

```powershell
.\BuildTool build --target all --plain
```

On Windows, the first toolchain-backed command captures and validates the Visual Studio environment. BuildTool caches that environment delta under `Build/.agent-state/` so later invocations avoid rerunning `VsDevCmd.bat` and the compiler language probe. The cache refreshes automatically when the setup script, its arguments, or `cl.exe` changes, while caller-provided environment values and `PATH` changes remain live.

The registered Windows build environment defaults to `Win64-Debug-DurinEditor-Tests`, allowing the same output set to run the editor and native tests. Before launching the editor for a smoke test or final validation, build the complete runtime:

```powershell
.\BuildTool build --target all
.\BuildTool run
```

`run` launches the existing runtime executable selected by the preset, such as
`DurinEditor.exe` or `DurinGame.exe`; it does not build implicitly. On Windows,
BuildTool keeps relaunched runtime descendants in the same tracked process job,
so opening another editor project does not return from `run` or release the
checkout lock until the final editor instance exits. Pass runtime arguments after
the final `--args` option:

```powershell
.\BuildTool run --preset Win64-Debug-DurinGame --args -ExampleArgument
```

Select another registered configure preset with `--preset`:

```powershell
.\BuildTool build --preset Win64-Release-DurinEditor --target all
.\BuildTool rebuild --preset Win64-Shipping-DurinGame --target all
```

`CMakePresets.json` remains the source of truth for preset configuration. `AgentBuildProfiles.json` controls which presets BuildTool may own for each host environment. The IDE-only `Win64-Debug-DurinEditor-FastConfigure` preset is intentionally excluded.

## Interactive Shell

Run `BuildTool` without arguments, or pass `shell`, to open the human-oriented command shell:

```powershell
.\BuildTool
.\BuildTool shell
```

The selected preset is session-local and does not modify `.agents/build-config.json`:

```text
BuildTool> /presets
   1  Win64-Debug-DurinEditor
   2  Win64-Debug-DurinEditor-Tests [default, current]
   3  Win64-Debug-DurinGame
   4  Win64-Release-DurinEditor
   5  Win64-Release-DurinGame
   6  Win64-Shipping-DurinGame
BuildTool> 4
BuildTool> /preset
CMake preset: "Win64-Release-DurinEditor"
BuildTool> /preset Win64-Debug-DurinGame
BuildTool> /configure --fresh
BuildTool> /build
BuildTool> /rebuild DurinLauncher
BuildTool> /test CoreTests FJsonDocumentTests.*
BuildTool> /run
BuildTool> /open-runtime
BuildTool> /status
BuildTool> /exit
```

`/presets` displays the registered list, prints an input hint, and accepts a number on the next `BuildTool>` prompt. `/preset` without an argument displays the current preset; with an argument it requires the full preset name. `/build` and `/rebuild` default to target `all`. `/run [arguments...]` launches the current preset's existing runtime executable and returns to the shell when it exits. `/open-runtime` opens the selected preset's existing runtime directory in the platform file manager. `/status` reports the resolved profile, preset, build directory, configuration, CMake command, parallelism, and interruption recovery state. Use `/help` for the complete command list. Shell commands reuse the environment resolved when the shell starts, so switching presets does not rerun Visual Studio environment discovery.

## Clean And Purge

`clean` invokes the CMake clean target for the selected preset. It removes outputs known to that generated build graph, but keeps the configured CMake tree and may leave copied runtime files or generated metadata that CMake does not own.

`purge` removes the selected preset's configured build and install trees plus its project output and generated-metadata roots:

```powershell
.\BuildTool purge --preset Win64-Release-DurinEditor
.\BuildTool purge --preset Win64-Release-DurinEditor --yes
```

Inside the interactive shell:

```text
BuildTool> /purge
BuildTool> /purge --yes
BuildTool> /purge --all-presets
```

Purge asks for explicit confirmation unless `--yes` is supplied: enter `PURGE` for the current preset or `PURGE ALL` for the all-presets scope. Use `--all-presets` to remove artifacts for every preset registered to the selected Agent host profile:

```powershell
.\BuildTool purge --all-presets
.\BuildTool purge --all-presets --yes
```

Preset build trees are isolated, but final binaries are shared by platform/configuration and DHT metadata is shared by platform/profile. Purging one preset therefore also invalidates those shared outputs for other presets using the same configuration or profile. A subsequent build regenerates them normally.

Purge only removes registered preset trees under `Build/` and `Install/`, project `Binaries/<Platform>/<Config>/` roots, and project `Intermediate/Build[-Identifier]/<Platform>/<Profile>/` roots. It intentionally preserves bootstrapped dependencies such as `Build/ThirdParty` and `Engine/External`.

On non-Windows hosts, invoke `.venv/bin/python Engine/Scripts/Build/durin_build_tool/__main__.py <arguments>` directly after preparing an equivalent virtual environment. Windows callers must use `BuildTool.bat`. BuildTool enforces `VSLANG=1033` after Visual Studio environment setup and verifies that MSVC actually emits English diagnostics. This keeps CMake's `/showIncludes` dependency prefix stable for both interactive terminals and Agent output pipes. If validation reports a localized prefix, add the English language pack through Visual Studio Installer. The next `configure`, `build`, or `test` refreshes any existing Ninja tree that does not already contain the English dependency prefix.

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

For Agent-driven builds, prevent recovery from being needed: give the shell invocation a timeout of at least one hour (longer for a full build), and keep waiting on the same yielded invocation until it produces a final exit result. A runner yield, cell ID, quiet output, or elapsed UI window does not mean that BuildTool stopped and must not trigger a second build or recovery-state inspection.

Do not start a second build while an earlier CMake, Ninja, compiler, or linker process tree may still be running. If a build is cancelled, times out, or its terminal closes, wait for that process tree to exit and run:

```powershell
.\BuildTool rebuild --target all
```

When the interrupted operation used a non-default preset, add `--preset <affected-preset>` or select that preset in the interactive shell before rebuilding.

Use the same recovery after an accidental IDE build. IDE outputs share the final binary directory, and their timestamps can make an incremental Agent build incorrectly report that everything is current. The driver also blocks unsafe incremental `build` and `test` operations for the affected preset after a detected interruption until a `rebuild` succeeds.

BuildTool serializes all registered presets with one checkout-level ownership lock because different CMake trees can still share final outputs and generated metadata. Do not mix direct CMake build operations with `BuildTool` ownership of the same checkout.

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
