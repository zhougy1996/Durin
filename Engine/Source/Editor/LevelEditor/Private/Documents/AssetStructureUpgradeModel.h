#pragma once

#include "AssetSystem.h"

namespace Durin
{
	class DLevel;

	// Identifies one user decision for a pending compatibility-affected level.
	enum class EAssetStructureUpgradeDecision
	{
		SaveAndOpen,
		DiscardIncompatibleDataSaveAndOpen,
		OpenWithoutSaving,
		Cancel
	};

	// Describes the observable result of resolving a pending upgrade decision.
	enum class EAssetStructureUpgradeResult
	{
		Rejected,
		Activated,
		Cancelled,
		SaveFailed,
		ActivationFailed
	};

	// Supplies editor-owned persistence and activation effects to the testable workflow model.
	struct FAssetStructureUpgradeOperations
	{
		std::function<bool(DLevel*, bool)> Save;
		std::function<bool(DLevel*)> Activate;
		std::function<void(const FAssetPath&)> Unload;
		std::function<void(bool)> CompleteDeferredOpen;
	};

	// Owns a pending compatibility-affected level until the user resolves its opening workflow.
	class FAssetStructureUpgradeModel
	{
	public:
		auto Begin(DLevel* Level, Asset::FAssetLoadReport Report, bool bCompletesDeferredOpen) -> bool;
		auto Resolve(
			EAssetStructureUpgradeDecision Decision,
			const FAssetStructureUpgradeOperations& Operations) -> EAssetStructureUpgradeResult;

		auto IsPending() const -> bool { return PendingLevel != nullptr; }
		auto GetPendingLevel() const -> DLevel* { return PendingLevel; }
		auto GetReport() const -> const Asset::FAssetLoadReport& { return LoadReport; }
		auto CanSaveWithoutDataLoss() const -> bool { return IsPending() && !LoadReport.HasRiskItems(); }
		auto CanDiscardIncompatibleData() const -> bool { return IsPending() && LoadReport.HasRiskItems(); }

	private:
		auto Reset() -> void;
		auto Complete(bool bSucceeded, const FAssetStructureUpgradeOperations& Operations) -> void;

		DLevel* PendingLevel = nullptr;
		Asset::FAssetLoadReport LoadReport;
		bool bCompletesDeferredOpen = false;
	};
} // namespace Durin
