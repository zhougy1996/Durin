# AGENTS.md

This file provides guidance to Codex (Codex.ai/code) when working with code in this repository.

## Read first

- For configure, build, run, and binary layout, read `Documentation/Setup/BuildAndRun.md`.
- For third-party bootstrap and dependency deployment, read `Documentation/Setup/ThirdPartyBootstrap.md`.
- For native tests, read `Documentation/Setup/NativeTests.md`.
- For build-system structure and generated metadata flow, read `Documentation/Architecture/BuildSystem.md`.
- For profile semantics and generated metadata flow, read `Documentation/Architecture/Profiles.md`.
- For runtime boot flow and subsystem relationships, read `Documentation/Architecture/RuntimeArchitecture.md`.
- If present, read `LOCAL_ENV.md` for machine-specific tool paths and non-portable command examples.

## Important repo-specific behaviors

- This codebase relies on generated build metadata and generated reflection/export files. If a module looks incomplete from static files alone, inspect the generated/intermediate CMake and DHT outputs before assuming the source is missing.
- Shared library naming matters: runtime module loading expects the `DurinEditor-<Module>` naming convention established in `CMake/Modules.cmake`.
- The active rendering backend is effectively Vulkan-first today; changes in `RHI` often need matching updates in `VulkanRHI` and sometimes in the Mona ImGui backend.
- Because the launcher creates a real windowed application, UI/rendering changes should be validated by building and running `DurinEditor`, not only by compiling.
