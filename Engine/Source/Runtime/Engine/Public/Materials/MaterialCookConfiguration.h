#pragma once

#include "CoreMinimal.h"
#include "RHIFeatureLevel.h"

namespace Durin
{
	enum class EMaterialQualityLevel : uint8;

	// A detached material variant. No graph, texture asset, or platform policy dependency.
	struct FMaterialCookConfiguration
	{
		EMaterialQualityLevel Quality{};
		ERHIFeatureLevel FeatureLevel = ERHIFeatureLevel::SM5;
		auto operator<=>(const FMaterialCookConfiguration&) const = default;
	};
}
