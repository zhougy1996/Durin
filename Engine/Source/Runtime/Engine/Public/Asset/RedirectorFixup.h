#pragma once

#include "EngineAPI.h"
#include "Asset/MutationExtensions.h"
#include "Asset/References.h"

namespace Durin::Asset
{
	struct FAssetRedirectorFixupMapping
	{
		FPackagePath RedirectorPath;
		FPackagePath FinalPath;

		auto operator==(const FAssetRedirectorFixupMapping&) const -> bool = default;
	};

	enum class EAssetRedirectorFixupMode : uint8
	{
		RewriteOnly,
		RewriteAndDelete
	};

	class FAssetRedirectorFixupSummary
	{
	public:
		ENGINE_API auto GetMode() const -> EAssetRedirectorFixupMode;
		ENGINE_API auto GetRegistryRevision() const -> uint64;
		ENGINE_API auto GetRedirectors() const -> std::span<const FPackagePath>;
		ENGINE_API auto GetFinalPathMappings() const
			-> std::span<const FAssetRedirectorFixupMapping>;
		ENGINE_API auto GetPackageOccurrences() const
			-> std::span<const FAssetReferenceEdge>;
		ENGINE_API auto GetStoreOccurrences() const
			-> std::span<const FAssetReferenceStoreOccurrence>;
		ENGINE_API auto GetDeletableRedirectors() const
			-> std::span<const FPackagePath>;

	private:
		EAssetRedirectorFixupMode Mode =
			EAssetRedirectorFixupMode::RewriteOnly;
		uint64 RegistryRevision = 0;
		std::vector<FPackagePath> Redirectors;
		std::vector<FAssetRedirectorFixupMapping> FinalPathMappings;
		std::vector<FAssetReferenceEdge> PackageOccurrences;
		std::vector<FAssetReferenceStoreOccurrence> StoreOccurrences;
		std::vector<FPackagePath> DeletableRedirectors;

#if defined(DURIN_ENGINE_ASSET_INTERNAL)
		friend class FAssetMutationCoordinator;
#endif
	};

	ENGINE_API auto PrepareRedirectorFixupTransaction(
		std::span<const FPackagePath> Redirectors,
		EAssetRedirectorFixupMode Mode,
		FAssetRedirectorFixupSummary& OutSummary,
		FAssetMutationTransaction& OutTransaction
	) -> FAssetResult;
} // namespace Durin::Asset
