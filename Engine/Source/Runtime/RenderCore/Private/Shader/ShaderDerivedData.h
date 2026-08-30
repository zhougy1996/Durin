#pragma once

#include "DerivedDataCache/DerivedDataCache.h"
#include "Shader/ShaderCompilerCore.h"
#include "ShaderDependencyManifestStore.h"

namespace Durin::ShaderDerivedData
{
	inline constexpr uint32 PayloadMagic = 0x44485344;
	inline constexpr uint32 PayloadSchemaVersion = 1;
	inline constexpr uint32 BuilderVersion = 1;
	inline constexpr uint32 MaximumEntryPoints = 32;
	inline constexpr uint64 MaximumValueBytes = 256ull * 1024ull * 1024ull;
	inline constexpr uint64 BucketBudgetBytes = 2ull * 1024ull * 1024ull * 1024ull;
	inline constexpr uint32 CleanupDeleteLimit = 16;

	auto GetBucket() -> DerivedData::FCacheBucket;
	auto BuildKey(const FShaderVariantKey& VariantKey,
		const FShaderCompileOptions& Options) -> DerivedData::FCacheKey;
	auto Encode(const FShaderCompileOptions& Options,
		const FShaderCompilerOutput& Output, std::vector<std::byte>& OutBytes,
		std::string& OutError) -> bool;
	auto Decode(std::span<const std::byte> Bytes,
		const FShaderCompileOptions& Options, FShaderCompilerOutput& OutOutput,
		std::string& OutError) -> bool;
}
