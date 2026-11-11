#include "Assets/SceneImportDialog.h"

#include "Import/AssetDestinationValidation.h"
#include "Asset/Asset.h"
#include "Asset/AssetPicker.h"
#include "Materials/Material.h"
#include "Dialogs/FileDialog.h"
#include "Misc/Project.h"
#include "Misc/StringConvert.h"
#include "Misc/StringHelper.h"
#include "MonaImGui.h"
#include "Feedback/SlowTaskWidget.h"
#include "AssetForge/Builtins/ImportFeedback.h"

namespace Durin::LevelEditor
{
	FSceneImportDialog::FSceneImportDialog(FImportDialogCallbacks InCallbacks)
		: Callbacks(std::move(InCallbacks)) {}

	auto FSceneImportDialog::Open(std::string_view InDestinationDirectory) -> void
	{
		if (bImporting) return;
		if (Session && Session->GetProgress().Phase != AssetForge::Builtins::EAssetImportPhase::Ready
			&& Session->GetProgress().Phase != AssetForge::Builtins::EAssetImportPhase::Completed) return;
		Session.reset();
		bImporting = bReportedCompletion = bCloseWhenFinished = false;
		SourcePathBuffer.fill(0);
		Coordinates.Reset();
		MaterialOptions = {};
		OutputPreview = {};
		bPreviewDirty = true;
		DestinationDirectory.Reset(InDestinationDirectory);
		ModalState.RequestOpen();
	}

	auto FSceneImportDialog::Draw(bool bAllowAssetMutation) -> void
	{
		using AssetForge::Builtins::EAssetImportPhase;
		if (Session) Session->Tick();
		ModalState.OpenPopupIfRequested("Import Scene Source");
		const MonaImGui::FUIStyleMetrics Metrics = MonaImGui::GetUIStyleMetrics();
		ImGui::SetNextWindowSize(ImVec2(Metrics.WidePopupWidth, 0.0f), ImGuiCond_Appearing);
		if (!ImGui::BeginPopupModal("Import Scene Source", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize
				| ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) return;
		if (Session && Session->GetProgress().Phase == EAssetImportPhase::Completed && !bReportedCompletion)
		{
			bReportedCompletion = true;
			bImporting = false;
			const auto& Result = Session->GetResult();
			if (!Result.ImportedPackages.empty()) Callbacks.NotifyImportedDirectory(DestinationDirectory.GetPath());
			if (!Result && (!bCloseWhenFinished || !Result.ImportedPackages.empty()))
				SetError(Result.ImportedPackages.empty() ? Result.Message
					: std::format("{} package(s) imported before import stopped. {}", Result.ImportedPackages.size(), Result.Message));
			if (Result || bCloseWhenFinished)
			{
				ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
				return;
			}
			bImporting = false;
		}
		if (Session && Session->GetProgress().Phase != EAssetImportPhase::Ready
			&& Session->GetProgress().Phase != EAssetImportPhase::Completed)
		{
			const auto Progress = AssetForge::Builtins::MakeAssetImportFeedback(Session->GetProgress(), SourcePathBuffer.data());
			if (DrawSlowTaskProgress(Progress, true, false))
			{
				Session->Cancel();
				bCloseWhenFinished = true;
			}
			ImGui::EndPopup();
			return;
		}

		ImGui::TextUnformatted("Import the assets described by an FBX Scene source.");
		ImGui::TextWrapped("One-time import. Reimport is unavailable; use a new destination for later imports.");
		ImGui::TextDisabled("Outputs are peer assets grouped by type inside one destination directory.");
		ImGui::Spacing();
		ImGui::SeparatorText("Source model");
		ImGui::TextDisabled("The selected source and its relative dependencies remain in place.");
		const float BrowseButtonWidth = Metrics.StandardButtonWidth;
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x
			- BrowseButtonWidth - ImGui::GetStyle().ItemSpacing.x);
		ImGui::InputTextWithHint("##SceneImportSource",
			"Choose an FBX Scene source...", SourcePathBuffer.data(),
			SourcePathBuffer.size(), ImGuiInputTextFlags_ReadOnly);
		ImGui::SameLine();
		if (ImGui::Button("Browse...", ImVec2(BrowseButtonWidth, 0.0f))) BrowseSource();

		const std::filesystem::path SourcePath(SourcePathBuffer.data());
		const bool bHasSource = SourcePathBuffer[0] != '\0';
		const bool bSourceExists = bHasSource && std::filesystem::is_regular_file(SourcePath);
		const std::string Extension = StringUtils::FoldAscii(
			SourcePath.extension().generic_string());
		const bool bSupportedSource = Extension == ".fbx";
		if (bHasSource) ImGui::TextDisabled("%s", SourcePath.filename().generic_string().c_str());

		ImGui::Spacing();
		ImGui::SeparatorText("Coordinate system");
		ImGui::BeginGroup();
		Coordinates.Draw();
		ImGui::EndGroup();
		if (ImGui::IsItemEdited()) { bPreviewDirty = true; Session.reset(); }
		ImGui::Spacing();
		ImGui::SeparatorText("Destination");
		const std::string PreviousDirectory(DestinationDirectory.GetPath());
		if (DestinationDirectory.DrawRow("Output directory", "##SceneImportDirectory",
			"/Project/Imported/SceneName", "Choose...", BrowseButtonWidth))
			BrowseDestinationDirectory();
		if (PreviousDirectory != DestinationDirectory.GetPath()) bPreviewDirty = true;
		const FContentDirectoryValidation DestinationValidation = DestinationDirectory.Inspect();
		const auto SettingsValidation = Coordinates.GetSettings().Validate();
		const bool bImportSettingsValid = SettingsValidation.has_value();
		const std::string ImportSettingsError = SettingsValidation ? std::string{} : FormatStaticMeshImportSettingsError(SettingsValidation.error());
		if (!Session && bSourceExists && bSupportedSource && bImportSettingsValid && DestinationValidation)
		{
			Session = std::make_unique<AssetForge::Builtins::FAssetImportSession>(
				SourcePathBuffer.data(), DestinationValidation.DirectoryPath, Coordinates.GetSettings(),
				AssetForge::Builtins::FAssetImportOptions{.MeshMode = AssetForge::Builtins::EAssetImportMeshMode::Combined});
			bReportedCompletion = bImporting = false;
		}
		DrawMaterials(DestinationValidation.DirectoryPath,
			bAllowAssetMutation && DestinationValidation && bSourceExists && bSupportedSource && bImportSettingsValid);

		if (DestinationValidation.bDirectoryPathValid
			&& DestinationValidation.bMountedDestination && bSourceExists && bSupportedSource)
		{
			ImGui::BeginChild("SceneImportOutputPreview",
				ImVec2(0.0f, MonaImGui::ScaleUI(112.0f)), ImGuiChildFlags_Borders);
			ImGui::TextDisabled("Source filename");
			ImGui::TextUnformatted(SourcePath.generic_string().c_str());
			ImGui::TextDisabled("Output directory");
			ImGui::TextUnformatted(DestinationValidation.DirectoryPath.ToString().c_str());
			for (const auto& Output : OutputPreview.Outputs)
				ImGui::TextWrapped("%s: %s", Output.Role.c_str(), Output.AssetPath.ToString().c_str());
			ImGui::EndChild();
		}

		std::string ValidationMessage;
		if (!bHasSource) ValidationMessage = "Select a source model to continue.";
		else if (!bSourceExists) ValidationMessage = "The selected source file no longer exists.";
		else if (!bSupportedSource) ValidationMessage = "Scene import supports FBX files. Use glTF / GLB Assets for glTF sources.";
		else if (!bImportSettingsValid) ValidationMessage = ImportSettingsError;
		else if (!DestinationValidation) ValidationMessage = FormatContentDirectoryValidation(DestinationValidation);
		else if (!Session || Session->GetProgress().Phase != EAssetImportPhase::Ready) ValidationMessage = "Scene preparation is not ready.";
		else if (bPreviewDirty) ValidationMessage = "Validating import outputs...";
		else if (!OutputPreview.bSucceeded) ValidationMessage = OutputPreview.Message;
		DrawImportDialogWarning(ValidationMessage);
		ImGui::TextWrapped("Model imports create new assets. Choose an unoccupied output directory.");

		ImGui::Spacing();
		ImGui::Separator();
		if (!bAllowAssetMutation) DrawImportDialogWarning("Asset imports are unavailable during Play.");
		ImGui::BeginDisabled(!bAllowAssetMutation || !ValidationMessage.empty());
		if (ImGui::Button("Import Scene", ImVec2(MonaImGui::ScaleUI(150.0f), 0.0f))
			&& !bImporting) (void)Import();
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (MonaImGui::DialogButton("Cancel", true))
		{
			if (Session) Session->Cancel();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	auto FSceneImportDialog::BrowseSource() -> void
	{
		FFileDialogRequest Request;
		Request.ParentWindowHandle = ImGui::GetMainViewport()->PlatformHandleRaw;
		Request.Title = "Select an FBX Scene Source";
		Request.Filters = {{"Autodesk FBX", "*.fbx"}, {"All Files", "*.*"}};
		if (const FProjectInfo* Project = GetCurrentProject())
			Request.InitialDirectory = Project->ProjectDir;
		if (SourcePathBuffer[0] != '\0')
			Request.InitialDirectory = std::filesystem::path(
				SourcePathBuffer.data()).parent_path().generic_string();
		const FFileDialogResult Result = OpenFileDialog(Request);
		if (Result.Status == EFileDialogStatus::Cancelled) return;
		if (Result.Status == EFileDialogStatus::Error) { SetError(Result.ErrorMessage); return; }
		if (Result.FilePath.size() >= SourcePathBuffer.size())
		{
			SetError("The selected file path is too long for the import form.");
			return;
		}
		SourcePathBuffer.fill(0);
		Session.reset();
		bImporting = bReportedCompletion = bCloseWhenFinished = false;
		std::memcpy(SourcePathBuffer.data(), Result.FilePath.data(),
			std::min(Result.FilePath.size(), SourcePathBuffer.size() - 1));
		Coordinates.Reset();
		MaterialOptions.Overrides.clear();
		OutputPreview = {};
		bPreviewDirty = true;
		const std::string SceneName = StringUtils::SanitizeFileName(
			std::filesystem::path(Result.FilePath).stem().generic_string(), "Scene");
		const FProjectInfo* Project = GetCurrentProject();
		DestinationDirectory.SuggestPath(DestinationDirectory.MakeSuggestedPath(
			SceneName, (Project ? Project->MountRoot : "/") + std::string("Imported/")));
	}

	auto FSceneImportDialog::BrowseDestinationDirectory() -> void
	{
		(void)DestinationDirectory.Browse("Choose a Scene Output Directory",
			"The selected directory path is too long for the import form.",
			"Scene outputs must be saved inside a package-enabled mount.", Callbacks);
		bPreviewDirty = true;
	}

	auto FSceneImportDialog::Import() -> bool
	{
		const FContentDirectoryValidation DestinationValidation =
			DestinationDirectory.Inspect();
		if (!DestinationValidation)
		{
			SetError(FormatContentDirectoryValidation(DestinationValidation));
			return false;
		}
		const FPackagePath& OutputDirectory = DestinationValidation.DirectoryPath;
		bImporting = Session && Session->BeginImport(OutputDirectory, MaterialOptions);
		return bImporting;
	}

	auto FSceneImportDialog::DrawMaterials(const FPackagePath& Directory, bool bCanPreview) -> void
	{
		using namespace AssetForge::Builtins;
		ImGui::SeparatorText("Materials");
		const auto DrawSelection = [&](FSceneMaterialSelection& Selection) {
			int Mode = static_cast<int>(Selection.Mode);
			if (ImGui::Combo("Mode", &Mode, "Create Materials\0Create Material Instances\0"))
			{
				Selection.Mode = static_cast<ESceneMaterialImportMode>(Mode);
				bPreviewDirty = true;
			}
			if (Selection.Mode == ESceneMaterialImportMode::CreateInstances)
			{
				ImGui::TextUnformatted("Parent material");
				const auto Picker = AssetPicker::Draw({
					.RequiredClass = DMaterial::StaticClass(),
					.ClassPolicy = EAssetClassPolicy::Exact,
					.AssignmentMode = EAssetAssignmentMode::AssetPath,
					.CurrentSelectionPath = Selection.ParentMaterialPath,
					.SearchText = ParentSearch,
					.AssignPathSelection = [&](std::string_view Path, std::string&) {
						Selection.ParentMaterialPath = Path;
						bPreviewDirty = true;
						return true;
					}});
				if (!Picker.Error.empty()) SetError(Picker.Error);
				ImGui::TextDisabled("Mapping: standard PBR or compatible imported PBR (automatic)");
			}
		};
		DrawSelection(MaterialOptions.Default);
		if (!OutputPreview.Materials.empty())
		{
			ImGui::BeginChild("SceneMaterials", ImVec2(0, MonaImGui::ScaleUI(240)), ImGuiChildFlags_Borders);
			for (const auto& Row : OutputPreview.Materials)
			{
				ImGui::PushID(Row.StableIdentity.c_str());
				ImGui::SeparatorText(Row.SourceName.empty() ? "Unnamed material" : Row.SourceName.c_str());
				ImGui::TextWrapped("%s", Row.AssetPath.ToString().c_str());
				ImGui::TextWrapped("%s", bPreviewDirty ? "Selections changed; refresh preview." : Row.Message.c_str());
				if (Row.bPreserved)
				{
					ImGui::TextDisabled("Existing %s", Row.Selection.Mode == ESceneMaterialImportMode::CreateInstances ? "Material Instance" : "Material");
					if (!Row.Selection.ParentMaterialPath.empty()) ImGui::TextWrapped("Parent: %s", Row.Selection.ParentMaterialPath.c_str());
				}
				else
				{
					auto Override = std::ranges::find(MaterialOptions.Overrides, Row.StableIdentity, &FSceneMaterialOverride::StableIdentity);
					bool bOverride = Override != MaterialOptions.Overrides.end();
					if (ImGui::Checkbox("Override default", &bOverride))
					{
						if (bOverride)
						{
							MaterialOptions.Overrides.push_back({Row.StableIdentity, MaterialOptions.Default});
							Override = std::prev(MaterialOptions.Overrides.end());
						}
						else MaterialOptions.Overrides.erase(Override);
						bPreviewDirty = true;
					}
					if (bOverride) DrawSelection(Override->Selection);
				}
				ImGui::PopID();
			}
			ImGui::EndChild();
		}
		if (bCanPreview && bPreviewDirty && Session
			&& Session->GetProgress().Phase == EAssetImportPhase::Ready)
		{
			OutputPreview = Session->PreviewOutputs(Directory, MaterialOptions);
			bPreviewDirty = false;
		}
		if (Session && Session->GetProgress().Phase == EAssetImportPhase::Completed && ImGui::Button("Retry preparation"))
		{
			Session.reset();
			bPreviewDirty = true;
		}
	}

	auto FSceneImportDialog::SetError(std::string Message) const -> void
	{
		Callbacks.Report(std::move(Message));
	}
} // namespace Durin::LevelEditor
