#include "AssetCompatibilityWindow.h"

#include "MonaImGui.h"
#include "Misc/Paths.h"

namespace Durin::Editor::MainFrame
{
	namespace
	{
		auto InspectionName(Asset::EAssetCompatibilityInspection Value) -> const char*
		{
			switch (Value)
			{
			case Asset::EAssetCompatibilityInspection::NotChecked: return "Not checked";
			case Asset::EAssetCompatibilityInspection::Ready: return "Ready";
			case Asset::EAssetCompatibilityInspection::Failed: return "Failed";
			}
			return "Unknown";
		}

		auto CompatibilityName(Asset::EAssetPackageCompatibility Value) -> const char*
		{
			switch (Value)
			{
			case Asset::EAssetPackageCompatibility::Compatible: return "Compatible";
			case Asset::EAssetPackageCompatibility::Incompatible: return "Incompatible";
			case Asset::EAssetPackageCompatibility::Unsupported: return "Unsupported";
			}
			return "Unknown";
		}

		auto StateName(Editor::EAssetCompatibilityAuditState Value) -> const char*
		{
			switch (Value)
			{
			case Editor::EAssetCompatibilityAuditState::Idle: return "Ready to run";
			case Editor::EAssetCompatibilityAuditState::Running: return "Running";
			case Editor::EAssetCompatibilityAuditState::Completed: return "Completed";
			case Editor::EAssetCompatibilityAuditState::Cancelled: return "Cancelled";
			case Editor::EAssetCompatibilityAuditState::Failed: return "Failed";
			}
			return "Unknown";
		}

		auto MountOwnerName(PathUtilities::EMountOwner Value) -> const char*
		{
			switch (Value)
			{
			case PathUtilities::EMountOwner::Engine: return "Engine";
			case PathUtilities::EMountOwner::ActiveProject: return "Active project";
			case PathUtilities::EMountOwner::Extension: return "Extension";
			case PathUtilities::EMountOwner::ExternalSources: return "External sources";
			case PathUtilities::EMountOwner::Test: return "Test";
			}
			return "Unknown";
		}

		auto CanonicalResaveStatusName(Asset::EAssetCanonicalResavePackageStatus Value) -> const char*
		{
			switch (Value)
			{
			case Asset::EAssetCanonicalResavePackageStatus::Skipped: return "Skipped";
			case Asset::EAssetCanonicalResavePackageStatus::Ready: return "Ready";
			case Asset::EAssetCanonicalResavePackageStatus::Resaved: return "Resaved";
			case Asset::EAssetCanonicalResavePackageStatus::Blocked: return "Blocked";
			case Asset::EAssetCanonicalResavePackageStatus::Failed: return "Failed";
			case Asset::EAssetCanonicalResavePackageStatus::Cancelled: return "Cancelled";
			case Asset::EAssetCanonicalResavePackageStatus::Stale: return "Stale";
			}
			return "Unknown";
		}

		auto CountCanonicalResaveStatus(
			const Asset::FAssetCanonicalResavePlan& Plan,
			Asset::EAssetCanonicalResavePackageStatus Status) -> size_t
		{
			return static_cast<size_t>(std::ranges::count(
				Plan.Packages, Status, &Asset::FAssetCanonicalResavePackagePlan::Status));
		}

		auto IsCanonicalResaveRecommended(
			const Asset::FAssetPackageCompatibilityRecord& Record) -> bool
		{
			return !Record.CanonicalizationEvidence.empty()
				|| !Record.DeprecatedRouteEvidence.empty();
		}
	}

	auto FAssetCompatibilityWindow::Draw(bool& bOpen, const FRevealAsset& RevealAsset) -> void
	{
		if (!bOpen) return;
		ImGui::SetNextWindowSize(ImVec2(MonaImGui::ScaleUI(1100.0f), MonaImGui::ScaleUI(720.0f)), ImGuiCond_FirstUseEver);
		if (!ImGui::Begin("Asset Compatibility & Canonical Resave###Durin.AssetCompatibility", &bOpen))
		{
			ImGui::End();
			return;
		}

		// Comparison only: this does not scan the registry or touch package bytes.
		Audit.Tick();
		const uint64 CatalogRevision = Asset::GetAssetCatalogRevision();
		if (!ReconciledCatalogRevision || *ReconciledCatalogRevision != CatalogRevision)
		{
			const Asset::FAssetCatalogSnapshot Snapshot = Asset::CaptureAssetCatalogSnapshot();
			Audit.ReconcileAssetCatalog(Snapshot.Assets);
			ReconciledCatalogRevision = Snapshot.Revision;
		}
		TickCanonicalResave();
		const auto State = Audit.GetState();
		const bool bMaintenanceRunning = PendingMaintenancePlan.has_value();
		const bool bMaintenancePreviewed = PreviewMaintenancePlan.has_value();

		ImGui::BeginDisabled(State == Editor::EAssetCompatibilityAuditState::Running
			|| bMaintenanceRunning || bMaintenancePreviewed);
		if (ImGui::Button("Refresh Catalog")) RefreshCatalog();
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (State == Editor::EAssetCompatibilityAuditState::Running)
		{
			if (ImGui::Button("Cancel")) Audit.Cancel();
		}
		else
		{
			ImGui::BeginDisabled(bMaintenanceRunning || bMaintenancePreviewed);
			if (ImGui::Button(State == Editor::EAssetCompatibilityAuditState::Idle ? "Run Audit" : "Run Again"))
				RunAudit();
			ImGui::EndDisabled();
		}
		ImGui::SameLine();
		ImGui::TextDisabled("%s", StateName(State));
		const auto Progress = Audit.GetProgress();
		if (State == Editor::EAssetCompatibilityAuditState::Running)
		{
			ImGui::SameLine();
			const float Fraction = Progress.Total == 0 ? 1.0f
				: static_cast<float>(Progress.Completed) / static_cast<float>(Progress.Total);
			const std::string Overlay = std::format("{} / {}", Progress.Completed, Progress.Total);
			ImGui::ProgressBar(Fraction, ImVec2(MonaImGui::ScaleUI(220.0f), 0.0f), Overlay.c_str());
		}
		if (!WindowMessage.empty())
		{
			ImGui::SameLine();
			ImGui::TextDisabled("%s", WindowMessage.c_str());
		}

		const auto& Records = Audit.GetPresentationRecords();
		const uint64 PresentationRevision = Audit.GetPresentationRevision();
		if (!SummaryPresentationRevision || *SummaryPresentationRevision != PresentationRevision)
		{
			PresentationCounts = Editor::CountAssetCompatibilityAuditRecords(Records);
			CanonicalDebt = static_cast<size_t>(std::ranges::count_if(
				Records, IsCanonicalResaveRecommended));
			std::erase_if(SelectedPackages, [&](const FAssetPath& Path) {
				return Audit.FindRecord(Path) == nullptr;
			});
			SummaryPresentationRevision = PresentationRevision;
		}
		ImGui::Text("Compatible %zu   Incompatible %zu   Unsupported %zu   Failed %zu   Stale %zu   Not checked %zu",
			static_cast<size_t>(PresentationCounts.Compatible), static_cast<size_t>(PresentationCounts.Incompatible),
			static_cast<size_t>(PresentationCounts.Unsupported), static_cast<size_t>(PresentationCounts.Failed),
			static_cast<size_t>(PresentationCounts.Stale), static_cast<size_t>(PresentationCounts.NotChecked));
		ImGui::SameLine();
		ImGui::Text("Canonical resave recommended %zu", CanonicalDebt);
		if (PendingMaintenancePlan)
		{
			const size_t Total = CountCanonicalResaveStatus(*PendingMaintenancePlan,
				Asset::EAssetCanonicalResavePackageStatus::Ready) + MaintenanceCompleted;
			ImGui::Text("Canonical resave progress: %zu / %zu", MaintenanceCompleted, Total);
			ImGui::SameLine();
			if (ImGui::SmallButton("Cancel Canonical Resave")) bCancelMaintenance = true;
		}
		else if (!MaintenanceMessage.empty())
		{
			ImGui::TextWrapped("%s", MaintenanceMessage.c_str());
			if (ImGui::SmallButton("Copy Maintenance Report"))
				ImGui::SetClipboardText(MaintenanceMessage.c_str());
			if (PreviewMaintenanceSelection
				&& State == Editor::EAssetCompatibilityAuditState::Completed)
			{
				ImGui::SameLine();
				if (ImGui::SmallButton("Preview Last Scope"))
				{
					auto Selection = *PreviewMaintenanceSelection;
					const std::string Scope = PreviewMaintenanceScope;
					PreviewCanonicalResave(std::move(Selection), Scope);
				}
			}
		}

		ImGui::SeparatorText("Audit Results");
		static constexpr const char* FilterNames[] = {
			"All", "Issues", "Incompatible", "Unsupported", "Failed", "Stale", "Not checked"};
		int FilterIndex = static_cast<int>(Filter);
		ImGui::SetNextItemWidth(MonaImGui::ScaleUI(170.0f));
		if (ImGui::Combo("Filter", &FilterIndex, FilterNames, std::size(FilterNames)))
			Filter = static_cast<Editor::EAssetCompatibilityAuditFilter>(FilterIndex);
		ImGui::SameLine();
		ImGui::Checkbox("Canonical resave recommended only", &bCanonicalDebtOnly);
		ImGui::SameLine();
		ImGui::SetNextItemWidth(MonaImGui::ScaleUI(260.0f));
		ImGui::InputTextWithHint("##CompatibilitySearch", "Search paths and diagnostics...",
			SearchText.data(), SearchText.size());
		if (!FilteredPresentationRevision || *FilteredPresentationRevision != PresentationRevision
			|| CachedFilter != Filter || bCachedCanonicalDebtOnly != bCanonicalDebtOnly
			|| CachedSearchText != SearchText.data())
		{
			FilteredRecordIndices.clear();
			FilteredRecordIndices.reserve(Records.size());
			FilteredCanonicalDebt = 0;
			for (size_t Index = 0; Index < Records.size(); ++Index)
			{
				const auto& Record = Records[Index];
				if (!Editor::MatchesAssetCompatibilityAuditFilter(Record, Filter)) continue;
				if (bCanonicalDebtOnly && !IsCanonicalResaveRecommended(Record)) continue;
				if (!Editor::MatchesAssetCompatibilityAuditSearch(Record, SearchText.data())) continue;
				FilteredRecordIndices.push_back(Index);
				FilteredCanonicalDebt += IsCanonicalResaveRecommended(Record);
			}
			FilteredPresentationRevision = PresentationRevision;
			CachedFilter = Filter;
			bCachedCanonicalDebtOnly = bCanonicalDebtOnly;
			CachedSearchText = SearchText.data();
		}

		ImGui::BeginDisabled(FilteredRecordIndices.empty());
		if (ImGui::SmallButton("Select Filtered"))
			for (const size_t Index : FilteredRecordIndices)
				SelectedPackages.insert(Records[Index].PackagePath);
		ImGui::SameLine();
		if (ImGui::SmallButton("Copy Filtered Report")) CopyFilteredReport();
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(SelectedPackages.empty());
		if (ImGui::SmallButton("Clear Selection")) SelectedPackages.clear();
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::TextDisabled("%zu selected", SelectedPackages.size());

		ImGui::SeparatorText("Canonical Resave");
		const bool bCanPlanMaintenance = State == Editor::EAssetCompatibilityAuditState::Completed
			&& !bMaintenanceRunning && !bMaintenancePreviewed;
		if (State != Editor::EAssetCompatibilityAuditState::Completed)
			ImGui::TextDisabled("Complete the audit before previewing a canonical resave plan.");
		else if (!bMaintenanceRunning && !bMaintenancePreviewed)
			ImGui::TextDisabled("Every write requires a reviewed plan and explicit confirmation.");
		ImGui::BeginDisabled(!bCanPlanMaintenance || SelectedPackages.empty());
		if (ImGui::Button("Preview Selected Resaves"))
		{
			Asset::FAssetCanonicalResaveSelection Selection;
			Selection.Packages.assign(SelectedPackages.begin(), SelectedPackages.end());
			PreviewCanonicalResave(std::move(Selection),
				std::format("{} selected package(s)", SelectedPackages.size()));
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(!bCanPlanMaintenance || FilteredCanonicalDebt == 0);
		if (ImGui::Button(std::format(
			"Preview Filtered Recommended ({})", FilteredCanonicalDebt).c_str()))
		{
			Asset::FAssetCanonicalResaveSelection Selection;
			for (const size_t Index : FilteredRecordIndices)
				if (IsCanonicalResaveRecommended(Records[Index]))
					Selection.Packages.push_back(Records[Index].PackagePath);
			PreviewCanonicalResave(std::move(Selection), "filtered recommended packages");
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(!bCanPlanMaintenance || CanonicalDebt == 0);
		if (ImGui::Button("Preview All Recommended"))
		{
			Asset::FAssetCanonicalResaveSelection Selection;
			for (const auto& Record : Records)
				if (IsCanonicalResaveRecommended(Record)) Selection.Packages.push_back(Record.PackagePath);
			PreviewCanonicalResave(std::move(Selection), "all project recommendations");
		}
		ImGui::EndDisabled();

		if (PreviewMaintenancePlan) DrawMaintenancePlan();

		if (State == Editor::EAssetCompatibilityAuditState::Idle)
			ImGui::TextDisabled("Opening this window does not inspect packages. Select Run Audit to begin.");
		else if (State == Editor::EAssetCompatibilityAuditState::Cancelled)
			ImGui::TextDisabled("The audit was cancelled. Completed rows are retained; unchecked rows were not inspected.");
		else if (State == Editor::EAssetCompatibilityAuditState::Failed)
			ImGui::TextWrapped("Audit failed: %s", Audit.GetFailure().c_str());
		else if (Records.empty()) ImGui::TextDisabled("No packages are registered for this project.");

		const float DetailsHeight = MonaImGui::ScaleUI(205.0f);
		if (ImGui::BeginTable("CompatibilityRows", 6,
			ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
			ImVec2(0.0f, -DetailsHeight)))
		{
			ImGui::TableSetupColumn("##Select", ImGuiTableColumnFlags_WidthFixed, MonaImGui::ScaleUI(28.0f));
			ImGui::TableSetupColumn("Package", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Inspection", ImGuiTableColumnFlags_WidthFixed, MonaImGui::ScaleUI(95.0f));
			ImGui::TableSetupColumn("Compatibility", ImGuiTableColumnFlags_WidthFixed, MonaImGui::ScaleUI(110.0f));
			ImGui::TableSetupColumn("Freshness", ImGuiTableColumnFlags_WidthFixed, MonaImGui::ScaleUI(75.0f));
			ImGui::TableSetupColumn("Findings", ImGuiTableColumnFlags_WidthFixed, MonaImGui::ScaleUI(65.0f));
			ImGui::TableHeadersRow();
			ImGuiListClipper Clipper;
			Clipper.Begin(static_cast<int>(std::min(
				FilteredRecordIndices.size(), static_cast<size_t>(std::numeric_limits<int>::max()))));
			while (Clipper.Step())
			{
				for (int RowIndex = Clipper.DisplayStart; RowIndex < Clipper.DisplayEnd; ++RowIndex)
				{
					const auto& Record = Records[FilteredRecordIndices[static_cast<size_t>(RowIndex)]];
					const std::string PackagePath = Record.PackagePath.ToString();
					ImGui::PushID(PackagePath.c_str());
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					bool bSelectedForMaintenance = SelectedPackages.contains(Record.PackagePath);
					if (ImGui::Checkbox("##MaintenanceSelected", &bSelectedForMaintenance))
					{
						if (bSelectedForMaintenance) SelectedPackages.insert(Record.PackagePath);
						else SelectedPackages.erase(Record.PackagePath);
					}
					ImGui::TableSetColumnIndex(1);
					if (ImGui::Selectable(PackagePath.c_str(), Record.PackagePath == SelectedPath))
						SelectedPath = Record.PackagePath;
					ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(InspectionName(Record.Inspection));
					ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(CompatibilityName(Record.Compatibility));
					ImGui::TableSetColumnIndex(4); ImGui::TextUnformatted(
						Record.Freshness == Asset::EAssetCompatibilityFreshness::Current ? "Current" : "Stale");
					ImGui::TableSetColumnIndex(5); ImGui::Text("%zu + %zu resave", Record.Findings.size(),
						Record.CanonicalizationEvidence.size() + Record.DeprecatedRouteEvidence.size());
					ImGui::PopID();
				}
			}
			ImGui::EndTable();
		}
		DrawDetails(RevealAsset, bCanPlanMaintenance);
		DrawApplyConfirmation();
		ImGui::End();
	}

	auto FAssetCompatibilityWindow::DrawDetails(
		const FRevealAsset& RevealAsset,
		bool bCanPlanMaintenance) -> void
	{
		const auto* Record = SelectedPath.IsValid() ? Audit.FindRecord(SelectedPath) : nullptr;
		ImGui::SeparatorText("Details");
		if (!Record) { ImGui::TextDisabled("Select a package to inspect its findings."); return; }
		ImGui::TextUnformatted(Record->PackagePath.ToString().c_str());
		const PathUtilities::FMountLookupResult Mount =
			PathUtilities::FindMountForVirtualPath(Record->PackagePath.GetView());
		if (Mount)
		{
			ImGui::SameLine();
			ImGui::TextDisabled("%s mount; content writes %s", MountOwnerName(Mount.Mount->Owner),
				Mount.Mount->bContentWritable ? "writable" : "read-only");
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("Copy Diagnostics")) CopySelectedDiagnostics();
		ImGui::SameLine();
		if (ImGui::SmallButton("Show in Content Browser") && RevealAsset) RevealAsset(Record->PackagePath);
		if (IsCanonicalResaveRecommended(*Record))
		{
			ImGui::SameLine();
			ImGui::BeginDisabled(!bCanPlanMaintenance);
			if (ImGui::SmallButton("Preview Package Resave"))
			{
				Asset::FAssetCanonicalResaveSelection Selection{.Packages = {Record->PackagePath}};
				PreviewCanonicalResave(std::move(Selection), Record->PackagePath.ToString());
			}
			ImGui::SameLine();
			if (ImGui::SmallButton("Preview Folder Recommendations"))
			{
				const std::string Path = Record->PackagePath.ToString();
				std::string Folder = Path.substr(0, Path.rfind('/'));
				if (Folder.empty()) Folder = "/";
				std::vector<FAssetPath> Packages;
				for (const auto& Candidate : Audit.GetPresentationRecords())
					if (IsCanonicalResaveRecommended(Candidate)
						&& (Folder == "/" || Candidate.PackagePath.GetView().starts_with(Folder + "/")))
						Packages.push_back(Candidate.PackagePath);
				Asset::FAssetCanonicalResaveSelection Selection{.Packages = std::move(Packages)};
				PreviewCanonicalResave(std::move(Selection), std::format("folder {}", Folder));
			}
			if (Mount)
			{
				ImGui::SameLine();
				if (ImGui::SmallButton("Preview Mount Recommendations"))
				{
					std::vector<FAssetPath> Packages;
					for (const auto& Candidate : Audit.GetPresentationRecords())
						if (IsCanonicalResaveRecommended(Candidate)
							&& Candidate.PackagePath.GetView().starts_with(Mount.Mount->VirtualRoot))
							Packages.push_back(Candidate.PackagePath);
					Asset::FAssetCanonicalResaveSelection Selection{.Packages = std::move(Packages)};
					PreviewCanonicalResave(std::move(Selection),
						std::format("mount {}", Mount.Mount->VirtualRoot));
				}
			}
			ImGui::EndDisabled();
		}
		if (Record->Freshness == Asset::EAssetCompatibilityFreshness::Stale)
			ImGui::TextDisabled("This result is stale because the package fingerprint changed. Run the audit again.");
		ImGui::BeginChild("CompatibilityFindingDetails", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
		if (Record->Findings.empty()) ImGui::TextDisabled("No compatibility findings.");
		for (const auto& Finding : Record->Findings)
		{
			ImGui::SeparatorText(Asset::AssetCompatibilityFindingCodeName(Finding.Code).data());
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
			Plan, Asset::EAssetCanonicalResavePackageStatus::Ready);
		const size_t BlockedCount = CountCanonicalResaveStatus(
			Plan, Asset::EAssetCanonicalResavePackageStatus::Blocked);
		const size_t SkippedCount = CountCanonicalResaveStatus(
			Plan, Asset::EAssetCanonicalResavePackageStatus::Skipped);
		const bool bPlanStale = Plan.RegistryRevision != Asset::GetAssetCatalogRevision();

		ImGui::SeparatorText("Canonical Resave Plan");
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
			ImGui::OpenPopup("Confirm Canonical Resave");
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("Copy Plan Report"))
		{
			const std::string Report = Asset::SerializeAssetCanonicalResavePlanReport(Plan);
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
		if (!ImGui::BeginPopupModal("Confirm Canonical Resave", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) return;
		const size_t ReadyCount = PreviewMaintenancePlan
			? CountCanonicalResaveStatus(*PreviewMaintenancePlan,
				Asset::EAssetCanonicalResavePackageStatus::Ready)
			: 0;
		ImGui::Text("Apply canonical resave to %zu package(s)?", ReadyCount);
		ImGui::TextWrapped("This writes authored package files. Check out the listed files in source control "
			"and review their diffs after completion.");
		const bool bCanApply = PreviewMaintenancePlan && ReadyCount != 0
			&& PreviewMaintenancePlan->RegistryRevision == Asset::GetAssetCatalogRevision()
			&& CountCanonicalResaveStatus(*PreviewMaintenancePlan,
				Asset::EAssetCanonicalResavePackageStatus::Blocked) == 0;
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

	auto FAssetCompatibilityWindow::CopyFilteredReport() const -> void
	{
		const auto& Records = Audit.GetPresentationRecords();
		std::vector<Asset::FAssetPackageCompatibilityRecord> FilteredRecords;
		FilteredRecords.reserve(FilteredRecordIndices.size());
		for (const size_t Index : FilteredRecordIndices) FilteredRecords.push_back(Records[Index]);
		const std::string Report = Editor::FormatAssetCompatibilityAuditReport(FilteredRecords);
		ImGui::SetClipboardText(Report.c_str());
	}

	auto FAssetCompatibilityWindow::RefreshCatalog() -> void
	{
		const Asset::FAssetCatalogRefreshResult Result = Asset::RefreshAssetCatalog();
		if (!Result)
		{
			WindowMessage = Result.Errors.empty()
				? "Asset catalog refresh did not publish a complete result."
				: Result.Errors.front().Message;
			return;
		}
		const Asset::FAssetCatalogSnapshot Snapshot = Asset::CaptureAssetCatalogSnapshot();
		Audit.ReconcileAssetCatalog(Snapshot.Assets);
		ReconciledCatalogRevision = Snapshot.Revision;
		WindowMessage = std::format("Catalog refreshed: {} assets reparsed, {} reused.",
			Result.CatalogStats.Reparsed, Result.CatalogStats.Reused);
	}

	auto FAssetCompatibilityWindow::RunAudit() -> void
	{
		PreviewMaintenancePlan.reset();
		PreviewMaintenanceSelection.reset();
		PreviewMaintenanceScope.clear();
		MaintenanceMessage.clear();
		WindowMessage.clear();
		SelectedPath = {};
		SelectedPackages.clear();
		(void)Audit.RunCurrentProjectAudit();
	}

	auto FAssetCompatibilityWindow::PreviewCanonicalResave(
		Asset::FAssetCanonicalResaveSelection Selection,
		std::string Scope) -> void
	{
		if (PendingMaintenancePlan
			|| Audit.GetState() != Editor::EAssetCompatibilityAuditState::Completed) return;
		PreviewMaintenanceSelection = Selection;
		PreviewMaintenanceScope = std::move(Scope);
		PreviewMaintenancePlan = Asset::PlanAssetCanonicalResaves(
			Audit.GetPresentationRecords(), Selection);
		MaintenanceMessage.clear();
	}

	auto FAssetCompatibilityWindow::BeginCanonicalResave() -> void
	{
		if (!PreviewMaintenancePlan) return;
		if (PreviewMaintenancePlan->RegistryRevision != Asset::GetAssetCatalogRevision())
		{
			MaintenanceMessage = "The asset catalog changed after planning. Re-plan before applying.";
			return;
		}
		if (CountCanonicalResaveStatus(*PreviewMaintenancePlan,
			Asset::EAssetCanonicalResavePackageStatus::Blocked) != 0) return;
		PendingMaintenancePlan = std::move(PreviewMaintenancePlan);
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
				(void)Audit.RunCurrentProjectAudit();
				SelectedPath = {};
			}
			return;
		}
		auto Ready = std::ranges::find(
			PendingMaintenancePlan->Packages,
			Asset::EAssetCanonicalResavePackageStatus::Ready,
			&Asset::FAssetCanonicalResavePackagePlan::Status);
		if (Ready == PendingMaintenancePlan->Packages.end())
		{
			MaintenanceMessage = std::format(
				"Canonical resave completed: {} package(s) resaved.", MaintenanceCompleted);
			PendingMaintenancePlan.reset();
			bCancelMaintenance = false;
			(void)Audit.RunCurrentProjectAudit();
			SelectedPath = {};
			return;
		}
		Asset::FAssetCanonicalResavePlan Unit;
		Unit.RegistryRevision = Asset::GetAssetCatalogRevision();
		Unit.Packages.push_back(*Ready);
		const Asset::FReflectionCompatibilityCatalog Catalog =
			Asset::FReflectionCompatibilityCatalog::Capture();
		const auto Applied = Asset::ApplyAssetCanonicalResaves(std::move(Unit), Catalog);
		*Ready = Applied.Plan.Packages.front();
		if (Applied.Status != Asset::EAssetCanonicalResaveApplyStatus::Succeeded)
		{
			MaintenanceMessage = std::format(
				"Canonical resave stopped after {} completed package(s).\n{}",
				MaintenanceCompleted, Asset::SerializeAssetCanonicalResaveApplyReport(Applied));
			PendingMaintenancePlan.reset();
			bCancelMaintenance = false;
			(void)Audit.RunCurrentProjectAudit();
			SelectedPath = {};
			return;
		}
		++MaintenanceCompleted;
	}

	auto FAssetCompatibilityWindow::CopySelectedDiagnostics() const -> void
	{
		const auto* Record = Audit.FindRecord(SelectedPath);
		if (!Record) return;
		const std::string Text = Editor::FormatAssetCompatibilityAuditDiagnostics(*Record);
		ImGui::SetClipboardText(Text.c_str());
	}
}
