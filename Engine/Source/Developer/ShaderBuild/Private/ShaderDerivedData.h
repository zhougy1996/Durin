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
	inline constexpr uint64 BucketBudgetBytes =
		2ull * 1024ull * 1024ull * 1024ull;
	inline constexpr uint32 CleanupDeleteLimit = 16;

	auto GetBucket() -> DerivedData::FCacheBucket;
	auto BuildKey(
		const FShaderVariantKey& VariantKey,
		const FShaderCompileOptions& Options) -> DerivedData::FCacheKey;
	inline auto Encode(
		const FShaderCompileOptions& Options,
		const FShaderCompilerOutput& Output,
		std::vector<std::byte>& OutBytes,
		std::string& OutError) -> bool
	{
		return ShaderCompiledOutput::Encode(Options, Output, OutBytes, OutError);
	}
	inline auto Decode(
		std::span<const std::byte> Bytes,
		const FShaderCompileOptions& Options,
		FShaderCompilerOutput& OutOutput,
		std::string& OutError) -> bool
	{
		return ShaderCompiledOutput::Decode(Bytes, Options, OutOutput, OutError);
	}
}
