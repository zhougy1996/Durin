#include "Asset/AssetSaveReadiness.h"

#include "Modules/ModularFeature.h"

namespace Durin
{
	auto ValidateAssetSaveReadiness(const DObject* Asset)
		-> FAssetResult
	{
		if (!Asset)
			return {EAssetError::InvalidObjectGraph,
				"Asset save-readiness requires an exact loaded asset."};

		const auto Invoked = FModularFeatureRegistry::Get().InvokeAll<
			IAssetSaveReadinessFeature>(
			[&](IAssetSaveReadinessFeature& Feature) {
				return Feature.Validate(*Asset);
			});
		std::optional<FAssetResult> Handled;
		for (const auto& Invocation : Invoked.Invocations)
		{
			if (Invocation.Status != EFeatureInvokeStatus::Invoked || !Invocation.Value)
				return {EAssetError::StaleData,
					"An asset save-readiness provider failed."};
			if (!Invocation.Value->bHandled) continue;
			if (Handled)
				return {EAssetError::StaleData,
					"Asset save-readiness ownership is ambiguous."};
			Handled = Invocation.Value->Result;
		}
		return Handled.value_or(FAssetResult{});
	}
}
