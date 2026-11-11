#include "AssetCompatibilityWindow.h"
#include "Feedback/SlowTaskWidget.h"
#include "AssetRegistry/Scan.h"

#include "MonaImGui.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"

namespace Durin::MainFrame
{
	namespace
	{
		auto InspectionName(EAssetCompatibilityInspection Value) -> const char*
		{
			switch (Value)
			{
			case EAssetCompatibilityInspection::NotChecked: return "Not checked";
			case EAssetCompatibilityInspection::Ready: return "Ready";
			case EAssetCompatibilityInspection::Failed: return "Failed";
			}
			return "Unknown";
		}

		auto CompatibilityName(EAssetPackageCompatibility Value) -> const char*
		{
			switch (Value)
			{
			case EAssetPackageCompatibility::Compatible: return "Compatible";
			case EAssetPackageCompatibility::Incompatible: return "Incompatible";
			case EAssetPackageCompatibility::Unsupported: return "Unsupported";
			}
			return "Unknown";
		}

		auto StateName(EAssetCompatibilityAuditState Value) -> const char*
		{
			switch (Value)
			{
			case EAssetCompatibilityAuditState::Idle: return "Ready to run";
			case EAssetCompatibilityAuditState::Running: return "Running";
			case EAssetCompatibilityAuditState::Completed: return "Completed";
			case EAssetCompatibilityAuditState::Cancelled: return "Cancelled";
			case EAssetCompatibilityAuditState::Failed: return "Failed";
			}
			return "Unknown";
		}

		auto MountOwnerName(EMountOwner Value) -> const char*
		{
			switch (Value)
			{
			case EMountOwner::Engine: return "Engine";
			case EMountOwner::ActiveProject: return "Active project";
			case EMountOwner::Extension: return "Extension";
			case EMountOwner::ExternalSources: return "External sources";
			case EMountOwner::Test: return "Test";
			}
			return "Unknown";
		}

		auto CanonicalResaveStatusName(EAssetCanonicalResavePackageStatus Value) -> const char*
		{
			switch (Value)
			{
			case EAssetCanonicalResavePackageStatus::Skipped: return "Skipped";
			case EAssetCanonicalResavePackageStatus::Ready: return "Ready";
			case EAssetCanonicalResavePackageStatus::Resaved: return "Resaved";
			case EAssetCanonicalResavePackageStatus::Blocked: return "Blocked";
			case EAssetCanonicalResavePackageStatus::Failed: return "Failed";
			case EAssetCanonicalResavePackageStatus::Cancelled: return "Cancelled";
			case EAssetCanonicalResavePackageStatus::Stale: return "Stale";
			}
			return "Unknown";
		}

		auto CountCanonicalResaveStatus(
			const FAssetCanonicalResavePlan& Plan,
			EAssetCanonicalResavePackageStatus Status) -> size_t
		{
			return static_cast<size_t>(std::ranges::count(
				Plan.Packages, Status, &FAssetCanonicalResavePackagePlan::Status));
		}

		auto IsCanonicalResaveRecommended(
			const FAssetPackageCompatibilityRecord& Record) -> bool
		{
			return !Record.CanonicalizationEvidence.empty()
				|| !Record.DeprecatedRouteEvidence.empty();
		}
	}

	auto FAssetCompatibilityWindow::Tick(std::string_view ProjectFile, bool bInAllowAssetWrites) -> void
	{
		if (ActiveProjectFile != ProjectFile)
		{
			ProjectChanged();
			ActiveProjectFile = ProjectFile;
		}
		bAllowAssetWrites = bInAllowAssetWrites;
		if (Audit.GetState() == EAssetCompatibilityAuditState::Idle && !PendingMaintenancePlan) return;
		Audit.Tick();
		const uint64 Revision = GetAssetCatalogRevision();
		if (!ReconciledCatalogRevision || *ReconciledCatalogRevision != Revision)
		{
			const auto Snapshot = CaptureAssetCatalogSnapshot();
			Audit.ReconcileAssetCatalog(Snapshot.Assets);
			ReconciledCatalogRevision = Snapshot.Revision;
		}
		if (PendingMaintenancePlan && !bAllowAssetWrites) bCancelMaintenance = true;
		TickCanonicalResave();
	}

	auto FAssetCompatibilityWindow::Defer() -> void
	{
		Audit.Cancel();
		if (PendingMaintenancePlan) bCancelMaintenance = true;
		PreviewMaintenancePlan.reset();
		PreviewMaintenanceSelection.reset();
		PreviewMaintenanceScope.clear();
	}

	auto FAssetCompatibilityWindow::Draw(bool& bOpen, const FRevealAsset& RevealAsset) -> void
	{
		if (!bOpen) return;
		ImGui::SetNextWindowSize(ImVec2(MonaImGui::ScaleUI(1000.0f), MonaImGui::ScaleUI(680.0f)), ImGuiCond_FirstUseEver);
		if (!ImGui::Begin("Project Asset Upgrade###Durin.AssetCompatibility", &bOpen))
		{
			if (!bOpen) Defer();
			ImGui::End();
			return;
		}
		const auto State = Audit.GetState();
		const bool bChecking = State == EAssetCompatibilityAuditState::Running;
		const bool bApplying = PendingMaintenancePlan.has_value();
		ImGui::TextWrapped("Check this project's assets, review recommended upgrades, then confirm which files to rewrite. "
			"Opening a project or this window does not scan or rewrite assets.");
		ImGui::BeginDisabled(bChecking || bApplying || PreviewMaintenancePlan.has_value());
		if (ImGui::Button(State == EAssetCompatibilityAuditState::Idle ? "Check Project Assets" : "Check Again"))
		{
			if (RefreshCatalog()) RunAudit();
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button(bApplying ? "Cancel and Close" : "Later")) { Defer(); bOpen = false; }
		if (bChecking)
		{
			ImGui::SameLine();
			if (ImGui::Button("Cancel Check")) Audit.Cancel();
			const auto Progress = Audit.GetProgress();
			const auto Overlay = std::format("Checking {} / {} packages", Progress.Completed, Progress.Total);
			ImGui::ProgressBar(Progress.Total ? float(Progress.Completed) / float(Progress.Total) : 0.0f,
				ImVec2(-1.0f, 0.0f), Overlay.c_str());
		}
		else ImGui::TextDisabled("Check: %s", StateName(State));
		if (!WindowMessage.empty()) ImGui::TextWrapped("%s", WindowMessage.c_str());
		const auto& Records = Audit.GetPresentationRecords();
		const uint64 Revision = Audit.GetPresentationRevision();
		if (!SummaryPresentationRevision || *SummaryPresentationRevision != Revision)
		{
			PresentationCounts = CountAssetCompatibilityAuditRecords(Records);
			CanonicalDebt = static_cast<size_t>(std::ranges::count_if(Records, IsCanonicalResaveRecommended));
			SummaryPresentationRevision = Revision;
		}
		ImGui::Text("Upgrade recommended: %zu   Incompatible: %llu   Unsupported: %llu   Failed: %llu   Stale: %llu",
			CanonicalDebt, static_cast<unsigned long long>(PresentationCounts.Incompatible),
			static_cast<unsigned long long>(PresentationCounts.Unsupported), static_cast<unsigned long long>(PresentationCounts.Failed),
			static_cast<unsigned long long>(PresentationCounts.Stale));
		ImGui::TextDisabled("Recommendations are optional rewrites. Incompatible or unreadable assets require separate attention.");
		if (bApplying)
		{
			const size_t Total = MaintenanceCompleted + CountCanonicalResaveStatus(*PendingMaintenancePlan, EAssetCanonicalResavePackageStatus::Ready);
			const FSlowTaskSnapshot Progress{.Title = "Upgrading project assets",
				.Message = std::format("Upgraded {} / {} packages", MaintenanceCompleted, Total),
				.Fraction = Total ? double(MaintenanceCompleted) / double(Total) : 1.0,
				.bCanCancel = true, .bCancelRequested = bCancelMaintenance};
			if (DrawSlowTaskProgress(Progress, true, false)) bCancelMaintenance = true;
		}
		if (!MaintenanceMessage.empty())
		{
			ImGui::TextWrapped("%s", MaintenanceMessage.c_str());
			if (ImGui::SmallButton("Copy Upgrade Result")) ImGui::SetClipboardText(MaintenanceMessage.c_str());
		}
		if (State == EAssetCompatibilityAuditState::Failed) ImGui::TextWrapped("Check failed: %s", Audit.GetFailure().c_str());
		if (State == EAssetCompatibilityAuditState::Cancelled)
			ImGui::TextDisabled("Check cancelled. Completed results are retained; unchecked assets cannot be upgraded.");
		if (State == EAssetCompatibilityAuditState::Completed && Records.empty()) ImGui::TextDisabled("No registered packages to check.");
		const bool bCanPlan = State == EAssetCompatibilityAuditState::Completed && !bApplying && !PreviewMaintenancePlan;
		ImGui::BeginDisabled(!bCanPlan || CanonicalDebt == 0 || !bAllowAssetWrites);
		if (ImGui::Button("Preview Recommended Upgrades"))
		{
			FAssetCanonicalResaveSelection Selection;
			for (const auto& Record : Records)
				if (IsCanonicalResaveRecommended(Record)) Selection.Packages.push_back(Record.PackagePath);
			PreviewCanonicalResave(std::move(Selection), "project upgrade recommendations");
		}
		ImGui::EndDisabled();
		if (!bAllowAssetWrites) ImGui::TextDisabled("Asset upgrades are unavailable while the editor cannot modify assets.");
		if (PreviewMaintenancePlan) DrawMaintenancePlan();
		ImGui::SeparatorText("Check Results");
		ImGui::Checkbox("Show unchanged assets", &bShowUnchangedAssets);
		if (!FilteredPresentationRevision || *FilteredPresentationRevision != Revision || bCachedShowUnchangedAssets != bShowUnchangedAssets)
		{
			FilteredRecordIndices.clear();
			for (size_t Index = 0; Index < Records.size(); ++Index)
			{
				const auto& Record = Records[Index];
				if (bShowUnchangedAssets || IsCanonicalResaveRecommended(Record) || !Record.Findings.empty()
					|| Record.Inspection != EAssetCompatibilityInspection::Ready
					|| Record.Compatibility != EAssetPackageCompatibility::Compatible || Record.Freshness != EAssetCompatibilityFreshness::Current)
					FilteredRecordIndices.push_back(Index);
			}
			bCachedShowUnchangedAssets = bShowUnchangedAssets;
			FilteredPresentationRevision = Revision;
		}
		if (ImGui::SmallButton("Copy Check Report")) CopyCheckReport();
		if (State == EAssetCompatibilityAuditState::Completed && FilteredRecordIndices.empty())
			ImGui::TextDisabled("No upgrade recommendations or compatibility issues were found.");
		if (ImGui::BeginTable("CompatibilityRows", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY,
			ImVec2(0.0f, -MonaImGui::ScaleUI(205.0f))))
		{
			for (const auto* Label : {"Package", "Inspection", "Compatibility", "Freshness", "Findings"}) ImGui::TableSetupColumn(Label);
			ImGui::TableHeadersRow();
			ImGuiListClipper Clipper;
			Clipper.Begin(static_cast<int>(std::min(FilteredRecordIndices.size(), static_cast<size_t>(std::numeric_limits<int>::max()))));
			while (Clipper.Step()) for (int Row = Clipper.DisplayStart; Row < Clipper.DisplayEnd; ++Row)
			{
				const auto& Record = Records[FilteredRecordIndices[static_cast<size_t>(Row)]];
				ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
				if (ImGui::Selectable(Record.PackagePath.ToString().c_str(), Record.PackagePath == SelectedPath)) SelectedPath = Record.PackagePath;
				ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(InspectionName(Record.Inspection));
				ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(CompatibilityName(Record.Compatibility));
				ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(Record.Freshness == EAssetCompatibilityFreshness::Current ? "Current" : "Stale");
				ImGui::TableSetColumnIndex(4); ImGui::Text("%zu + %zu upgrade", Record.Findings.size(), Record.CanonicalizationEvidence.size() + Record.DeprecatedRouteEvidence.size());
			}
			ImGui::EndTable();
		}
		DrawDetails(RevealAsset);
		DrawApplyConfirmation();
		if (!bOpen) Defer();
		ImGui::End();
	}

	auto FAssetCompatibilityWindow::DrawDetails(
		const FRevealAsset& RevealAsset) -> void
	{
		const auto* Record = SelectedPath.IsValid() ? Audit.FindRecord(SelectedPath) : nullptr;
		ImGui::SeparatorText("Details");
		if (!Record) { ImGui::TextDisabled("Select a package to inspect its findings."); return; }
		ImGui::TextUnformatted(Record->PackagePath.ToString().c_str());
		const auto Mount =
			FMountPaths::FindMountForVirtualPath(Record->PackagePath.GetView());
		if (Mount)
		{
			ImGui::SameLine();
			ImGui::TextDisabled("%s mount; content writes %s", MountOwnerName(Mount->Mount->Owner),
				Mount->Mount->bContentWritable ? "writable" : "read-only");
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("Copy Diagnostics")) CopySelectedDiagnostics();
		ImGui::SameLine();
		if (ImGui::SmallButton("Show in Content Browser") && RevealAsset) RevealAsset(Record->PackagePath);
		ImGui::BeginChild("CompatibilityFindingDetails", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
		if (Record->Findings.empty()) ImGui::TextDisabled("No compatibility findings.");
		for (const auto& Finding : Record->Findings)
		{
			ImGui::SeparatorText(AssetCompatibilityFindingCodeName(Finding.Code).data());
			if (!Finding.ObjectPath.empty()) ImGui::Text("Object: %s", Finding.ObjectPath.c_str());
			if (!Finding.FieldName.empty()) ImGui::Text("Field: %s::%s", Finding.DeclaringType.c_str(), Finding.FieldName.c_str());
			ImGui::TextWrapped("%s", Finding.Diagnostic.c_str());
		}
		for (const auto& Evidence : Record->CanonicalizationEvidence)
		{
			ImGui::SeparatorText("Canonical resave recommended");
			ImGui::Text("%s -> %s", Evidence.StoredIdentity.c_str(), Evidence.CurrentIdentity.c_str());
			ImGui::TextDisabled("%s", Evidence.LogicalPath.c_str());
		}
		for (const auto& Evidence : Record->DeprecatedRouteEvidence)
		{
			ImGui::SeparatorText("Canonical resave recommended");
			ImGui::Text("%s::%s uses deprecated route %s", Evidence.DeclaringType.c_str(),
				Evidence.StoredFieldName.c_str(), Evidence.DeprecatedPropertyName.c_str());
			if (!Evidence.ObjectPath.empty()) ImGui::TextDisabled("%s", Evidence.ObjectPath.c_str());
		}
		ImGui::EndChild();
	}

	auto FAssetCompatibilityWindow::DrawMaintenancePlan() -> void
	{
		if (!PreviewMaintenancePlan) return;
		const auto& Plan = *PreviewMaintenancePlan;
		const size_t ReadyCount = CountCanonicalResaveStatus(
			Plan, EAssetCanonicalResavePackageStatus::Ready);
		const size_t BlockedCount = CountCanonicalResaveStatus(
			Plan, EAssetCanonicalResavePackageStatus::Blocked);
		const size_t SkippedCount = CountCanonicalResaveStatus(
			Plan, EAssetCanonicalResavePackageStatus::Skipped);
		const bool bPlanStale = Plan.RegistryRevision != GetAssetCatalogRevision();

		ImGui::SeparatorText("Upgrade Preview");
		ImGui::Text("Scope: %s", PreviewMaintenanceScope.c_str());
		ImGui::SameLine();
		ImGui::Text("Ready %zu   Blocked %zu   Skipped %zu", ReadyCount, BlockedCount, SkippedCount);
		if (bPlanStale)
			ImGui::TextDisabled("The asset catalog changed after this preview. Re-plan before applying.");
		else if (BlockedCount != 0)
			ImGui::TextDisabled("Resolve every blocker and re-plan; blocked plans cannot be applied.");
		if (ImGui::BeginTable("CanonicalResavePlanRows", 3,
			ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY,
			ImVec2(0.0f, MonaImGui::ScaleUI(120.0f))))
		{
			ImGui::TableSetupColumn("Package", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, MonaImGui::ScaleUI(75.0f));
			ImGui::TableSetupColumn("Plan details", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableHeadersRow();
			ImGuiListClipper Clipper;
			Clipper.Begin(static_cast<int>(std::min(
				Plan.Packages.size(), static_cast<size_t>(std::numeric_limits<int>::max()))));
			while (Clipper.Step())
				for (int RowIndex = Clipper.DisplayStart; RowIndex < Clipper.DisplayEnd; ++RowIndex)
				{
					const auto& Package = Plan.Packages[static_cast<size_t>(RowIndex)];
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::TextUnformatted(Package.PackagePath.ToString().c_str());
					ImGui::TableSetColumnIndex(1);
					ImGui::TextUnformatted(CanonicalResaveStatusName(Package.Status));
					ImGui::TableSetColumnIndex(2);
					if (!Package.Diagnostics.empty()) ImGui::TextUnformatted(Package.Diagnostics.front().c_str());
					else ImGui::Text("%zu identity update(s)",
						Package.Evidence.size() + Package.DeprecatedRouteEvidence.size());
				}
			ImGui::EndTable();
		}

		ImGui::BeginDisabled(ReadyCount == 0 || BlockedCount != 0 || bPlanStale);
		if (ImGui::Button(std::format("Apply {} Package(s)...", ReadyCount).c_str()))
			ImGui::OpenPopup("Confirm Asset Upgrade");
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("Copy Plan Report"))
		{
			const std::string Report = SerializeAssetCanonicalResavePlanReport(Plan);
			ImGui::SetClipboardText(Report.c_str());
		}
		ImGui::SameLine();
		if (ImGui::Button("Re-plan") && PreviewMaintenanceSelection)
		{
			auto Selection = *PreviewMaintenanceSelection;
			const std::string Scope = PreviewMaintenanceScope;
			PreviewCanonicalResave(std::move(Selection), Scope);
		}
		ImGui::SameLine();
		if (ImGui::Button("Discard Plan"))
		{
			PreviewMaintenancePlan.reset();
			PreviewMaintenanceSelection.reset();
			PreviewMaintenanceScope.clear();
		}
	}

	auto FAssetCompatibilityWindow::DrawApplyConfirmation() -> void
	{
		if (!ImGui::BeginPopupModal("Confirm Asset Upgrade", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) return;
		const size_t ReadyCount = PreviewMaintenancePlan
			? CountCanonicalResaveStatus(*PreviewMaintenancePlan,
				EAssetCanonicalResavePackageStatus::Ready)
			: 0;
		ImGui::Text("Upgrade %zu package(s)?", ReadyCount);
		ImGui::TextWrapped("This writes authored package files. Check out the listed files in source control "
			"and review their diffs after completion.");
		const bool bCanApply = bAllowAssetWrites && PreviewMaintenancePlan && ReadyCount != 0
			&& PreviewMaintenancePlan->RegistryRevision == GetAssetCatalogRevision()
			&& CountCanonicalResaveStatus(*PreviewMaintenancePlan,
				EAssetCanonicalResavePackageStatus::Blocked) == 0;
		ImGui::BeginDisabled(!bCanApply);
		if (ImGui::Button("Apply"))
		{
			BeginCanonicalResave();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	auto FAssetCompatibilityWindow::CopyCheckReport() const -> void
	{
		const std::string Report = FormatAssetCompatibilityAuditReport(Audit.GetPresentationRecords());
		ImGui::SetClipboardText(Report.c_str());
	}

	auto FAssetCompatibilityWindow::RefreshCatalog() -> bool
	{
		const FAssetCatalogRefreshResult Result = RefreshAssetRegistry();
		if (!Result)
		{
			WindowMessage = Result.Errors.empty()
				? "Asset catalog refresh did not publish a complete result."
				: FormatAssetRegistryError(Result.Errors.front());
			return false;
		}
		const FAssetCatalogSnapshot Snapshot = CaptureAssetCatalogSnapshot();
		Audit.ReconcileAssetCatalog(Snapshot.Assets);
		ReconciledCatalogRevision = Snapshot.Revision;
		WindowMessage = std::format("Catalog refreshed: {} assets reparsed, {} reused.",
			Result.CatalogStats.Reparsed, Result.CatalogStats.Reused);
		return true;
	}

	auto FAssetCompatibilityWindow::RunAudit() -> void
	{
		PreviewMaintenancePlan.reset();
		PreviewMaintenanceSelection.reset();
		PreviewMaintenanceScope.clear();
		MaintenanceMessage.clear();
		WindowMessage.clear();
		SelectedPath = {};
		(void)Audit.RunCurrentProjectAudit();
	}

	auto FAssetCompatibilityWindow::PreviewCanonicalResave(
		FAssetCanonicalResaveSelection Selection,
		std::string Scope) -> void
	{
		if (PendingMaintenancePlan
			|| Audit.GetState() != EAssetCompatibilityAuditState::Completed) return;
		PreviewMaintenanceSelection = Selection;
		PreviewMaintenanceScope = std::move(Scope);
		PreviewMaintenancePlan = PlanAssetCanonicalResaves(
			Audit.GetPresentationRecords(), Selection);
		MaintenanceMessage.clear();
	}

	auto FAssetCompatibilityWindow::BeginCanonicalResave() -> void
	{
		if (!bAllowAssetWrites || !PreviewMaintenancePlan) return;
		if (PreviewMaintenancePlan->RegistryRevision != GetAssetCatalogRevision())
		{
			MaintenanceMessage = "The asset catalog changed after planning. Re-plan before applying.";
			return;
		}
		if (CountCanonicalResaveStatus(*PreviewMaintenancePlan,
			EAssetCanonicalResavePackageStatus::Blocked) != 0) return;
		PendingMaintenancePlan = std::move(PreviewMaintenancePlan);
		PreviewMaintenancePlan.reset();
		MaintenanceCompleted = 0;
		bCancelMaintenance = false;
		MaintenanceMessage.clear();
	}

	auto FAssetCompatibilityWindow::TickCanonicalResave() -> void
	{
		if (!PendingMaintenancePlan) return;
		if (bCancelMaintenance)
		{
			MaintenanceMessage = std::format(
				"Canonical resave cancelled after {} completed package(s).", MaintenanceCompleted);
			PendingMaintenancePlan.reset();
			bCancelMaintenance = false;
			if (MaintenanceCompleted != 0)
			{
				SelectedPath = {};
			}
			return;
		}
		auto Ready = std::ranges::find(
			PendingMaintenancePlan->Packages,
			EAssetCanonicalResavePackageStatus::Ready,
			&FAssetCanonicalResavePackagePlan::Status);
		if (Ready == PendingMaintenancePlan->Packages.end())
		{
			MaintenanceMessage = std::format(
				"Canonical resave completed: {} package(s) resaved.", MaintenanceCompleted);
			PendingMaintenancePlan.reset();
			bCancelMaintenance = false;
			SelectedPath = {};
			return;
		}
		FAssetCanonicalResavePlan Unit;
		if (PendingMaintenancePlan->RegistryRevision != GetAssetCatalogRevision())
		{
			MaintenanceMessage = "Asset catalog changed during upgrade. Check again before applying more packages.";
			PendingMaintenancePlan.reset();
			return;
		}
		Unit.RegistryRevision = PendingMaintenancePlan->RegistryRevision;
		Unit.Packages.push_back(*Ready);
		const FReflectionCompatibilityCatalog Catalog =
			FReflectionCompatibilityCatalog::Capture();
		const auto Applied = ApplyAssetCanonicalResaves(std::move(Unit), Catalog);
		*Ready = Applied.Plan.Packages.front();
		if (Applied.Status != EAssetCanonicalResaveApplyStatus::Succeeded)
		{
			MaintenanceMessage = std::format(
				"Canonical resave stopped after {} completed package(s).\n{}",
				MaintenanceCompleted, SerializeAssetCanonicalResaveApplyReport(Applied));
			PendingMaintenancePlan.reset();
			bCancelMaintenance = false;
			SelectedPath = {};
			return;
		}
		PendingMaintenancePlan->RegistryRevision = GetAssetCatalogRevision();
		++MaintenanceCompleted;
	}

	auto FAssetCompatibilityWindow::CopySelectedDiagnostics() const -> void
	{
		const auto* Record = Audit.FindRecord(SelectedPath);
		if (!Record) return;
		const std::string Text = FormatAssetCompatibilityAuditDiagnostics(*Record);
		ImGui::SetClipboardText(Text.c_str());
	}
}
