#pragma once

#include "EngineAPI.h"
#include "Asset/AssetDefinitions.h"
#include "Modules/ModularFeature.h"

namespace Durin
{
	class DObject;

	// Carries one provider's decision for an asset family it may or may not own.
	struct FAssetSaveReadinessFeatureResult
	{
		bool bHandled = false;
		FAssetResult Result;
	};

	// Lets asset-family modules validate family-specific transient state before a save.
	class IAssetSaveReadinessFeature : public IModularFeature
	{
	public:
		static constexpr std::string_view FeatureName = "Engine.AssetSaveReadiness";
		static constexpr uint32 FeatureVersion = 1;
		virtual auto Validate(const DObject& Asset) const
			-> FAssetSaveReadinessFeatureResult = 0;
	};

	// Rejects missing assets, ambiguous providers, and handled assets that are not ready.
	ENGINE_API auto ValidateAssetSaveReadiness(const DObject* Asset)
		-> FAssetResult;
}
