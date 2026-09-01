#pragma once

#include "AssetCompatibilityAudit.h"
#include "AssetMaintenance/CanonicalResave.h"

namespace Durin::Editor::MainFrame
{
	class FAssetCompatibilityWindow
	{
	public:
		using FRevealAsset = std::function<void(const FPackagePath&)>;

		auto Draw(bool& bOpen, const FRevealAsset& RevealAsset) -> void;
		auto ProjectChanged() -> void
		{
			Audit.ProjectChanged();
			ReconciledCatalogRevision.reset();
			SelectedPath = {};
			SelectedPackages.clear();
			PreviewMaintenancePlan.reset();
			PreviewMaintenanceSelection.reset();
			PreviewMaintenanceScope.clear();
			PendingMaintenancePlan.reset();
			MaintenanceCompleted = 0;
			MaintenanceMessage.clear();
			WindowMessage.clear();
			bCancelMaintenance = false;
		}

	private:
		auto DrawDetails(const FRevealAsset& RevealAsset, bool bCanPlanMaintenance) -> void;
		auto DrawMaintenancePlan() -> void;
		auto DrawApplyConfirmation() -> void;
		auto CopySelectedDiagnostics() const -> void;
		auto CopyFilteredReport() const -> void;
		auto RefreshCatalog() -> void;
		auto RunAudit() -> void;
		auto PreviewCanonicalResave(
			FAssetCanonicalResaveSelection Selection,
			std::string Scope) -> void;
		auto BeginCanonicalResave() -> void;
		auto TickCanonicalResave() -> void;

		Editor::FAssetCompatibilityAuditModel Audit;
		std::optional<uint64> ReconciledCatalogRevision;
		std::optional<uint64> SummaryPresentationRevision;
		std::optional<uint64> FilteredPresentationRevision;
		Editor::FAssetCompatibilityAuditCounts PresentationCounts;
		std::vector<size_t> FilteredRecordIndices;
		size_t CanonicalDebt = 0;
		size_t FilteredCanonicalDebt = 0;
		FPackagePath SelectedPath;
		std::unordered_set<FPackagePath> SelectedPackages;
		Editor::EAssetCompatibilityAuditFilter Filter = Editor::EAssetCompatibilityAuditFilter::All;
		Editor::EAssetCompatibilityAuditFilter CachedFilter = Editor::EAssetCompatibilityAuditFilter::All;
		std::array<char, 256> SearchText{};
		std::string CachedSearchText;
		std::string WindowMessage;
		std::string MaintenanceMessage;
		bool bCanonicalDebtOnly = false;
		bool bCachedCanonicalDebtOnly = false;
		std::optional<FAssetCanonicalResavePlan> PreviewMaintenancePlan;
		std::optional<FAssetCanonicalResaveSelection> PreviewMaintenanceSelection;
		std::string PreviewMaintenanceScope;
		std::optional<FAssetCanonicalResavePlan> PendingMaintenancePlan;
		size_t MaintenanceCompleted = 0;
		bool bCancelMaintenance = false;
	};
}
