#pragma once

#include "CoreMinimal.h"
#include "Engine/SceneLoadProgress.h"

namespace Durin::Editor::Level
{
	// Observes a load snapshot without pumping the host or mutating the active document.
	class FLevelLoadingDialogPresenter
	{
	public:
		// Returns a cancellation request; a missing snapshot closes any previous dialog.
		auto Draw(std::string_view LevelPath, const std::optional<FSceneLoadProgress>& Progress) -> bool;
	};

	// Identifies the user decision returned by the unsaved-level modal.
	enum class EUnsavedLevelDialogDecision : uint8
	{
		None,
		Save,
		Discard,
		Cancel
	};

	// Presents the unsaved-level modal and reports a decision without owning document transitions.
	class FUnsavedLevelDialogPresenter
	{
	public:
		using FResolve = std::function<bool(EUnsavedLevelDialogDecision)>;

		auto Draw(bool bRequestOpen, const FResolve& Resolve)
			-> std::optional<EUnsavedLevelDialogDecision>;
	};
} // namespace Durin::Editor::Level
