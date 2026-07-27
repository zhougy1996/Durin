#include "Documents/LevelDocumentController.h"
#include "Documents/LevelDocumentRevisionState.h"

#include "Asset/AssetUpgradeAuditService.h"
#include "AssetSystem.h"
#include "Editor/EditorEngine.h"
#include "Editor/EditorTransaction.h"
#include "Settings/LevelEditorSessionSettings.h"
#include "Assets/EditorAssetMoveCoordinator.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Workspace/LevelEditorContext.h"
#include "Misc/Project.h"
#include "MonaImGui.h"
#include "Panels/SceneViewportPanel.h"

namespace Durin
{
	namespace
	{
		auto GetLevelTransactions() -> FEditorTransactionManager*
		{
			return GEditor ? &GEditor->GetTransactionManager() : nullptr;
		}

		auto PublishWorkspaceLoadReport(const Asset::FAssetLoadReport& Report) -> void
		{
			if (GEditor) GEditor->GetAssetUpgradeAuditService().MergeWorkspaceLoadReport(Report);
		}

		auto InvalidateAssetUpgradeReport(const FAssetPath& Path) -> void
		{
			if (GEditor) GEditor->GetAssetUpgradeAuditService().InvalidatePackage(Path);
		}

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
	}

	// Owns level document transitions and the unsaved-change workflow.
	FLevelDocumentController::FLevelDocumentController(
		FLevelEditorContext& InContext,
		FLevelEditorSessionSettings& InSessionSettings,
		FSceneViewportPanel& InSceneViewportPanel,
		FEditorAssetMoveCoordinator& InAssetMoveCoordinator,
		std::string& InDefaultLevel,
		std::function<void()> InClearError,
		std::function<void(std::string)> InReportError,
		std::function<void(bool)> InCompleteDeferredOpen
	)
		: Context(InContext)
		, SessionSettings(InSessionSettings)
		, SceneViewportPanel(InSceneViewportPanel)
		, AssetMoveCoordinator(InAssetMoveCoordinator)
		, DefaultLevel(InDefaultLevel)
		, ClearError(std::move(InClearError))
		, ReportError(std::move(InReportError))
		, CompleteDeferredOpen(std::move(InCompleteDeferredOpen))
	{
	}

	auto FLevelDocumentController::RequestAction(ELevelDocumentAction Action) -> void
	{
		if (PendingUpgrade.IsPending()) return;
		PendingLevelPath.clear();
		bPendingDocumentOpen = false;
		PendingAction = Action;
		if (Context.Level && Context.Level->GetPackage() && Context.Level->GetPackage()->IsDirty())
		{
			QueuedPopup = EQueuedPopup::UnsavedLevel;
			return;
		}
		ExecutePendingAction();
	}

	auto FLevelDocumentController::RequestOpenLevel(std::string Path) -> ELevelDocumentOpenResult
	{
		if (PendingUpgrade.IsPending()) return ELevelDocumentOpenResult::Rejected;
		FAssetPath AssetPath;
		if (!FAssetPath::TryCreate(Path, AssetPath)) return ELevelDocumentOpenResult::Rejected;
		const Asset::FAssetData* Data = Asset::GetAssetRegistry().FindAsset(AssetPath);
		if (!Data || Data->AssetClassName != DLevel::StaticClass()->GetQualifiedName().ToString())
			return ELevelDocumentOpenResult::Rejected;
		PendingLevelPath = std::move(Path);
		PendingAction = ELevelDocumentAction::OpenLevel;
		bPendingDocumentOpen = true;
		if (Context.Level && Context.Level->GetPackage() && Context.Level->GetPackage()->IsDirty())
		{
			QueuedPopup = EQueuedPopup::UnsavedLevel;
			return ELevelDocumentOpenResult::Deferred;
		}
		const ELevelDocumentOpenResult Result = OpenLevel(PendingLevelPath);
		if (Result != ELevelDocumentOpenResult::Deferred)
		{
			PendingLevelPath.clear();
			PendingAction = ELevelDocumentAction::None;
			bPendingDocumentOpen = false;
		}
		return Result;
	}

	auto FLevelDocumentController::ExecutePendingAction() -> ELevelDocumentOpenResult
	{
		if (PendingAction == ELevelDocumentAction::OpenLevel)
		{
			if (!PendingLevelPath.empty())
			{
				const ELevelDocumentOpenResult Result = OpenLevel(PendingLevelPath);
				if (Result != ELevelDocumentOpenResult::Deferred)
				{
					PendingLevelPath.clear();
					PendingAction = ELevelDocumentAction::None;
				}
				return Result;
			}
			PendingAction = ELevelDocumentAction::None;
			return ELevelDocumentOpenResult::Rejected;
		}
		else if (PendingAction == ELevelDocumentAction::OpenProject)
		{
			std::string Error;
			if (!RelaunchEditorForProject({}, &Error))
			{
				SetError(std::move(Error));
				return ELevelDocumentOpenResult::Rejected;
			}
			return ELevelDocumentOpenResult::Opened;
		}
		return ELevelDocumentOpenResult::Rejected;
	}

	auto FLevelDocumentController::DrawDialogs() -> void
	{
		switch (QueuedPopup)
		{
		case EQueuedPopup::UnsavedLevel: ImGui::OpenPopup("Unsaved Level"); break;
		case EQueuedPopup::AssetStructureUpgrade: break;
		case EQueuedPopup::None: break;
		}
		QueuedPopup = EQueuedPopup::None;
		// Startup window placement may invalidate a popup opened during the first frame.
		// Pending compatibility state is authoritative, so keep the modal available until resolved.
		if (PendingUpgrade.IsPending() && !ImGui::IsPopupOpen("Asset Structure Upgrade Required"))
			ImGui::OpenPopup("Asset Structure Upgrade Required");
		DrawUnsavedLevelDialog();
		DrawAssetStructureUpgradeDialog();
	}

	auto FLevelDocumentController::DrawUnsavedLevelDialog() -> void
	{
		if (ImGui::BeginPopupModal("Unsaved Level", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings))
		{
			ImGui::TextUnformatted("The current level has unsaved changes.");
			if (ImGui::Button("Save"))
			{
				if (SaveCurrentLevel())
				{
					ImGui::CloseCurrentPopup();
					const bool bCompletesDeferredOpen = PendingAction == ELevelDocumentAction::OpenLevel;
					const ELevelDocumentOpenResult Result = ExecutePendingAction();
					if (bCompletesDeferredOpen && Result != ELevelDocumentOpenResult::Deferred)
						CompletePendingDocumentOpen(Result == ELevelDocumentOpenResult::Opened);
				}
			}
			ImGui::SameLine();
			if (ImGui::Button("Discard"))
			{
				ImGui::CloseCurrentPopup();
				const bool bCompletesDeferredOpen = PendingAction == ELevelDocumentAction::OpenLevel;
				const ELevelDocumentOpenResult Result = ExecutePendingAction();
				if (bCompletesDeferredOpen && Result != ELevelDocumentOpenResult::Deferred)
					CompletePendingDocumentOpen(Result == ELevelDocumentOpenResult::Opened);
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
			{
				const bool bCancelsDeferredOpen = PendingAction == ELevelDocumentAction::OpenLevel;
				PendingAction = ELevelDocumentAction::None;
				PendingLevelPath.clear();
				if (bCancelsDeferredOpen) CompletePendingDocumentOpen(false);
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}

	auto FLevelDocumentController::DrawAssetStructureUpgradeDialog() -> void
	{
		ImGui::SetNextWindowPos(
			ImGui::GetMainViewport()->GetCenter(),
			ImGuiCond_Appearing,
			ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSizeConstraints(
			ImVec2(MonaImGui::ScaleUI(620.0f), MonaImGui::ScaleUI(480.0f)),
			ImVec2(MonaImGui::ScaleUI(960.0f), MonaImGui::ScaleUI(760.0f)));
		if (!ImGui::BeginPopupModal(
			"Asset Structure Upgrade Required",
			nullptr,
			ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings))
			return;

		const Asset::FAssetLoadReport& PendingLoadReport = PendingUpgrade.GetReport();
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

		if (bHasRisk)
		{
			ImGui::Checkbox("I understand that saving will permanently discard the incompatible data.", &bCompatibilityDataLossConfirmed);
			ImGui::BeginDisabled(!bCompatibilityDataLossConfirmed);
			if (ImGui::Button("Discard Incompatible Data, Save and Open"))
			{
				const EAssetStructureUpgradeResult Result = ResolvePendingLevelUpgrade(
					EAssetStructureUpgradeDecision::DiscardIncompatibleDataSaveAndOpen);
				if (Result != EAssetStructureUpgradeResult::SaveFailed) ImGui::CloseCurrentPopup();
			}
			ImGui::EndDisabled();
		}
		else if (ImGui::Button("Upgrade, Save and Open"))
		{
			const EAssetStructureUpgradeResult Result =
				ResolvePendingLevelUpgrade(EAssetStructureUpgradeDecision::SaveAndOpen);
			if (Result != EAssetStructureUpgradeResult::SaveFailed) ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Open Without Saving"))
		{
			ResolvePendingLevelUpgrade(EAssetStructureUpgradeDecision::OpenWithoutSaving);
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel Open"))
		{
			ResolvePendingLevelUpgrade(EAssetStructureUpgradeDecision::Cancel);
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	auto FLevelDocumentController::OpenDefaultLevel() -> void
	{
		const FProjectInfo* Project = GetCurrentProject();
		if (!Project || DefaultLevel.empty() || !DefaultLevel.starts_with(Project->MountRoot)) return;
		OpenLevel(DefaultLevel);
	}

	auto FLevelDocumentController::OpenLevel(std::string_view PathString) -> ELevelDocumentOpenResult
	{
		if (ClearError) ClearError();
		FAssetPath Path;
		std::string PathError;
		if (!FAssetPath::TryCreate(PathString, Path, &PathError))
		{
			SetError(PathError);
			return ELevelDocumentOpenResult::Rejected;
		}
		DLevel* Level = nullptr;
		Asset::FAssetLoadReport LoadReport;
		Asset::FAssetResult Result = Asset::LoadAsset(Path, Level, &LoadReport);
		if (!Result)
		{
			SetError(Result.Message);
			return ELevelDocumentOpenResult::Rejected;
		}
		PublishWorkspaceLoadReport(LoadReport);
		if (LoadReport.HasCompatibilityIssues())
		{
			const bool bCompletesDeferredOpen = std::exchange(bPendingDocumentOpen, false);
			PendingUpgrade.Begin(Level, std::move(LoadReport), bCompletesDeferredOpen);
			bCompatibilityDataLossConfirmed = false;
			QueuedPopup = EQueuedPopup::AssetStructureUpgrade;
			return ELevelDocumentOpenResult::Deferred;
		}
		if (!ActivateLevel(Level))
		{
			Asset::FAssetResult UnloadResult = Asset::UnloadPackage(Path);
			if (!UnloadResult && UnloadResult.Error != Asset::EAssetError::NotFound)
				DURIN_WARN("Failed to unload level after activation failed: {}", UnloadResult.Message);
			return ELevelDocumentOpenResult::Rejected;
		}
		PendingAction = ELevelDocumentAction::None;
		return ELevelDocumentOpenResult::Opened;
	}

	auto FLevelDocumentController::ResolvePendingLevelUpgrade(
		EAssetStructureUpgradeDecision Decision) -> EAssetStructureUpgradeResult
	{
		FAssetStructureUpgradeOperations Operations{
			.Save = [this](DLevel* Level, bool bAllowCompatibilityDataLoss) {
				if (!Level || !Level->GetPackage())
				return SetError("The pending level is no longer available."), false;
				Asset::FAssetResult Result = Asset::SavePackage(
					Level->GetPackage(),
					{.bAllowCompatibilityDataLoss = bAllowCompatibilityDataLoss});
				if (Result)
				{
					InvalidateAssetUpgradeReport(PendingUpgrade.GetReport().PackagePath);
					return true;
				}
				SetError(Result.Message);
				return false;
			},
			.Activate = [this](DLevel* Level) { return ActivateLevel(Level); },
			.Unload = [this](const FAssetPath& Path) {
				Asset::FAssetResult Result = Asset::UnloadPackage(Path);
				if (!Result && Result.Error != Asset::EAssetError::NotFound)
					SetError(std::format("Could not unload the pending level: {}", Result.Message));
			},
			.CompleteDeferredOpen = [this](bool bSucceeded) {
				if (CompleteDeferredOpen) CompleteDeferredOpen(bSucceeded);
			}};
		const EAssetStructureUpgradeResult Result = PendingUpgrade.Resolve(Decision, Operations);
		if (Result != EAssetStructureUpgradeResult::SaveFailed)
		{
			PendingAction = ELevelDocumentAction::None;
			PendingLevelPath.clear();
			bCompatibilityDataLossConfirmed = false;
		}
		return Result;
	}

	auto FLevelDocumentController::CompletePendingDocumentOpen(bool bSucceeded) -> void
	{
		if (!std::exchange(bPendingDocumentOpen, false) || !CompleteDeferredOpen) return;
		CompleteDeferredOpen(bSucceeded);
	}

	auto FLevelDocumentController::SaveCurrentLevel() -> bool
	{
		if (ClearError) ClearError();
		if (!Context.Level || !Context.Level->GetPackage())
		{
			SetError("The current level is transient and cannot be saved.");
			return false;
		}
		SessionSettings.CaptureViewportState(Context, SceneViewportPanel);
		SessionSettings.Save(&SceneViewportPanel);
		Asset::FAssetResult Result = Asset::SavePackage(Context.Level->GetPackage());
		FLevelDocumentRevisionState::CompleteSave(
			GetLevelTransactions(), *Context.Level->GetPackage(), static_cast<bool>(Result)
		);
		if (!Result)
		{
			SetError(Result.Message);
			return false;
		}
		FAssetPath SavedPath;
		if (FAssetPath::TryCreate(Context.Level->GetPackage()->GetPackagePath(), SavedPath))
			InvalidateAssetUpgradeReport(SavedPath);
		return true;
	}

	auto FLevelDocumentController::RenameCurrentLevel(std::string_view NewName) -> bool
	{
		if (ClearError) ClearError();
		if (!Context.Level)
		{
			SetError("No level is open.");
			return false;
		}
		DPackage* Package = Context.Level->GetPackage();
		if (!Package || !Package->IsAssetPackage())
		{
			SetError("Transient levels cannot be renamed as assets.");
			return false;
		}
		if (NewName.empty())
		{
			SetError("Level name cannot be empty.");
			return false;
		}
		if (NewName.find_first_of("/\\") != std::string_view::npos)
		{
			SetError("Level name cannot contain path separators.");
			return false;
		}

		FAssetPath OldPath;
		std::string PathError;
		if (!FAssetPath::TryCreate(Package->GetPackagePath(), OldPath, &PathError))
		{
			SetError(PathError);
			return false;
		}
		const std::string OldPathString = OldPath.ToString();
		const size_t Separator = OldPathString.find_last_of('/');
		const std::string NewPathString = OldPathString.substr(0, Separator + 1) + std::string(NewName);
		FAssetPath NewPath;
		if (!FAssetPath::TryCreate(NewPathString, NewPath, &PathError))
		{
			SetError(PathError);
			return false;
		}

		if (OldPath == NewPath)
		{
			if (Context.Level->GetName() == NewName) return true;
			const FName OldObjectName = Context.Level->GetFName();
			const bool bWasDirty = Package->IsDirty();
			Context.Level->Rename(FName(NewName));
			const Asset::FAssetResult SaveResult = Asset::SavePackage(Package);
			FLevelDocumentRevisionState::CompleteSave(
				GetLevelTransactions(), *Package, static_cast<bool>(SaveResult)
			);
			if (!SaveResult)
			{
				Context.Level->Rename(OldObjectName);
				if (!bWasDirty) Package->ClearDirty();
				SetError(SaveResult.Message);
				return false;
			}
			InvalidateAssetUpgradeReport(OldPath);
			return true;
		}

		const Asset::FAssetResult MoveResult = AssetMoveCoordinator.MoveAsset(OldPath, NewPath);
		FLevelDocumentRevisionState::CompleteSave(
			GetLevelTransactions(), *Package, static_cast<bool>(MoveResult)
		);
		if (!MoveResult)
		{
			SetError(MoveResult.Message);
			return false;
		}
		InvalidateAssetUpgradeReport(OldPath);
		return true;
	}

	auto FLevelDocumentController::ActivateLevel(DLevel* Level) -> bool
	{
		if (!Context.World || !Level)
		{
			SetError("No world is available to activate the level.");
			return false;
		}
		SessionSettings.CaptureViewportState(Context, SceneViewportPanel);
		SessionSettings.Save(&SceneViewportPanel);
		DLevel* Previous = Context.World->GetCurrentLevel();
		DPackage* PreviousPackage = Previous ? Previous->GetPackage() : nullptr;
		if (!Context.World->SetCurrentLevel(Level))
		{
			SetError("The level is already active in another world.");
			return false;
		}
		FLevelDocumentRevisionState::Activate(GetLevelTransactions(), Level->GetPackage());
		Context.Synchronize(Context.World);
		SessionSettings.RestoreViewportState(Level, SceneViewportPanel);
		if (PreviousPackage && PreviousPackage != Level->GetPackage())
		{
			FAssetPath PreviousPath;
			if (FAssetPath::TryCreate(PreviousPackage->GetPackagePath(), PreviousPath))
			{
				Asset::FAssetResult Result = Asset::UnloadPackage(PreviousPath);
				if (!Result && Result.Error != Asset::EAssetError::NotFound) DURIN_WARN("Failed to unload previous level: {}", Result.Message);
			}
		}
		return true;
	}

	auto FLevelDocumentController::SetError(std::string Message) const -> void
	{
		if (ReportError) ReportError(std::move(Message));
	}
} // namespace Durin
