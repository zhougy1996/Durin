#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/Catalog.h"

#include "DurinEdAPI.h"

namespace Durin
{
	inline constexpr const char* AssetDragDropPayloadType = "DURIN_CONTENT_ASSET";

	// Prefer the registry snapshot and retain drag metadata for uncatalogued assets.
	DURINED_API auto GetDraggedAssetData(std::string_view Path, std::string_view ClassName) -> std::optional<FAssetData>;

	// Carries fixed-size asset identity between editor asset views and assignment targets.
	struct FAssetDragDropPayload
	{
		std::array<char, 256> AssetPath{};
		std::array<char, 192> AssetClassName{};
	};
}
