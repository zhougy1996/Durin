#pragma once

#include "Documents/AssetStructureUpgradeModel.h"

namespace Durin
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

	// Presents compatibility details while the upgrade model owns the pending package and decision state.
	class FAssetStructureUpgradeDialogPresenter
	{
	public:
		using FResolve = std::function<EAssetStructureUpgradeResult(EAssetStructureUpgradeDecision)>;

		auto Draw(
			const FAssetStructureUpgradeModel& Model,
			bool bRequestOpen,
			bool& bDataLossConfirmed,
			const FResolve& Resolve
		) -> std::optional<EAssetStructureUpgradeDecision>;
	};
} // namespace Durin
