#pragma once

#include "CoreMinimal.h"

#include "Materials/MaterialTypes.h"
#include "RHIFeatureLevel.h"

namespace Durin
{
	inline constexpr std::array SupportedMaterialQualityLevels{
		EMaterialQualityLevel::Low,
		EMaterialQualityLevel::High};
	inline constexpr std::array SupportedMaterialFeatureLevels{
		ERHIFeatureLevel::ES3_1,
		ERHIFeatureLevel::SM5,
		ERHIFeatureLevel::SM6};
	inline constexpr uint32 MaterialSupportedConfigurationCount =
		static_cast<uint32>(SupportedMaterialQualityLevels.size()
			* SupportedMaterialFeatureLevels.size());

	constexpr auto IsSupportedMaterialQualityLevel(
		EMaterialQualityLevel Quality) -> bool
	{
		for (const auto Supported : SupportedMaterialQualityLevels)
			if (Supported == Quality) return true;
		return false;
	}

	constexpr auto IsSupportedMaterialFeatureLevel(
		ERHIFeatureLevel FeatureLevel) -> bool
	{
		for (const auto Supported : SupportedMaterialFeatureLevels)
			if (Supported == FeatureLevel) return true;
		return false;
	}
}
