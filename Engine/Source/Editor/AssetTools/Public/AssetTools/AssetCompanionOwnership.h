#pragma once

#include "CoreMinimal.h"

#include "AssetToolsAPI.h"
#include "DObject/AssetPath.h"
#include "Misc/UnifiedError.h"

namespace Durin
{
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
