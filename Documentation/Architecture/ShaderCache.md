# Shader Cache

This document describes the current Slang shader cache strategy used by `RenderCore`.

## Overview

Shader cache lookup is owned by the RenderCore shader compile service, exposed through `GetOrCompileShader()`. Backend compilers such as `FSlangShaderCompiler` compile physical source files only; they do not resolve virtual shader paths or read/write disk cache.

The cache has two layers:

- Source metadata stored once per virtual shader path.
- Compiled SPIR-V artifacts stored under one variant directory per source tree and macro set.

`FFileFingerprintCache` caches physical dependency fingerprints in memory for the lifetime of the shader compile service. It avoids repeated file reads and hash calculations when multiple shaders depend on the same physical file.

## Paths

The cache root comes from the registered shader mount point. For the default engine mount:

```text
/Engine/ -> Engine/Shaders/Slang/
cache   -> Engine/ShaderCache/SPIR-V/
```

Cache directories preserve the virtual shader path hierarchy, but the final shader node gets a `.slang` suffix so a shader file does not conflict with a same-named folder.

Examples:

```text
/Engine/ImGui
=> Engine/ShaderCache/SPIR-V/ImGui.slang/

/Engine/ImGui/Button
=> Engine/ShaderCache/SPIR-V/ImGui/Button.slang/
```

Each shader directory contains:

```text
Shader.slang.meta
<VariantKey>/
  vertexMain.spv
  fragmentMain.spv
```

Old layouts such as `SPIR-V/ImGui/ImGui.slang.meta`, `vertexMain.vs.spv`, or `fragmentMain.ps.spv` are not migrated or read for compatibility.

## Metadata

The metadata file is fixed as `Shader.slang.meta` inside the shader cache directory.

Current schema:

```json
{
  "Version": 2,
  "SourceTreeSignature": "..."
}
```

`SourceTreeSignature` is built from the normalized physical paths, file sizes, and content hashes of the root shader file and every resolved dependency. If the metadata file does not exist, cache lookup treats it as a normal miss and does not log a file-load warning.

## Variant Key And Artifacts

The variant key includes:

- Key version.
- Backend, target format, and target profile.
- Virtual shader path.
- Current source tree signature.
- Normalized macro definitions.

Entry points and shader frequencies are intentionally not part of the variant key. Multiple entry points compiled from the same source tree and macro set share one variant directory.

Compiled artifacts are named only by entry point:

```text
vertexMain.spv
fragmentMain.spv
main.spv
```

If two requested entry points sanitize to the same file name, binary cache load/save is skipped for that compile request and real compilation still proceeds.

## Cache Flow

`GetOrCompileShader()` performs the full cache flow:

1. Resolve the virtual shader path to a physical `.slang` source path.
2. Resolve Slang dependency files through the private dependency resolver.
3. Build current source metadata using `FFileFingerprintCache`.
4. Build the variant key from virtual identity, source signature, and normalized macros.
5. If metadata exists and matches, try to load all expected `.spv` artifacts.
6. On cache miss or forced recompile, log a debug message and invoke `FSlangShaderCompiler` for real physical compilation.
7. After successful compilation, write binary artifacts and save `Shader.slang.meta`.

`bForceRecompile` skips binary cache load, but still resolves dependencies and updates metadata/artifacts after a successful compile.

## Notes

- Duplicate macro names are rejected before dependency resolution or compilation.
- Virtual shader paths are cache identities owned by callers of `GetOrCompileShader()`.
- Physical source paths are consumed only by the private compile and dependency-resolution steps.
- The current schema intentionally does not support old metadata compatibility or migration.
