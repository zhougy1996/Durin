# Build System

This document explains where Durin's CMake entrypoints live, how generated metadata flows through the build, and where module output naming is defined.

## Overview

The repository configures from the root `CMakeLists.txt` and delegates into `Engine/CMakeLists.txt`. Most engine and editor code is built as loadable modules rather than a monolithic executable.

The main build entrypoints are:

- `CMake/DurinWorkspaceSetup.cmake`
- `CMake/DurinBuildApi.cmake`
- `CMake/Project/ProjectSetup.cmake`
- `CMake/Project/ProjectTargets.cmake`

Important helper APIs:

- `add_durin_project(...)`
- `add_durin_module(...)`
- `add_durin_test(...)`

## Windows Process Manifests

`durin_target_enable_windows_long_paths(...)` applies the repository-owned
Windows manifest to process images. `DurinLauncher` calls it directly, and
`add_durin_test(...)` applies it to every native-test executable. Module DLLs do
not carry this process capability.

CMake attaches `CMake/Windows/DurinLongPathAware.manifest` as a target source so
the MSVC manifest tool merges it into the final executable. A post-link check
extracts the embedded resource and requires exactly one
`longPathAware=true` declaration. Host policy is a separate prerequisite
enforced by Setup and DurinDevTool; see
[Build and Run](BuildAndRun.md).

## Generated Metadata Flow

`add_durin_project(...)` invokes DurinHeaderTool, imports generated project
metadata from
`Engine/Intermediate/Build/<Platform>/<RuntimeVariant>/...`,
resolves active runtime-variant values, and then adds module subdirectories for
the current project.

DHT uses atomic replacement and cross-process locks scoped by platform, runtime
variant, project, and module. Conflicting writers serialize while independent
modules remain parallel. Presets in one worktree intentionally share
configuration-independent metadata.

Ninja schedules build-time DHT commands through the `durin_dht` job pool. Each
command receives an explicit parser-worker limit, and module-internal parallelism
scales with the number of headers requiring parsing: fewer than 8 uses one worker,
8-15 uses at most two, 16-31 uses at most four, and larger sets may use the
configured limit. The defaults balance large-module incremental builds with
Ninja-level module and compiler scheduling: at most two DHT commands run at once,
and each uses at most four parser workers. `DURIN_DHT_JOB_POOL_SIZE` and
`DURIN_DHT_WORKERS` are cache settings intended for measured preset or CI tuning;
worker count is constrained to 1-8.

DHT emits one INFO summary per module export/reflection command. Per-header timing,
cache, dependency-loading, and worker details are DEBUG-only. Set the
`DURIN_DHT_LOG_LEVEL` cache setting to `DEBUG` for diagnostics or `WARNING` for
Ninja-only progress unless DHT reports a problem.

Generated metadata is part of the source of truth. If a module appears
incomplete, inspect its `Engine/Intermediate/Build/...` metadata
and DHT output before assuming source files are missing.

Project entry scripts such as `Engine/CMake/EngineSetup.cmake` and `Sandbox/CMake/SandboxSetup.cmake` run before that helper and may perform project-specific setup such as third-party registration.

`add_durin_module(...)` imports generated per-module CMake metadata, wires reflection-generated sources and export files, applies shared PCH settings, and builds the resulting shared or static library.

When `BUILD_TESTING` is enabled, `add_durin_project(...)` registers native tests
from `DURIN_PROJECT_TESTS_DIR`. The default is
`<ProjectRoot>/Tests/Native`; a project may override it before calling
`add_durin_project(...)`. Test subdirectories and GoogleTest are excluded from
CMake's default `all` target. `DurinNativeTests` explicitly aggregates every
native-test executable for whole-suite builds.

Ordinary module sources under `Public` and `Private` are discovered by per-module
CMake `GLOB_RECURSE CONFIGURE_DEPENDS` rules. Adding or removing a supported C/C++
source or header therefore updates the Ninja graph during a normal build. DHT
metadata only describes module configuration, reflection inputs, and generated
outputs; it does not freeze the ordinary source list at configure time.

During configuration, CMake hashes the tracked DHT Python package together with
`requirements.txt` into `DHT.fingerprint`. Those files are configure dependencies,
and export/reflection build commands depend on the resulting fingerprint. The
fingerprint is also passed into DHT's private manifests, so a tool implementation
change invalidates both CMake's build edge and DHT's internal cache.

## Build Output Isolation

Each worktree owns its `Build/`, `Binaries/`, and `Intermediate/` trees. Use
separate worktrees when workflows need concurrent writers. Within one worktree,
presets have distinct CMake `binaryDir` values, while final outputs are derived
from the CMake configuration and preset role. Profiling uses the
`Release-Profiling` output configuration; standard Release uses `Release`.
Third-party runtime DLLs are role-independent and are deployed beneath
`Binaries/<Platform>/ThirdParty/<CMakeConfig>/`, so standard Release and
Release Profiling reuse the same copies.

A preset's `binaryDir` isolates CMake, object, and Ninja state only—not DHT
metadata. All presets in a worktree must still follow the single-writer workflow
in `BuildAndRun.md`.

## Module Output Naming

Shared module naming is part of the runtime contract. Current outputs follow
`<RuntimeVariant>-<ModuleName>`, for example:

- `DurinEditor-Core.dll`
- `DurinEditor-RenderCore.dll`
- `DurinGame-Core.dll`

The runtime module loader expects that naming convention.

## Optional Profiling Linkage

`DURIN_ENABLE_TRACY` defaults to `OFF`. Disabled targets receive
`DURIN_WITH_TRACY=0` and the build does not inspect, compile, link, or deploy
Tracy. Profiling presets set `DURIN_WITH_TRACY=1` and link repository targets to
one shared `Tracy::TracyClient` runtime placed beside the selected profiling
launcher. Shipping rejects the option.

## DurinDevTool Command Interface

DurinDevTool's direct parser and interactive shell share one command specification
for command names, slash aliases, accepted options, compact shell operands,
defaults, summaries, and help. Lowercase named syntax is canonical; interactive
slash prefixes and compact operands remain compatibility forms.

`build` and `rebuild` default to target `all`, while `test` requires an explicit
target. An option belongs only to commands that consume it: toolchain and job
overrides are not accepted by artifact-only commands such as `purge`, `run`, or
`open-runtime`. The direct `presets`, `status`, and `open-runtime` actions use the
same display and context paths as their interactive counterparts.

Interactive shell startup resolves only repository configuration, the host build
profile, registered presets, and the selected preset. CMake discovery, job-count
resolution, environment setup, compiler validation, and required-command checks
are deferred until the first configure, build, clean, rebuild, or test request.
The resolved toolchain environment is cached by the shell and reused across
preset switches; operation locking remains scoped to command execution, so an
idle shell does not own the checkout lock. Status output distinguishes unresolved
session defaults from a validated toolchain context.

## Related Docs

- `Documentation/Runtime/Assets/Versioning.md`
- `Documentation/Runtime/Core/ReflectionSystem.md`
- `Documentation/Runtime/Core/GarbageCollection.md`
- `Documentation/Development/Build/BuildAndRun.md`
- `Documentation/Development/Build/RuntimeVariants.md`
- `Documentation/Development/Build/Profiling.md`
- `Documentation/Runtime/Core/RuntimeLifecycle.md`
- `Documentation/Runtime/Core/FileIO.md`
