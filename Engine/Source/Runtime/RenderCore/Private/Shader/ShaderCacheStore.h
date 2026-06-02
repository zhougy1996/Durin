#pragma once

#include "Hash/XxHash.h"
#include "Shader/ShaderCompilerCore.h"

namespace Durin
{
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
		FShaderCacheStore();
		~FShaderCacheStore();

		FShaderCacheStore(const FShaderCacheStore&) = delete;
		auto operator=(const FShaderCacheStore&) -> FShaderCacheStore& = delete;

		// Load the .slang.meta file for a virtual shader path. Returns false on any IO / parse / schema error.
		auto LoadMetaData(std::string_view VirtualShaderPath, FShaderMetaData& OutMetaData) -> bool;

		// Write the .slang.meta file.
		auto SaveMetaData(std::string_view VirtualShaderPath, const FShaderMetaData& MetaData) -> bool;

		// Try to load pre-compiled .spv artifacts from the variant directory.
		// Metadata validation must be performed by the caller before invoking this.
		auto TryLoad(std::string_view VirtualShaderPath, const FShaderCompileOptions& Options, const FShaderVariantKey& VariantKey, FShaderCompilerOutput& OutOutput) -> bool;

		// Write compiled .spv artifacts to the variant directory.
		auto Save(std::string_view VirtualShaderPath, const FShaderCompileOptions& Options, const FShaderVariantKey& VariantKey, const FShaderCompilerOutput& Output) -> bool;
	};
}
