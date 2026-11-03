#pragma once

#include "CoreMinimal.h"

#include "AssetCompatibilityAudit.h"
#include "AssetMaintenance/CanonicalResave.h"

namespace Durin::Editor::MainFrame
{
	// On-demand project upgrade flow; host updates advance work independently of visibility.
	class FAssetCompatibilityWindow
	{
	public:
		using FRevealAsset = std::function<void(const FPackagePath&)>;

		auto Draw(bool& bOpen, const FRevealAsset& RevealAsset) -> void;
		// Updates jobs even while hidden; changing projects retires the previous work.
		auto Tick(std::string_view ProjectFile, bool bAllowAssetWrites) -> void;
		auto Defer() -> void;
		auto ProjectChanged() -> void
		{
			Audit.ProjectChanged();
			ReconciledCatalogRevision.reset();
			SelectedPath = {};
			PreviewMaintenancePlan.reset();
			PreviewMaintenanceSelection.reset();
			PreviewMaintenanceScope.clear();
			PendingMaintenancePlan.reset();
			MaintenanceCompleted = 0;
			MaintenanceMessage.clear();
			WindowMessage.clear();
			bCancelMaintenance = false;
			SummaryPresentationRevision.reset();
			FilteredPresentationRevision.reset();
		}

	private:
		friend struct FAssetCompatibilityWindowTestAccess;
		auto DrawDetails(const FRevealAsset& RevealAsset) -> void;
		auto DrawMaintenancePlan() -> void;
		auto DrawApplyConfirmation() -> void;
		auto CopySelectedDiagnostics() const -> void;
		auto CopyCheckReport() const -> void;
		auto RefreshCatalog() -> bool;
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
		FPackagePath SelectedPath;
		std::string ActiveProjectFile;
		std::string WindowMessage;
		std::string MaintenanceMessage;
		bool bShowUnchangedAssets = false;
		bool bCachedShowUnchangedAssets = false;
		bool bAllowAssetWrites = false;
		std::optional<FAssetCanonicalResavePlan> PreviewMaintenancePlan;
		std::optional<FAssetCanonicalResaveSelection> PreviewMaintenanceSelection;
		std::string PreviewMaintenanceScope;
		std::optional<FAssetCanonicalResavePlan> PendingMaintenancePlan;
		size_t MaintenanceCompleted = 0;
		bool bCancelMaintenance = false;
	};
}
