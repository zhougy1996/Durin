#pragma once

#include "Asset/AssetCompatibilityAudit.h"
#include "AssetTools.h"

namespace Durin::Editor::MainFrame
{
	class FAssetCompatibilityWindow
	{
	public:
		using FRevealAsset = std::function<void(const FAssetPath&)>;

		auto Draw(bool& bOpen, const FRevealAsset& RevealAsset) -> void;
		auto ProjectChanged() -> void
		{
			Audit.ProjectChanged();
			ReconciledCatalogRevision.reset();
			SelectedPath = {};
			PendingMaintenancePlan.reset();
			MaintenanceCompleted = 0;
		}

	private:
		auto DrawDetails(const FRevealAsset& RevealAsset) -> void;
		auto CopySelectedDiagnostics() const -> void;
		auto ApplyCanonicalResave(std::vector<FAssetPath> Packages) -> void;
		auto TickCanonicalResave() -> void;

		Editor::FAssetCompatibilityAuditModel Audit;
		std::optional<uint64> ReconciledCatalogRevision;
		std::optional<uint64> SummaryPresentationRevision;
		std::optional<uint64> FilteredPresentationRevision;
		Editor::FAssetCompatibilityAuditCounts PresentationCounts;
		std::vector<size_t> FilteredRecordIndices;
		size_t CanonicalDebt = 0;
		FAssetPath SelectedPath;
		Editor::EAssetCompatibilityAuditFilter Filter = Editor::EAssetCompatibilityAuditFilter::All;
		Editor::EAssetCompatibilityAuditFilter CachedFilter = Editor::EAssetCompatibilityAuditFilter::All;
		std::string MaintenanceMessage;
		bool bCanonicalDebtOnly = false;
		bool bCachedCanonicalDebtOnly = false;
		std::optional<Asset::FAssetCanonicalResavePlan> PendingMaintenancePlan;
		size_t MaintenanceCompleted = 0;
		bool bCancelMaintenance = false;
	};
}
