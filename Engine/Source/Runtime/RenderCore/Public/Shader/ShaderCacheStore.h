#pragma once

#include "RenderCoreAPI.h"
#include "Hash/XxHash.h"

namespace Durin
{
	struct FShaderCompileOptions;
	struct FShaderCompilerOutput;

	// Source-level cache identity shared by all entry points and frequencies compiled from the same shader file.
	struct FShaderMetaData
	{
		FXxHash128 SourceTreeSignature{};
	};

	// Identifies a specific compiled variant (source tree + macros).
	struct FShaderVariantKey
	{
		FXxHash128 Value{};
		std::string Hex;
	};

	// File-system storage for compiled shader artifacts.
	// Owns IO and JSON serialization; does not perform compilation or hash computation.
	class FShaderCacheStore
	{
	public:
		RENDERCORE_API FShaderCacheStore();
		RENDERCORE_API ~FShaderCacheStore();

		FShaderCacheStore(const FShaderCacheStore&) = delete;
		auto operator=(const FShaderCacheStore&) -> FShaderCacheStore& = delete;

		// Load the .slang.meta file for a virtual shader path. Returns false on any IO / parse / schema error.
		RENDERCORE_API auto LoadMetaData(std::string_view VirtualShaderPath, FShaderMetaData& OutMetaData) -> bool;

		// Write the .slang.meta file.
		RENDERCORE_API auto SaveMetaData(std::string_view VirtualShaderPath, const FShaderMetaData& MetaData) -> bool;

		// Try to load pre-compiled .spv artifacts from the variant directory.
		// Metadata validation must be performed by the caller before invoking this.
		RENDERCORE_API auto TryLoad(std::string_view VirtualShaderPath, const FShaderCompileOptions& Options, const FShaderVariantKey& VariantKey, FShaderCompilerOutput& OutOutput) -> bool;

		// Write compiled .spv artifacts to the variant directory.
		RENDERCORE_API auto Save(std::string_view VirtualShaderPath, const FShaderCompileOptions& Options, const FShaderVariantKey& VariantKey, const FShaderCompilerOutput& Output) -> bool;
	};
}
