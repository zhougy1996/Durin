#include "AssetCompatibilityWindow.h"

#include "AssetSystem.h"
#include "MonaImGui.h"
#include "Misc/Paths.h"

namespace Durin
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
	}

	auto FAssetCompatibilityWindow::Draw(bool& bOpen, const FRevealAsset& RevealAsset) -> void
	{
		if (!bOpen) return;
		ImGui::SetNextWindowSize(ImVec2(MonaImGui::ScaleUI(980.0f), MonaImGui::ScaleUI(620.0f)), ImGuiCond_FirstUseEver);
		if (!ImGui::Begin("Asset Compatibility###Durin.AssetCompatibility", &bOpen))
		{
			ImGui::End();
			return;
		}

		// Comparison only: this does not scan the registry or touch package bytes.
		Audit.Tick(Asset::GetAssetRegistry().GetAssets());
		const auto State = Audit.GetState();
		if (State == Editor::EAssetCompatibilityAuditState::Running)
		{
			if (ImGui::Button("Cancel")) Audit.Cancel();
		}
		else if (ImGui::Button(State == Editor::EAssetCompatibilityAuditState::Idle ? "Run Audit" : "Run Again"))
		{
			Audit.RunCurrentProjectAudit();
			SelectedPath = {};
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

		const auto Records = Audit.GetPresentationRecords();
		const auto Counts = Editor::CountAssetCompatibilityAuditRecords(Records);
		ImGui::Text("Compatible %zu   Incompatible %zu   Unsupported %zu   Failed %zu   Stale %zu   Not checked %zu",
			static_cast<size_t>(Counts.Compatible), static_cast<size_t>(Counts.Incompatible),
			static_cast<size_t>(Counts.Unsupported), static_cast<size_t>(Counts.Failed),
			static_cast<size_t>(Counts.Stale), static_cast<size_t>(Counts.NotChecked));

		static constexpr const char* FilterNames[] = {
			"All", "Issues", "Incompatible", "Unsupported", "Failed", "Stale", "Not checked"};
		int FilterIndex = static_cast<int>(Filter);
		ImGui::SetNextItemWidth(MonaImGui::ScaleUI(170.0f));
		if (ImGui::Combo("Filter", &FilterIndex, FilterNames, std::size(FilterNames)))
			Filter = static_cast<Editor::EAssetCompatibilityAuditFilter>(FilterIndex);

		if (State == Editor::EAssetCompatibilityAuditState::Idle)
			ImGui::TextDisabled("Opening this window does not inspect packages. Select Run Audit to begin.");
		else if (State == Editor::EAssetCompatibilityAuditState::Cancelled)
			ImGui::TextDisabled("The audit was cancelled. Completed rows are retained; unchecked rows were not inspected.");
		else if (State == Editor::EAssetCompatibilityAuditState::Failed)
			ImGui::TextWrapped("Audit failed: %s", Audit.GetFailure().c_str());
		else if (Records.empty()) ImGui::TextDisabled("No packages are registered for this project.");

		const float DetailsHeight = MonaImGui::ScaleUI(205.0f);
		if (ImGui::BeginTable("CompatibilityRows", 5,
			ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
			ImVec2(0.0f, -DetailsHeight)))
		{
			ImGui::TableSetupColumn("Package", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Inspection", ImGuiTableColumnFlags_WidthFixed, MonaImGui::ScaleUI(95.0f));
			ImGui::TableSetupColumn("Compatibility", ImGuiTableColumnFlags_WidthFixed, MonaImGui::ScaleUI(110.0f));
			ImGui::TableSetupColumn("Freshness", ImGuiTableColumnFlags_WidthFixed, MonaImGui::ScaleUI(75.0f));
			ImGui::TableSetupColumn("Findings", ImGuiTableColumnFlags_WidthFixed, MonaImGui::ScaleUI(65.0f));
			ImGui::TableHeadersRow();
			for (const auto& Record : Records)
			{
				if (!Editor::MatchesAssetCompatibilityAuditFilter(Record, Filter)) continue;
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				if (ImGui::Selectable(Record.PackagePath.ToString().c_str(), Record.PackagePath == SelectedPath,
					ImGuiSelectableFlags_SpanAllColumns)) SelectedPath = Record.PackagePath;
				ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(InspectionName(Record.Inspection));
				ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(CompatibilityName(Record.Compatibility));
				ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(
					Record.Freshness == Asset::EAssetCompatibilityFreshness::Current ? "Current" : "Stale");
				ImGui::TableSetColumnIndex(4); ImGui::Text("%zu", Record.Findings.size());
			}
			ImGui::EndTable();
		}
		DrawDetails(RevealAsset);
		ImGui::End();
	}

	auto FAssetCompatibilityWindow::DrawDetails(const FRevealAsset& RevealAsset) -> void
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
			ImGui::TextDisabled("%s mount; authoring %s", MountOwnerName(Mount.Mount->Owner),
				Mount.Mount->bAuthoringWritable ? "writable" : "read-only");
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("Copy Diagnostics")) CopySelectedDiagnostics();
		ImGui::SameLine();
		if (ImGui::SmallButton("Show in Content Browser") && RevealAsset) RevealAsset(Record->PackagePath);
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
		ImGui::EndChild();
	}

	auto FAssetCompatibilityWindow::CopySelectedDiagnostics() const -> void
	{
		const auto* Record = Audit.FindRecord(SelectedPath);
		if (!Record) return;
		const std::string Text = Editor::FormatAssetCompatibilityAuditDiagnostics(*Record);
		ImGui::SetClipboardText(Text.c_str());
	}
}
