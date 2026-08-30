#pragma once

#include "Hash/XxHash.h"
#include "Misc/FileFingerprintCache.h"

namespace Durin
{
	// Records one portable dependency identity beside its machine-local fingerprint.
	struct FShaderPortableDependency
	{
		std::string VirtualPath;
		FXxHash64 ContentHash{};

		auto operator==(const FShaderPortableDependency&) const -> bool = default;
	};

	// Combines a portable source-tree identity with machine-local validation facts.
	struct FShaderMetaData
	{
		FXxHash128 SourceTreeSignature{};
		std::vector<FFileFingerprint> Dependencies;
		std::vector<FShaderPortableDependency> PortableDependencies;
	};

	// Addresses a local dependency manifest before its source-tree signature is known.
	struct FShaderDependencyKey
	{
		FXxHash128 Value{};
		std::string Hex;
	};

	// Identifies one portable shader variant before requested outputs are appended.
	struct FShaderVariantKey
	{
		FXxHash128 Value{};
		std::string Hex;
	};

	// Persists machine-local dependency fingerprints; compiled output is not owned here.
	class FShaderDependencyManifestStore
	{
	public:
		auto Load(std::string_view VirtualShaderPath,
			const FShaderDependencyKey& DependencyKey,
			FShaderMetaData& OutMetaData) -> bool;
		auto Save(std::string_view VirtualShaderPath,
			const FShaderDependencyKey& DependencyKey,
			const FShaderMetaData& MetaData) -> bool;
	};
}
