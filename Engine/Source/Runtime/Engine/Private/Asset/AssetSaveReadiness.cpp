#include "Asset/AssetSaveReadiness.h"

#include "Modules/ModularFeature.h"

namespace Durin
{
	auto ValidateAssetSaveReadiness(const DObject* Asset)
		-> std::expected<void, UnifiedError::FError>
	{
		if (!Asset)
			return std::unexpected(UnifiedError::FError{{"AssetSaveReadiness", "InvalidData"}, "Asset save-readiness requires an exact loaded asset."});

		std::optional<std::expected<void, UnifiedError::FError>> Handled;
		const char* Error = nullptr;
		ForEachModularFeature<IAssetSaveReadinessFeature>([&](IAssetSaveReadinessFeature& Feature) {
			try
			{
				auto Outcome = Feature.Validate(*Asset);
				if (Error || !Outcome.bHandled) return;
				if (Handled) Error = "Asset save-readiness ownership is ambiguous.";
				else Handled = std::move(Outcome.Result);
			}
			catch (...)
			{
				if (!Error) Error = "An asset save-readiness provider failed.";
			}
		});
		if (Error)
			return std::unexpected(UnifiedError::FError{{"AssetSaveReadiness", "StaleData"}, Error});
		return Handled.value_or(std::expected<void, UnifiedError::FError>{});
	}
}
