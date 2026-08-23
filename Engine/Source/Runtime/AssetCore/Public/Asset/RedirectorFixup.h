#pragma once

#include "Asset/MutationExtensions.h"
#include "Asset/References.h"

namespace Durin::Asset
{
	struct FAssetRedirectorFixupMapping
	{
		FAssetPath RedirectorPath;
		FAssetPath FinalPath;

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
		ASSETCORE_API auto GetMode() const -> EAssetRedirectorFixupMode;
		ASSETCORE_API auto GetRegistryRevision() const -> uint64;
		ASSETCORE_API auto GetRedirectors() const -> std::span<const FAssetPath>;
		ASSETCORE_API auto GetFinalPathMappings() const
			-> std::span<const FAssetRedirectorFixupMapping>;
		ASSETCORE_API auto GetPackageOccurrences() const
			-> std::span<const FAssetReferenceEdge>;
		ASSETCORE_API auto GetStoreOccurrences() const
			-> std::span<const FAssetReferenceStoreOccurrence>;
		ASSETCORE_API auto GetDeletableRedirectors() const
			-> std::span<const FAssetPath>;

	private:
		EAssetRedirectorFixupMode Mode =
			EAssetRedirectorFixupMode::RewriteOnly;
		uint64 RegistryRevision = 0;
		std::vector<FAssetPath> Redirectors;
		std::vector<FAssetRedirectorFixupMapping> FinalPathMappings;
		std::vector<FAssetReferenceEdge> PackageOccurrences;
		std::vector<FAssetReferenceStoreOccurrence> StoreOccurrences;
		std::vector<FAssetPath> DeletableRedirectors;

#if defined(DURIN_ASSETCORE_INTERNAL)
		friend class FAssetMutationCoordinator;
#endif
	};

	ASSETCORE_API auto PrepareRedirectorFixupTransaction(
		std::span<const FAssetPath> Redirectors,
		EAssetRedirectorFixupMode Mode,
		FAssetRedirectorFixupSummary& OutSummary,
		FAssetMutationTransaction& OutTransaction
	) -> FAssetResult;
} // namespace Durin::Asset
