#pragma once

#include "CoreMinimal.h"

#include "Asset/AssetWriteResult.h"

#include "AssetToolsAPI.h"
#include "Misc/FeedbackContext.h"
#include "AssetRegistry/Catalog.h"
#include "Asset/PackageInspection.h"

namespace Durin
{
	class DClass;
}

namespace Durin
{
	// Additional authored files owned by a package, discovered without loading objects.
	struct FAssetDeleteContribution
	{
		std::vector<std::filesystem::path> Files;
	};

	using FAssetDeleteContributor = std::function<bool(
		const FPackageData&,
		const FAssetPackageInspection&,
		FAssetDeleteContribution&, std::optional<UnifiedError::FError>*
	)>;
	using FAssetDeleteContributorHandle = uint64;
	// Owner-thread epoch; destructive hosts fence policy callbacks between physical effects.
	ASSETTOOLS_API auto GetAssetDeleteContributorRevision() -> uint64;
	ASSETTOOLS_API auto RegisterAssetDeleteContributor(
		DClass* Class,
		FAssetDeleteContributor Contributor) -> FAssetDeleteContributorHandle;

	ASSETTOOLS_API auto UnregisterAssetDeleteContributor(
		FAssetDeleteContributorHandle Handle
	) -> void;

	// Distinguishes ordinary files from uniquely or ambiguously claimed companions.
	enum class EAssetCompanionOwnershipState : uint8
	{
		Unclaimed,
		Owned,
		Ambiguous,
	};

	// Owned package identities claiming one normalized physical path.
	struct FAssetCompanionOwnership
	{
		EAssetCompanionOwnershipState State =
			EAssetCompanionOwnershipState::Unclaimed;
		std::vector<FPackagePath> Owners;
	};

	// Failure is distinct from Unclaimed; ownership output is valid only on success.
	ASSETTOOLS_API auto QueryAssetCompanionOwnership(
		const std::filesystem::path& PhysicalPath,
		FAssetCompanionOwnership& OutOwnership,
		std::optional<UnifiedError::FError>* OutError = nullptr) -> bool;

} // namespace Durin
