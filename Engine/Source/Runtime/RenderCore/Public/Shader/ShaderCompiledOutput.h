#pragma once

#include "Shader/ShaderDiagnostics.h"

#include "Shader/ShaderCompilerCore.h"

namespace Durin::ShaderCompiledOutput
{
	inline constexpr uint32 PayloadMagic = 0x44485344;
	inline constexpr uint32 PayloadSchemaVersion = 1;
	inline constexpr uint32 BuilderVersion = 1;
	inline constexpr uint32 MaximumEntryPoints = 32;
	inline constexpr uint64 MaximumValueBytes = 256ull * 1024ull * 1024ull;
	RENDERCORE_API auto Encode(
		const FShaderCompileOptions& Options,
		const FShaderCompilerOutput& Output,
		FByteBuffer& OutBytes) -> FShaderOperationResult;
	RENDERCORE_API auto Decode(
		FByteView Bytes,
		const FShaderCompileOptions& Options,
		FShaderCompilerOutput& OutOutput) -> FShaderOperationResult;
}
