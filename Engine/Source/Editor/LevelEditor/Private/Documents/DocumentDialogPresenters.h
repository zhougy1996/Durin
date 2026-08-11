#pragma once

namespace Durin::Editor::Level
{
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
