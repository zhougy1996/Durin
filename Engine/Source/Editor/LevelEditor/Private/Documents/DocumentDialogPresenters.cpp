#include "Documents/DocumentDialogPresenters.h"

#include "AssetSystem.h"
#include "MonaImGui.h"

namespace Durin
{
	namespace
	{
		constexpr auto AssetStructureUpgradePopup = "Asset Structure Upgrade Required";

		constexpr auto GetClassificationLabel(Asset::EAssetCompatibilityClassification Classification) -> std::string_view
		{
			switch (Classification)
			{
			case Asset::EAssetCompatibilityClassification::SafeCleanup: return "Safe Cleanup";
			case Asset::EAssetCompatibilityClassification::Migrated: return "Migrated";
			case Asset::EAssetCompatibilityClassification::DataLossRisk: return "Data Loss Risk";
			case Asset::EAssetCompatibilityClassification::UnknownIncompatible: return "Unknown Incompatible";
			}
			return "Unknown";
		}

		constexpr auto GetRiskLabel(Asset::EAssetCompatibilityRisk Risk) -> std::string_view
		{
			switch (Risk)
			{
			case Asset::EAssetCompatibilityRisk::None: return "None";
			case Asset::EAssetCompatibilityRisk::PotentialDataLoss: return "Potential Data Loss";
			case Asset::EAssetCompatibilityRisk::UnknownNewerSchema: return "Unknown Newer Schema";
			}
			return "Unknown";
		}

		auto GetTargetStructureLabel(const Asset::FAssetCompatibilityIssue& Issue) -> std::string
		{
			switch (Issue.Classification)
			{
			case Asset::EAssetCompatibilityClassification::SafeCleanup:
				return "Removed fields; no replacement storage";
			case Asset::EAssetCompatibilityClassification::Migrated:
				return std::format("Current reflected structure on {}", Issue.DeclaringClass);
			case Asset::EAssetCompatibilityClassification::DataLossRisk:
			case Asset::EAssetCompatibilityClassification::UnknownIncompatible:
				return "No recognized target structure";
			}
			return "Unknown";
		}

		auto DrawCompatibilityDetail(std::string_view Label, std::string_view Value) -> void
		{
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextDisabled("%.*s", static_cast<int>(Label.size()), Label.data());
			ImGui::TableSetColumnIndex(1);
			ImGui::TextWrapped("%.*s", static_cast<int>(Value.size()), Value.data());
		}

		auto JoinLegacyFieldNames(const Asset::FAssetCompatibilityIssue& Issue) -> std::string
		{
			std::string Result;
			for (const Asset::FAssetLegacyField& Field : Issue.LegacyFields)
			{
				if (!Result.empty()) Result += ", ";
				Result += Field.Name;
			}
			return Result;
		}

		auto JoinLegacyFieldTypes(const Asset::FAssetCompatibilityIssue& Issue) -> std::string
		{
			std::string Result;
			for (const Asset::FAssetLegacyField& Field : Issue.LegacyFields)
			{
				if (!Result.empty()) Result += "\n";
				Result += std::format("{}: {}", Field.Name, Field.TypeSignature);
			}
			return Result;
		}
	} // namespace

	auto FUnsavedLevelDialogPresenter::Draw(bool bRequestOpen, const FResolve& Resolve)
		-> std::optional<EUnsavedLevelDialogDecision>
	{
		if (bRequestOpen) ImGui::OpenPopup("Unsaved Level");

		if (!ImGui::BeginPopupModal(
			"Unsaved Level",
			nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings))
			return std::nullopt;

		std::optional<EUnsavedLevelDialogDecision> Decision;
		ImGui::TextUnformatted("The current level has unsaved changes.");
		if (ImGui::Button("Save")) Decision = EUnsavedLevelDialogDecision::Save;
		ImGui::SameLine();
		if (ImGui::Button("Discard")) Decision = EUnsavedLevelDialogDecision::Discard;
		ImGui::SameLine();
		if (ImGui::Button("Cancel")) Decision = EUnsavedLevelDialogDecision::Cancel;

		if (Decision && (!Resolve || Resolve(*Decision))) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		return Decision;
	}

	auto FAssetStructureUpgradeDialogPresenter::Draw(
		const FAssetStructureUpgradeModel& Model,
		bool bRequestOpen,
		bool& bDataLossConfirmed,
		const FResolve& Resolve
	) -> std::optional<EAssetStructureUpgradeDecision>
	{
		if (!Model.IsPending()) return std::nullopt;
		if (bRequestOpen && !ImGui::IsPopupOpen(AssetStructureUpgradePopup))
			ImGui::OpenPopup(AssetStructureUpgradePopup);
		ImGui::SetNextWindowPos(
			ImGui::GetMainViewport()->GetCenter(),
			ImGuiCond_Appearing,
			ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSizeConstraints(
			ImVec2(MonaImGui::ScaleUI(620.0f), MonaImGui::ScaleUI(480.0f)),
			ImVec2(MonaImGui::ScaleUI(960.0f), MonaImGui::ScaleUI(760.0f)));
		if (!ImGui::BeginPopupModal(
			AssetStructureUpgradePopup,
			nullptr,
			ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings))
			return std::nullopt;

		const Asset::FAssetLoadReport& PendingLoadReport = Model.GetReport();
		const bool bHasRisk = PendingLoadReport.HasRiskItems();
		ImGui::TextWrapped(
			"%s contains %llu compatibility change%s across %llu object%s and %llu serialized field%s.",
			PendingLoadReport.PackagePath.ToString().c_str(),
			PendingLoadReport.CompatibilityIssues.size(),
			PendingLoadReport.CompatibilityIssues.size() == 1 ? "" : "s",
			PendingLoadReport.GetAffectedObjectCount(),
			PendingLoadReport.GetAffectedObjectCount() == 1 ? "" : "s",
			PendingLoadReport.GetLegacyFieldCount(),
			PendingLoadReport.GetLegacyFieldCount() == 1 ? "" : "s");
		if (bHasRisk)
		{
			ImGui::Spacing();
			ImGui::TextWrapped(
				"%llu change%s may discard data. Normal upgrade-and-save is disabled.",
				PendingLoadReport.GetRiskItemCount(),
				PendingLoadReport.GetRiskItemCount() == 1 ? "" : "s");
		}

		ImGui::Spacing();
		if (ImGui::BeginChild(
			"CompatibilityChanges",
			ImVec2(0.0f, -MonaImGui::ScaleUI(bHasRisk ? 118.0f : 72.0f)),
			true))
		{
			const std::string PackageLabel = std::format("{}##Package", PendingLoadReport.PackagePath.ToString());
			if (ImGui::TreeNodeEx(PackageLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
			{
				for (size_t IssueIndex = 0; IssueIndex < PendingLoadReport.CompatibilityIssues.size(); ++IssueIndex)
				{
					const Asset::FAssetCompatibilityIssue& Issue = PendingLoadReport.CompatibilityIssues[IssueIndex];
					const bool bFirstForObject = std::ranges::find_if(
						PendingLoadReport.CompatibilityIssues,
						[&Issue](const Asset::FAssetCompatibilityIssue& Candidate) {
							return Candidate.ObjectPath == Issue.ObjectPath;
						}) == PendingLoadReport.CompatibilityIssues.begin() + static_cast<std::ptrdiff_t>(IssueIndex);
					if (!bFirstForObject) continue;

					const std::string ObjectLabel = std::format("{}##Object{}", Issue.ObjectPath, IssueIndex);
					if (!ImGui::TreeNodeEx(ObjectLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) continue;
					for (size_t ChangeIndex = IssueIndex; ChangeIndex < PendingLoadReport.CompatibilityIssues.size(); ++ChangeIndex)
					{
						const Asset::FAssetCompatibilityIssue& Change = PendingLoadReport.CompatibilityIssues[ChangeIndex];
						if (Change.ObjectPath != Issue.ObjectPath) continue;
						const std::string ChangeLabel = std::format(
							"{}: {}##Change{}",
							GetClassificationLabel(Change.Classification),
							Change.MigrationSummary,
							ChangeIndex);
						if (!ImGui::TreeNodeEx(ChangeLabel.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth)) continue;
						if (ImGui::BeginTable(
							"CompatibilityDetail",
							2,
							ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg))
						{
							ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, MonaImGui::ScaleUI(132.0f));
							ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
							DrawCompatibilityDetail("Old type", JoinLegacyFieldTypes(Change));
							DrawCompatibilityDetail("Target structure", GetTargetStructureLabel(Change));
							DrawCompatibilityDetail("Migration rule", Change.HandlerId.empty() ? "No registered rule" : Change.HandlerId);
							DrawCompatibilityDetail("Original fields", JoinLegacyFieldNames(Change));
							DrawCompatibilityDetail("Classification", GetClassificationLabel(Change.Classification));
							DrawCompatibilityDetail("Summary", Change.MigrationSummary);
							DrawCompatibilityDetail("Risk", GetRiskLabel(Change.Risk));
							ImGui::EndTable();
						}
						ImGui::TreePop();
					}
					ImGui::TreePop();
				}
				ImGui::TreePop();
			}
		}
		ImGui::EndChild();

		std::optional<EAssetStructureUpgradeDecision> Decision;
		if (bHasRisk)
		{
			ImGui::Checkbox("I understand that saving will permanently discard the incompatible data.", &bDataLossConfirmed);
			ImGui::BeginDisabled(!bDataLossConfirmed);
			if (ImGui::Button("Discard Incompatible Data, Save and Open"))
				Decision = EAssetStructureUpgradeDecision::DiscardIncompatibleDataSaveAndOpen;
			ImGui::EndDisabled();
		}
		else if (ImGui::Button("Upgrade, Save and Open"))
			Decision = EAssetStructureUpgradeDecision::SaveAndOpen;

		if (Decision)
		{
			const EAssetStructureUpgradeResult Result = Resolve
				? Resolve(*Decision)
				: EAssetStructureUpgradeResult::Rejected;
			if (Result != EAssetStructureUpgradeResult::SaveFailed) ImGui::CloseCurrentPopup();
		}

		if (bHasRisk || Decision) ImGui::SameLine();
		if (ImGui::Button("Open Without Saving")) Decision = EAssetStructureUpgradeDecision::OpenWithoutSaving;
		if (Decision == EAssetStructureUpgradeDecision::OpenWithoutSaving)
		{
			const EAssetStructureUpgradeResult Result = Resolve
				? Resolve(*Decision)
				: EAssetStructureUpgradeResult::Rejected;
			(void)Result;
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel Open")) Decision = EAssetStructureUpgradeDecision::Cancel;
		if (Decision == EAssetStructureUpgradeDecision::Cancel)
		{
			const EAssetStructureUpgradeResult Result = Resolve
				? Resolve(*Decision)
				: EAssetStructureUpgradeResult::Rejected;
			(void)Result;
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
		return Decision;
	}
} // namespace Durin
