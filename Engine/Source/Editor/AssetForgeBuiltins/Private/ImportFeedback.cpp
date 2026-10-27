#include "AssetForge/Builtins/ImportFeedback.h"

namespace Durin::AssetForge::Builtins
{
	auto MakeAssetImportFeedback(const FSceneImportProgress& Progress, std::string_view SourceFile) -> FSlowTaskSnapshot
	{
		constexpr std::string_view PhaseNames[] = {"Reading source", "Configure import", "Building assets",
			"Preparing assets", "Compiling materials", "Saving assets", "Finishing", "Completed"};
		const auto Label = PhaseNames[static_cast<size_t>(Progress.Phase)];
		// Counts describe the current phase, not an estimated fraction of the whole import.
		return {
			.Title = "Importing " + std::filesystem::path(SourceFile).filename().generic_string(),
			.Message = Progress.Total ? std::format("{} ({} / {})", Label, Progress.Completed, Progress.Total) : std::string(Label),
			.Details = std::format("{}\n{}", SourceFile, Progress.Activity),
			.Fraction = Progress.Total ? std::optional(std::clamp(static_cast<double>(Progress.Completed) / Progress.Total, 0.0, 1.0)) : std::nullopt,
			.bCanCancel = true,
			.bCancelRequested = Progress.bCancellationRequested};
	}
}
