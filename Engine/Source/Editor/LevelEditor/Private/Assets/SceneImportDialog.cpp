#include "Assets/SceneImportDialog.h"

#include "Editor/Import/AssetDestinationValidation.h"
#include "Asset/Asset.h"
#include "Dialogs/FileDialog.h"
#include "Misc/Project.h"
#include "Misc/StringConvert.h"
#include "Misc/StringHelper.h"
#include "MonaImGui.h"

namespace Durin::Editor::Level
{
	FSceneImportDialog::FSceneImportDialog(FImportDialogCallbacks InCallbacks)
		: Callbacks(std::move(InCallbacks)) {}

	auto FSceneImportDialog::Open(std::string_view InDestinationDirectory) -> void
	{
		SourcePathBuffer.fill(0);
		Coordinates.Reset();
		DestinationDirectory.Reset(InDestinationDirectory);
		std::string Error;
		if (!AssetForge::Builtins::EnsureImportedSurfaceMaterial(Error))
			SetError(std::move(Error));
		ModalState.RequestOpen();
	}

	auto FSceneImportDialog::Draw(bool bAllowAssetMutation) -> void
	{
		ModalState.OpenPopupIfRequested("Import Scene Source");
		const MonaImGui::FUIStyleMetrics Metrics = MonaImGui::GetUIStyleMetrics();
		ImGui::SetNextWindowSize(ImVec2(Metrics.WidePopupWidth, 0.0f), ImGuiCond_Appearing);
		if (!ImGui::BeginPopupModal("Import Scene Source", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize
				| ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) return;

		ImGui::TextUnformatted("Import the assets described by an FBX or glTF Scene source.");
		ImGui::TextDisabled("Outputs are peer assets grouped by type inside one destination directory.");
		ImGui::Spacing();
		ImGui::SeparatorText("Source model");
		ImGui::TextDisabled("The selected source and its relative dependencies remain in place.");
		const float BrowseButtonWidth = Metrics.StandardButtonWidth;
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x
			- BrowseButtonWidth - ImGui::GetStyle().ItemSpacing.x);
		ImGui::InputTextWithHint("##SceneImportSource",
			"Choose an FBX, glTF, or GLB Scene source...", SourcePathBuffer.data(),
			SourcePathBuffer.size(), ImGuiInputTextFlags_ReadOnly);
		ImGui::SameLine();
		if (ImGui::Button("Browse...", ImVec2(BrowseButtonWidth, 0.0f))) BrowseSource();

		const std::filesystem::path SourcePath(SourcePathBuffer.data());
		const bool bHasSource = SourcePathBuffer[0] != '\0';
		const bool bSourceExists = bHasSource && std::filesystem::is_regular_file(SourcePath);
		const std::string Extension = StringUtils::FoldAscii(
			SourcePath.extension().generic_string());
		const bool bSupportedSource = Extension == ".fbx"
			|| Extension == ".gltf" || Extension == ".glb";
		if (bHasSource) ImGui::TextDisabled("%s", SourcePath.filename().generic_string().c_str());

		ImGui::Spacing();
		ImGui::SeparatorText("Coordinate system");
		Coordinates.Draw();
		ImGui::Spacing();
		ImGui::SeparatorText("Destination");
		if (DestinationDirectory.DrawRow("Output directory", "##SceneImportDirectory",
			"/Project/Imported/SceneName", "Choose...", BrowseButtonWidth))
			BrowseDestinationDirectory();
		const FContentDirectoryValidation DestinationValidation = DestinationDirectory.Inspect();
		std::string ImportSettingsError;
		const bool bImportSettingsValid = Coordinates.GetSettings().IsValid(&ImportSettingsError);

		if (DestinationValidation.bDirectoryPathValid
			&& DestinationValidation.bMountedDestination && bSourceExists && bSupportedSource)
		{
			ImGui::BeginChild("SceneImportOutputPreview",
				ImVec2(0.0f, MonaImGui::ScaleUI(112.0f)), ImGuiChildFlags_Borders);
			ImGui::TextDisabled("Source filename");
			ImGui::TextUnformatted(SourcePath.generic_string().c_str());
			ImGui::TextDisabled("Output directory");
			ImGui::TextUnformatted(DestinationValidation.DirectoryPath.ToString().c_str());
			ImGui::EndChild();
		}

		std::string ValidationMessage;
		if (!bHasSource) ValidationMessage = "Select a source model to continue.";
		else if (!bSourceExists) ValidationMessage = "The selected source file no longer exists.";
		else if (!bSupportedSource) ValidationMessage = "Scene import supports FBX, glTF, and GLB files.";
		else if (!bImportSettingsValid) ValidationMessage = ImportSettingsError;
		else if (!DestinationValidation) ValidationMessage = DestinationValidation.Message;
		DrawImportDialogWarning(ValidationMessage);

		ImGui::Spacing();
		ImGui::Separator();
		if (!bAllowAssetMutation) DrawImportDialogWarning("Asset imports are unavailable during Play.");
		ImGui::BeginDisabled(!bAllowAssetMutation || !ValidationMessage.empty());
		if (ImGui::Button("Import Scene", ImVec2(MonaImGui::ScaleUI(150.0f), 0.0f))
			&& Import()) ImGui::CloseCurrentPopup();
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (MonaImGui::DialogButton("Cancel", true)) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	auto FSceneImportDialog::BrowseSource() -> void
	{
		FFileDialogRequest Request;
		Request.ParentWindowHandle = ImGui::GetMainViewport()->PlatformHandleRaw;
		Request.Title = "Select an FBX or glTF Scene Source";
		Request.Filters = {{"Supported Scene Sources", "*.fbx;*.gltf;*.glb"},
			{"Autodesk FBX", "*.fbx"}, {"glTF", "*.gltf;*.glb"}, {"All Files", "*.*"}};
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
		std::memcpy(SourcePathBuffer.data(), Result.FilePath.data(),
			std::min(Result.FilePath.size(), SourcePathBuffer.size() - 1));
		Coordinates.Reset();
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
	}

	auto FSceneImportDialog::Import() -> bool
	{
		const FContentDirectoryValidation DestinationValidation =
			DestinationDirectory.Inspect();
		if (!DestinationValidation)
		{
			SetError(DestinationValidation.Message);
			return false;
		}
		const FPackagePath& OutputDirectory = DestinationValidation.DirectoryPath;
		AssetForge::Builtins::FSceneImportResult Result;
		if (!AssetForge::Builtins::ImportSceneAssets(SourcePathBuffer.data(), OutputDirectory,
			Coordinates.GetSettings(), Result))
		{
			SetError(Result.Message.empty() ? "Scene import failed." : std::move(Result.Message));
			return false;
		}
		Callbacks.NotifyImportedDirectory(DestinationDirectory.GetPath());
		for (const AssetForge::FImportOutputSummary& Output : Result.Outputs)
			UnloadPackage(Output.AssetPath);
		return true;
	}

	auto FSceneImportDialog::SetError(std::string Message) const -> void
	{
		Callbacks.Report(std::move(Message));
	}
} // namespace Durin::Editor::Level
