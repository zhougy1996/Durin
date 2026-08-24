#include "AssetAuthoringReadiness.h"

#include "Modules/ModularFeature.h"

namespace Durin
{
	auto ValidateAssetAuthoringReadiness(const DObject* Asset)
		-> Asset::FAssetResult
	{
		if (!Asset)
			return {Asset::EAssetError::InvalidObjectGraph,
				"Loaded package has no main asset."};

		const auto Invoked = FModularFeatureRegistry::Get().InvokeAll<
			IAssetAuthoringReadinessFeature>(
			[&](IAssetAuthoringReadinessFeature& Feature) {
				return Feature.Validate(*Asset);
			});
		std::optional<Asset::FAssetResult> Handled;
		for (const auto& Invocation : Invoked.Invocations)
		{
			if (Invocation.Status != EFeatureInvokeStatus::Invoked || !Invocation.Value)
				return {Asset::EAssetError::StaleData,
					"An asset authoring-readiness provider failed."};
			if (!Invocation.Value->bHandled) continue;
			if (Handled)
				return {Asset::EAssetError::StaleData,
					"Asset authoring-readiness ownership is ambiguous."};
			Handled = Invocation.Value->Result;
		}
		return Handled.value_or(Asset::FAssetResult{});
	}
}
