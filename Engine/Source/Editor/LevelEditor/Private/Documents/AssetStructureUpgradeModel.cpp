#include "Documents/AssetStructureUpgradeModel.h"

namespace Durin
{
	auto FAssetStructureUpgradeModel::Begin(
		DLevel* Level,
		Asset::FAssetLoadReport Report,
		bool bInCompletesDeferredOpen) -> bool
	{
		if (IsPending() || Level == nullptr || !Report.HasCompatibilityIssues()) return false;
		PendingLevel = Level;
		LoadReport = std::move(Report);
		bCompletesDeferredOpen = bInCompletesDeferredOpen;
		return true;
	}

	auto FAssetStructureUpgradeModel::Resolve(
		EAssetStructureUpgradeDecision Decision,
		const FAssetStructureUpgradeOperations& Operations) -> EAssetStructureUpgradeResult
	{
		if (!IsPending()) return EAssetStructureUpgradeResult::Rejected;

		const bool bSave = Decision == EAssetStructureUpgradeDecision::SaveAndOpen
			|| Decision == EAssetStructureUpgradeDecision::DiscardIncompatibleDataSaveAndOpen;
		const bool bAllowDataLoss =
			Decision == EAssetStructureUpgradeDecision::DiscardIncompatibleDataSaveAndOpen;
		if ((Decision == EAssetStructureUpgradeDecision::SaveAndOpen && !CanSaveWithoutDataLoss())
			|| (bAllowDataLoss && !CanDiscardIncompatibleData()))
			return EAssetStructureUpgradeResult::Rejected;

		if (Decision == EAssetStructureUpgradeDecision::Cancel)
		{
			const FAssetPath Path = LoadReport.PackagePath;
			Reset();
			if (Operations.Unload) Operations.Unload(Path);
			Complete(false, Operations);
			return EAssetStructureUpgradeResult::Cancelled;
		}

		if (bSave && (!Operations.Save || !Operations.Save(PendingLevel, bAllowDataLoss)))
			return EAssetStructureUpgradeResult::SaveFailed;

		if (!Operations.Activate || !Operations.Activate(PendingLevel))
		{
			const FAssetPath Path = LoadReport.PackagePath;
			Reset();
			if (Operations.Unload) Operations.Unload(Path);
			Complete(false, Operations);
			return EAssetStructureUpgradeResult::ActivationFailed;
		}

		Reset();
		Complete(true, Operations);
		return EAssetStructureUpgradeResult::Activated;
	}

	auto FAssetStructureUpgradeModel::Reset() -> void
	{
		PendingLevel = nullptr;
		LoadReport = {};
	}

	auto FAssetStructureUpgradeModel::Complete(
		bool bSucceeded,
		const FAssetStructureUpgradeOperations& Operations) -> void
	{
		if (!std::exchange(bCompletesDeferredOpen, false) || !Operations.CompleteDeferredOpen) return;
		Operations.CompleteDeferredOpen(bSucceeded);
	}
} // namespace Durin
