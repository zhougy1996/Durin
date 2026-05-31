# Shader Cache

This document describes the current Slang shader cache layout used by `RenderCore`.

## Overview

The cache is split into two layers:

- Source-level metadata stored once per virtual shader path
- Variant artifacts stored in per-hash subdirectories

The intent is to keep dependency tracking and artifact lookup separate:

- `*.slang.meta` answers whether the shader source tree still matches the last known state
- Variant directories answer whether a specific macro set has already produced compiled binaries

## Paths

For a virtual shader path such as `/Engine/ImGui`, the cache layout is:

```text
Engine/ShaderCache/SPIR-V/ImGui/
  ImGui.slang.meta
  <variant-hash>/
    vertexMain.vs.spv
    fragmentMain.ps.spv
```

The root directory comes from the registered shader mount point. The file and directory naming rules are implemented in [ShaderPaths.cpp](G:/Workspace/Durin/Engine/Source/Runtime/RenderCore/Private/Shader/ShaderPaths.cpp).

## Metadata File

Each shader root directory contains one JSON metadata file named `<ShaderName>.slang.meta`.

Example shape:

```json
{
  "version": 1,
  "macroSchemaVersion": 1,
  "virtualShaderPath": "/Engine/ImGui",
  "backend": "slang",
  "targetFormat": "SPIR-V",
  "targetProfile": "spirv_1_5",
  "mainSourceHash": "...",
  "sourceTreeSignature": "...",
  "dependencies": [
    {
      "path": "/Engine/ImGui",
      "size": 1234,
      "hash": "..."
    }
  ]
}
```

Field semantics:

- `virtualShaderPath`: stable shader identity used by callers and cache lookup
- `mainSourceHash`: hash of the root `.slang` file contents
- `sourceTreeSignature`: combined signature of the root file and every resolved dependency
- `dependencies`: dependency list recorded as virtual shader paths when possible, otherwise normalized physical paths

## Variant Hash

The variant directory hash is computed from:

- cache key version
- backend, target format, and target profile
- `virtualShaderPath`
- `sourceTreeSignature`
- normalized macro definitions

It intentionally does not include:

- entry point
- shader frequency

That allows multiple entry points compiled from the same source tree and macro set to share one variant directory.

## Artifact Naming

Artifacts are separated by entry point and stage suffix:

- `vertexMain.vs.spv`
- `fragmentMain.ps.spv`
- `main.cs.spv`

The stage suffix comes from `EShaderFrequency`.

## Cache Flow

At compile time, `FSlangShaderCompiler` does the following:

1. Create a fresh Slang session and inject normalized macros
2. Resolve the shader dependency graph
3. Compute `mainSourceHash`, `sourceTreeSignature`, and the variant hash
4. Load `<ShaderName>.slang.meta`
5. Compare the current source-level metadata with the cached metadata
6. If metadata matches, try to load the expected `.spv` artifacts from the variant directory
7. Otherwise compile, then write the `.spv` artifacts and overwrite the root metadata file

## Notes

- Duplicate macro names are rejected before compilation.
- Old `manifest.txt` layouts are not migrated automatically and are no longer read by the current implementation.
- Virtual shader paths are preferred in metadata to avoid embedding machine-specific absolute source paths in normal cache records.
