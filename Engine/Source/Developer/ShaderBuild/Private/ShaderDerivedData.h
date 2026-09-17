#pragma once

#include "DerivedDataCache/DerivedDataCache.h"
#include "Shader/ShaderCompiledOutput.h"
#include "ShaderCompileUtilities.h"

namespace Durin::ShaderDerivedData
{
	inline constexpr uint32 PayloadMagic = ShaderCompiledOutput::PayloadMagic;
	inline constexpr uint32 PayloadSchemaVersion =
		ShaderCompiledOutput::PayloadSchemaVersion;
	inline constexpr uint32 BuilderVersion = ShaderCompiledOutput::BuilderVersion;
	inline constexpr uint32 MaximumEntryPoints =
		ShaderCompiledOutput::MaximumEntryPoints;
	inline constexpr uint64 MaximumValueBytes =
		ShaderCompiledOutput::MaximumValueBytes;

	auto BuildKey(
		const FShaderVariantKey& VariantKey,
		const FShaderCompileOptions& Options) -> DerivedData::FCacheKey;
	inline auto Encode(
		const FShaderCompileOptions& Options,
		const FShaderCompilerOutput& Output,
		FByteBuffer& OutBytes) -> FShaderOperationResult
	{
		return ShaderCompiledOutput::Encode(Options, Output, OutBytes);
	}
	inline auto Decode(
		FByteView Bytes,
		const FShaderCompileOptions& Options,
		FShaderCompilerOutput& OutOutput) -> FShaderOperationResult
	{
		return ShaderCompiledOutput::Decode(Bytes, Options, OutOutput);
	}
}
