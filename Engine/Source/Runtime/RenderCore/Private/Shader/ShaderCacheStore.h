#pragma once

#include "Hash/XxHash.h"
#include "Misc/FileFingerprintCache.h"
#include "Shader/ShaderCompilerCore.h"

namespace Durin
{
	// Source-level cache identity shared by all entry points and frequencies compiled from the same shader file.
	struct FShaderMetaData
	{
		FXxHash128 SourceTreeSignature{};
		std::vector<FFileFingerprint> Dependencies;
	};

	// Addresses the dependency manifest before the current source-tree signature is known.
	struct FShaderDependencyKey
	{
		FXxHash128 Value{};
		std::string Hex;
	};

	// Identifies a specific compiled variant (source tree + macros).
	struct FShaderVariantKey
	{
		FXxHash128 Value{};
		std::string Hex;
	};

	struct FShaderCacheRetentionPolicy
	{
		uint32 MaxVariantsPerShader = 64;
		uint64 MaxBytesPerShader = 256ull * 1024ull * 1024ull;
	};

	// File-system storage for compiled shader artifacts.
	// Owns IO and JSON serialization; does not perform compilation or hash computation.
	class FShaderCacheStore
	{
	public:
		explicit FShaderCacheStore(FShaderCacheRetentionPolicy RetentionPolicy = {});
		~FShaderCacheStore();

		FShaderCacheStore(const FShaderCacheStore&) = delete;
		auto operator=(const FShaderCacheStore&) -> FShaderCacheStore& = delete;

		// Load the macro-specific dependency manifest. Returns false on any IO / parse / schema error.
		auto LoadMetaData(std::string_view VirtualShaderPath, const FShaderDependencyKey& DependencyKey, FShaderMetaData& OutMetaData) -> bool;

		// Atomically write the macro-specific dependency manifest.
		auto SaveMetaData(std::string_view VirtualShaderPath, const FShaderDependencyKey& DependencyKey, const FShaderMetaData& MetaData) -> bool;

		// Try to load pre-compiled .spv artifacts from the variant directory.
		auto TryLoad(std::string_view VirtualShaderPath, const FShaderCompileOptions& Options, const FShaderVariantKey& VariantKey, FShaderCompilerOutput& OutOutput) -> bool;

		// Write compiled .spv artifacts to the variant directory.
		auto Save(std::string_view VirtualShaderPath, const FShaderCompileOptions& Options, const FShaderVariantKey& VariantKey, const FShaderCompilerOutput& Output) -> bool;

	private:
		auto EnforceRetention(std::string_view VirtualShaderPath, std::string_view ProtectedVariantKey) -> void;

		FShaderCacheRetentionPolicy RetentionPolicy;
		std::mutex RetentionMutex;
	};
}
