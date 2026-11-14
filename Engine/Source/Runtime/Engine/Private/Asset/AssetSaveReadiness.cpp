#include "Asset/AssetSaveReadiness.h"

#include "Modules/ModularFeature.h"

namespace Durin
{
	auto ValidateAssetSaveReadiness(const DObject* Asset)
		-> std::expected<void, UnifiedError::FError>
	{
		if (!Asset)
			return std::unexpected(UnifiedError::FError{{"AssetSaveReadiness", "InvalidData"}, "Asset save-readiness requires an exact loaded asset."});

		const auto Invoked = FModularFeatureRegistry::Get().InvokeAll<
			IAssetSaveReadinessFeature>(
			[&](IAssetSaveReadinessFeature& Feature) {
				return Feature.Validate(*Asset);
			});
		std::optional<std::expected<void, UnifiedError::FError>> Handled;
		for (const auto& Invocation : Invoked.Invocations)
		{
			if (Invocation.Status != EFeatureInvokeStatus::Invoked || !Invocation.Value)
				return std::unexpected(UnifiedError::FError{{"AssetSaveReadiness", "StaleData"}, "An asset save-readiness provider failed."});
			if (!Invocation.Value->bHandled) continue;
			if (Handled)
				return std::unexpected(UnifiedError::FError{{"AssetSaveReadiness", "StaleData"}, "Asset save-readiness ownership is ambiguous."});
			Handled = Invocation.Value->Result;
		}
		return Handled.value_or(std::expected<void, UnifiedError::FError>{});
	}
}
