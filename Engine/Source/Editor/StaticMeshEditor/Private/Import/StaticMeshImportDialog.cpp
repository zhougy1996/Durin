#include "Import/StaticMeshImportDialog.h"

#include "Editor/Import/AssetDestinationValidation.h"
#include "Asset.h"
#include "Dialogs/FileDialog.h"
#include "Misc/Paths.h"
#include "Misc/Project.h"
#include "Misc/StringConvert.h"
#include "MonaImGui.h"
#include "AssetForge/Builtins/StaticMeshImport.h"

namespace Durin::Editor::StaticMesh
{
	namespace
	{
		auto Lowercase(std::string Value) -> std::string
		{
			std::ranges::transform(Value, Value.begin(), [](unsigned char Character) {
				return static_cast<char>(std::tolower(Character));
			});
			return Value;
		}

		auto IsSupportedModelExtension(std::string_view Extension) -> bool
		{
			const std::string Folded = Lowercase(std::string(Extension));
			return Folded == ".obj" || Folded == ".fbx" || Folded == ".gltf"
				|| Folded == ".glb" || Folded == ".dae" || Folded == ".3ds"
				|| Folded == ".ply" || Folded == ".stl";
		}

	} // namespace

	FStaticMeshImportDialog::FStaticMeshImportDialog(
		FImportDialogCallbacks InCallbacks)
		: Callbacks(std::move(InCallbacks))
	{
	}

	auto FStaticMeshImportDialog::Open(std::string_view DestinationDirectory) -> void
	{
		SourcePathBuffer.fill(0);
		Coordinates.Reset();
		Destination.Reset(DestinationDirectory);
		ModalState.RequestOpen();
	}

	auto FStaticMeshImportDialog::Draw(bool bAllowAssetMutation) -> void
	{
		ModalState.OpenPopupIfRequested("Import Static Mesh");

		const MonaImGui::FUIStyleMetrics Metrics = MonaImGui::GetUIStyleMetrics();
		ImGui::SetNextWindowSize(ImVec2(Metrics.WidePopupWidth, 0.0f), ImGuiCond_Appearing);
		if (!ImGui::BeginPopupModal("Import Static Mesh", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize
				| ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings))
			return;

		ImGui::TextUnformatted("Create one geometry-only StaticMesh from a model file.");
		ImGui::TextDisabled("Materials and textures are not created; use Scene Source for a complete FBX or glTF scene.");
		ImGui::Spacing();
		ImGui::SeparatorText("Source model");
		ImGui::TextDisabled(
			"The selected model remains in place and is retained as a source filename.");
		const float BrowseButtonWidth = Metrics.StandardButtonWidth;
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x
			- BrowseButtonWidth - ImGui::GetStyle().ItemSpacing.x);
		ImGui::InputTextWithHint("##StaticMeshImportSource",
			"Choose an OBJ or another supported model...", SourcePathBuffer.data(),
			SourcePathBuffer.size(), ImGuiInputTextFlags_ReadOnly);
		ImGui::SameLine();
		if (ImGui::Button("Browse...", ImVec2(BrowseButtonWidth, 0.0f))) BrowseSource();

		const std::filesystem::path SourcePath(SourcePathBuffer.data());
		const bool bHasSource = SourcePathBuffer[0] != '\0';
		const bool bSourceExists = bHasSource && std::filesystem::is_regular_file(SourcePath);
		const bool bSupportedSource = bHasSource
			&& IsSupportedModelExtension(SourcePath.extension().generic_string());
		if (bHasSource) ImGui::TextDisabled("%s", SourcePath.filename().generic_string().c_str());

		ImGui::Spacing();
		ImGui::SeparatorText("Coordinate system");
		Coordinates.Draw();

		ImGui::Spacing();
		ImGui::SeparatorText("Destination");
		if (Destination.DrawRow("Asset path (one .dasset)", "##StaticMeshImportAssetPath",
			"/Project/StaticMeshes/AssetName", "Choose...", BrowseButtonWidth))
			BrowseDestination();
		const FAssetDestinationValidation DestinationValidation = Destination.Inspect();
		std::string ImportSettingsError;
		const bool bImportSettingsValid = Coordinates.GetSettings().IsValid(&ImportSettingsError);

		if (DestinationValidation.bAssetPathValid
			&& DestinationValidation.bMountedDestination && bSourceExists
			&& bSupportedSource)
		{
			ImGui::BeginChild("StaticMeshImportOutputPreview",
				ImVec2(0.0f, MonaImGui::ScaleUI(112.0f)), ImGuiChildFlags_Borders);
			ImGui::TextDisabled("Asset identity");
			ImGui::TextUnformatted(DestinationValidation.AssetPath.ToString().c_str());
			ImGui::TextDisabled("Source filename");
			ImGui::TextUnformatted(SourcePath.generic_string().c_str());
			ImGui::EndChild();
		}

		std::string ValidationMessage;
		if (!bHasSource) ValidationMessage = "Select a source model to continue.";
		else if (!bSourceExists) ValidationMessage = "The selected source file no longer exists.";
		else if (!bSupportedSource)
			ValidationMessage = "Supported model formats are OBJ, FBX, glTF, COLLADA, 3DS, PLY, and STL.";
		else if (!bImportSettingsValid) ValidationMessage = ImportSettingsError;
		else if (!DestinationValidation) ValidationMessage = DestinationValidation.Message;

		DrawImportDialogWarning(ValidationMessage);
		if (!bAllowAssetMutation)
			DrawImportDialogWarning("Asset imports are unavailable during Play.");
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::BeginDisabled(!bAllowAssetMutation || !ValidationMessage.empty());
		if (ImGui::Button("Import Static Mesh",
			ImVec2(MonaImGui::ScaleUI(150.0f), 0.0f)) && Import())
			ImGui::CloseCurrentPopup();
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (MonaImGui::DialogButton("Cancel", true)) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	auto FStaticMeshImportDialog::BrowseSource() -> void
	{
		FFileDialogRequest Request;
		Request.ParentWindowHandle = ImGui::GetMainViewport()->PlatformHandleRaw;
		Request.Title = "Select a Static Mesh Source File";
		Request.Filters = {
			{"All Supported Models", "*.obj;*.fbx;*.gltf;*.glb;*.dae;*.3ds;*.ply;*.stl"},
			{"Wavefront OBJ", "*.obj"}, {"Autodesk FBX", "*.fbx"},
			{"glTF", "*.gltf;*.glb"}, {"COLLADA", "*.dae"},
			{"PLY", "*.ply"}, {"STL", "*.stl"}, {"All Files", "*.*"}
		};
		if (const FProjectInfo* Project = GetCurrentProject())
			Request.InitialDirectory = Project->ProjectDir;
		if (SourcePathBuffer[0] != '\0')
			Request.InitialDirectory = std::filesystem::path(SourcePathBuffer.data())
				.parent_path().generic_string();
		const FFileDialogResult Result = OpenFileDialog(Request);
		if (Result.Status == EFileDialogStatus::Cancelled) return;
		if (Result.Status == EFileDialogStatus::Error)
		{
			SetError(Result.ErrorMessage);
			return;
		}
		if (Result.FilePath.size() >= SourcePathBuffer.size())
		{
			SetError("The selected file path is too long for the import form.");
			return;
		}

		SourcePathBuffer.fill(0);
		std::memcpy(SourcePathBuffer.data(), Result.FilePath.data(), Result.FilePath.size());
		if (Lowercase(std::filesystem::path(Result.FilePath).extension().generic_string()) == ".obj")
		{
			Coordinates.SetPreset(
				FMeshCoordinateImportModel::EPreset::YUpNegativeZForward);
		}
		else
		{
			Coordinates.SetPreset(FMeshCoordinateImportModel::EPreset::Durin);
		}
		const std::string AssetName = StringUtils::SanitizeFileName(
			std::filesystem::path(Result.FilePath).stem().generic_string(), "StaticMesh");
		const FProjectInfo* Project = GetCurrentProject();
		Destination.SuggestPath(Destination.MakeSuggestedPath(AssetName,
			(Project ? Project->MountRoot : "/") + std::string("StaticMeshes/")));
	}

	auto FStaticMeshImportDialog::BrowseDestination() -> void
	{
		const std::string DefaultFileName = SourcePathBuffer[0] != '\0'
			? StringUtils::SanitizeFileName(
				std::filesystem::path(SourcePathBuffer.data()).stem().generic_string(),
				"StaticMesh") + ".dasset"
			: "StaticMesh.dasset";
		Destination.Browse("Choose a Static Mesh Asset Path", DefaultFileName,
			"The selected asset path is too long for the import form.",
			"Static mesh assets must be saved inside a package-enabled mount.", Callbacks);
	}

	auto FStaticMeshImportDialog::Import() -> bool
	{
		Callbacks.Clear();
		const FAssetDestinationValidation DestinationValidation =
			Destination.Inspect();
		if (!DestinationValidation)
		{
			SetError(DestinationValidation.Message);
			return false;
		}
		const FAssetPath& AssetPath = DestinationValidation.AssetPath;
		const FStaticMeshImportResult Result =
			AssetForge::Builtins::ImportStaticMeshAsset(
				SourcePathBuffer.data(), AssetPath.ToString(), Coordinates.GetSettings());
		if (!Result)
		{
			SetError(Result.Message.empty()
				? "StaticMesh import failed." : Result.Message);
			return false;
		}
		Callbacks.NotifyImported(AssetPath.ToString());
		Asset::UnloadPackage(AssetPath);
		return true;
	}

	auto FStaticMeshImportDialog::SetError(std::string Message) const -> void
	{
		Callbacks.Report(std::move(Message));
	}
} // namespace Durin::Editor::StaticMesh
