#pragma once

#include "Asset/AssetWriteResult.h"

#include "EngineAPI.h"
#include "Asset/MutationExtensions.h"
#include "Asset/MutationTypes.h"
#include "Asset/References.h"

namespace Durin
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

	// Prepares, revalidates, and commits synchronously on the owner thread.
	ENGINE_API auto FixUpRedirectors(
		std::span<const FPackagePath> Redirectors,
		EAssetRedirectorFixupMode Mode
	) -> FAssetMutationResultDetails;
} // namespace Durin
