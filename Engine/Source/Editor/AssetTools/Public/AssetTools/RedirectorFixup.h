#pragma once

#include "Asset/AssetWriteResult.h"

#include "AssetToolsAPI.h"
#include "Asset/MutationExtensions.h"
#include "AssetTools/MutationTypes.h"
#include "Asset/References.h"

namespace Durin
{
	enum class EAssetRedirectorFixupMode : uint8
	{
		RewriteOnly,
		RewriteAndDelete
	};

	// Prepares, revalidates, and commits synchronously on the owner thread.
	ASSETTOOLS_API auto FixUpRedirectors(
		std::span<const FPackagePath> Redirectors,
		EAssetRedirectorFixupMode Mode
	) -> FAssetMutationResultDetails;
} // namespace Durin
