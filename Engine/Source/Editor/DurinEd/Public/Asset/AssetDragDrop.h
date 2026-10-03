#pragma once

#include "CoreMinimal.h"

#include "DurinEdAPI.h"

namespace Durin::Editor
{
	inline constexpr const char* AssetDragDropPayloadType = "DURIN_CONTENT_ASSET";

	// Carries fixed-size asset identity between editor asset views and assignment targets.
	struct FAssetDragDropPayload
	{
		std::array<char, 256> AssetPath{};
		std::array<char, 192> AssetClassName{};
	};
}
