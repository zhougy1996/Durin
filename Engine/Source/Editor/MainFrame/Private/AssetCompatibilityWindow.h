#pragma once

#include "Asset/AssetCompatibilityAudit.h"
#include "AssetCanonicalResave.h"

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
		FAssetPath SelectedPath;
		Editor::EAssetCompatibilityAuditFilter Filter = Editor::EAssetCompatibilityAuditFilter::All;
		std::string MaintenanceMessage;
		bool bCanonicalDebtOnly = false;
		std::optional<Asset::FAssetCanonicalResavePlan> PendingMaintenancePlan;
		size_t MaintenanceCompleted = 0;
		bool bCancelMaintenance = false;
	};
}
