#pragma once

#include "CoreMinimal.h"

#include "Materials/MaterialTypes.h"
#include "RHIFeatureLevel.h"
#include "Materials/MaterialCookConfiguration.h"

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

	// Empty selects all supported variants. Canonical order is independent of caller order.
	inline auto ResolveMaterialCookConfigurations(std::span<const FMaterialCookConfiguration> Requested)
		-> std::optional<std::vector<FMaterialCookConfiguration>>
	{
		std::vector<FMaterialCookConfiguration> Result;
		if (Requested.empty())
			for (auto Quality : SupportedMaterialQualityLevels)
				for (auto Feature : SupportedMaterialFeatureLevels) Result.push_back({Quality, Feature});
		else Result.assign(Requested.begin(), Requested.end());
		if (Result.size() > MaterialSupportedConfigurationCount) return std::nullopt;
		for (const auto& Value : Result)
			if (!IsSupportedMaterialQualityLevel(Value.Quality) || !IsSupportedMaterialFeatureLevel(Value.FeatureLevel)) return std::nullopt;
		std::ranges::sort(Result);
		if (std::adjacent_find(Result.begin(), Result.end()) != Result.end()) return std::nullopt;
		return Result;
	}
}
