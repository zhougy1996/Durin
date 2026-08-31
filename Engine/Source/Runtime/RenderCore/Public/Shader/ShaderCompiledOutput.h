#pragma once

#include "Shader/ShaderCompilerCore.h"

namespace Durin::ShaderCompiledOutput
{
	inline constexpr uint32 PayloadMagic = 0x44485344;
	inline constexpr uint32 PayloadSchemaVersion = 1;
	inline constexpr uint32 BuilderVersion = 1;
	inline constexpr uint32 MaximumEntryPoints = 32;
	inline constexpr uint64 MaximumValueBytes = 256ull * 1024ull * 1024ull;
	RENDERCORE_API auto Encode(const FShaderCompileOptions& Options,
		const FShaderCompilerOutput& Output, FByteArray& OutBytes,
		std::string& OutError) -> bool;
	RENDERCORE_API auto Decode(std::span<const std::byte> Bytes,
		const FShaderCompileOptions& Options, FShaderCompilerOutput& OutOutput,
		std::string& OutError) -> bool;
}
