#pragma once

#include "EngineAPI.h"
#include "Asset/Result.h"
#include "Modules/ModularFeature.h"

namespace Durin
{
	class DObject;

	// Carries one provider's decision for an asset family it may or may not own.
	struct FAssetAuthoringReadinessFeatureResult
	{
		bool bHandled = false;
		Asset::FAssetResult Result;
	};

	// Lets authoring modules validate family-specific transient state before a save.
	class IAssetAuthoringReadinessFeature : public IModularFeature
	{
	public:
		static constexpr std::string_view FeatureName = "Engine.AssetAuthoringReadiness";
		static constexpr uint32 FeatureVersion = 1;
		virtual auto Validate(const DObject& Asset) const
			-> FAssetAuthoringReadinessFeatureResult = 0;
	};

	// Rejects missing assets, ambiguous providers, and handled assets that are not ready.
	ENGINE_API auto ValidateAssetAuthoringReadiness(const DObject* Asset)
		-> Asset::FAssetResult;
}
