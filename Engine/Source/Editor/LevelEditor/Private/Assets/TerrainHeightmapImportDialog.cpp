#include "Assets/TerrainHeightmapImportDialog.h"

#include "AssetTools/IAssetTools.h"
#include "AssetForge/Builtins/TerrainHeightmapFactory.h"
#include "Asset/AssetOperations.h"
#include "Editor/Import/AssetDestinationValidation.h"
#include "Asset/Asset.h"
#include "Dialogs/FileDialog.h"
#include "Misc/Project.h"
#include "Misc/StringConvert.h"
#include "Misc/StringHelper.h"
#include "MonaImGui.h"
#include "Terrain/TerrainHeightmap.h"
#include "Terrain/TerrainHeightmapBuildOperations.h"
#include "AssetForge/Builtins/TerrainHeightmapImport.h"

namespace Durin::Editor::Level
{
	FTerrainHeightmapImportDialog::FTerrainHeightmapImportDialog(
		FImportDialogCallbacks InCallbacks)
		: Callbacks(std::move(InCallbacks)) {}

	auto FTerrainHeightmapImportDialog::Open(
		std::string_view DestinationDirectory) -> void
	{
		SourcePathBuffer.fill(0);
		Destination.Reset(DestinationDirectory);
		ModalState.RequestOpen();
	}

	auto FTerrainHeightmapImportDialog::Draw(bool bAllowAssetMutation) -> void
	{
		ModalState.OpenPopupIfRequested("Import Terrain Heightmap");
		const MonaImGui::FUIStyleMetrics Metrics = MonaImGui::GetUIStyleMetrics();
		ImGui::SetNextWindowSize(
			ImVec2(Metrics.WidePopupWidth, 0.0f), ImGuiCond_Appearing);
		if (!ImGui::BeginPopupModal(
			"Import Terrain Heightmap", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize
				| ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) return;

		ImGui::TextUnformatted("Create a Terrain Heightmap from PNG16 or square RAW16.");
		ImGui::TextDisabled("PNG: non-interlaced grayscale 16-bit. RAW: headerless square unsigned U16LE.");
		ImGui::TextDisabled("Samples remain exact, top-left-origin, and row-major; no conversion or resampling is applied.");
		ImGui::SeparatorText("Source Heightmap");
		const float BrowseButtonWidth = Metrics.StandardButtonWidth;
		ImGui::SetNextItemWidth(
			ImGui::GetContentRegionAvail().x - BrowseButtonWidth
				- ImGui::GetStyle().ItemSpacing.x);
		ImGui::InputTextWithHint(
			"##TerrainHeightmapSource", "Choose a .png or .raw heightmap...",
			SourcePathBuffer.data(), SourcePathBuffer.size(),
			ImGuiInputTextFlags_ReadOnly);
		ImGui::SameLine();
		if (ImGui::Button("Browse...", ImVec2(BrowseButtonWidth, 0.0f))) BrowseSource();

		ImGui::SeparatorText("Destination");
		if (Destination.DrawRow(
			"Asset path (one .dasset)", "##TerrainHeightmapAssetPath",
			"/Project/Terrain/Heightmap", "Choose...", BrowseButtonWidth))
			BrowseDestination();
		const FAssetDestinationValidation DestinationValidation = Destination.Inspect();
		const std::filesystem::path Source(SourcePathBuffer.data());
		std::string ValidationMessage;
		if (SourcePathBuffer[0] == '\0')
			ValidationMessage = "Select a PNG16 or square U16LE RAW heightmap to continue.";
		else if (!std::filesystem::is_regular_file(Source))
			ValidationMessage = "The selected source file no longer exists.";
		else
		{
			const std::string Extension = StringUtils::FoldAscii(
				Source.extension().generic_string());
			if (Extension != ".png" && Extension != ".raw")
				ValidationMessage = "Terrain heightmaps require a .png or .raw source.";
			else if (!DestinationValidation) ValidationMessage = DestinationValidation.Message;
		}
		DrawImportDialogWarning(ValidationMessage);
		ImGui::Separator();
		if (!bAllowAssetMutation)
			DrawImportDialogWarning("Asset imports are unavailable during Play.");
		ImGui::BeginDisabled(!bAllowAssetMutation || !ValidationMessage.empty());
		if (ImGui::Button(
			"Import Heightmap", ImVec2(MonaImGui::ScaleUI(160.0f), 0.0f)) && Import())
			ImGui::CloseCurrentPopup();
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (MonaImGui::DialogButton("Cancel", true)) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	auto FTerrainHeightmapImportDialog::BrowseSource() -> void
	{
		FFileDialogRequest Request;
		Request.ParentWindowHandle = ImGui::GetMainViewport()->PlatformHandleRaw;
		Request.Title = "Select a Terrain Heightmap Source";
		Request.Filters = {
			{"Terrain Heightmaps", "*.png;*.raw"},
			{"16-bit grayscale PNG", "*.png"},
			{"Square unsigned 16-bit little-endian RAW", "*.raw"},
			{"All Files", "*.*"}};
		if (const FProjectInfo* Project = GetCurrentProject())
			Request.InitialDirectory = Project->ProjectDir;
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
		const std::string Name = StringUtils::SanitizeFileName(
			std::filesystem::path(Result.FilePath).stem().generic_string(), "Heightmap");
		const FProjectInfo* Project = GetCurrentProject();
		Destination.SuggestPath(Destination.MakeSuggestedPath(
			Name, (Project ? Project->MountRoot : "/") + std::string("Terrain/")));
	}

	auto FTerrainHeightmapImportDialog::BrowseDestination() -> void
	{
		const std::string Name = SourcePathBuffer[0] != '\0'
			? StringUtils::SanitizeFileName(
				std::filesystem::path(SourcePathBuffer.data()).stem().generic_string(),
				"Heightmap") + ".dasset"
			: "Heightmap.dasset";
		Destination.Browse(
			"Choose a Terrain Heightmap Asset Path", Name,
			"The selected asset path is too long for the import form.",
			"Terrain heightmaps must be saved inside a package-enabled mount.", Callbacks);
	}

	auto FTerrainHeightmapImportDialog::Import() -> bool
	{
		Callbacks.Clear();
		const std::filesystem::path Source(SourcePathBuffer.data());
		const FAssetDestinationValidation DestinationValidation =
			Destination.Inspect();
		if (!DestinationValidation)
		{
			SetError(DestinationValidation.Message);
			return false;
		}
		const FPackagePath& AssetPath = DestinationValidation.AssetPath;
		FTopLevelAssetPath TopLevelAssetPath;
		if (!FTopLevelAssetPath::TryCreate(
			AssetPath, AssetPath.GetPackageName(), TopLevelAssetPath))
		{
			SetError("The terrain-heightmap top-level asset path is invalid.");
			return false;
		}
		auto* Factory = NewObject<AssetForge::Builtins::DTerrainHeightmapFactory>(
			nullptr, "TerrainHeightmapDialogFactory", EObjectFlags::Transient);
		const FAssetToolsResult Result = IAssetTools::Get().ImportAsset(
			TopLevelAssetPath, DTerrainHeightmap::StaticClass(),
			Source.generic_string(), Factory);
		if (!Result)
		{
			SetError(Result.Message.empty()
				? "Terrain heightmap import failed." : Result.Message);
			return false;
		}
		if (const FAssetOperationResult Saved = IAssetTools::Get().SaveAssets({
				.AssetPaths = {AssetPath},
				.Publish = [this, &AssetPath](const FAssetOperationNotification&) {
					Callbacks.NotifyAssetCreated(AssetPath.ToString());
				}});
			!Saved)
		{
			SetError(Saved.Message.empty()
				? "Terrain heightmap was created but its package could not be saved."
				: Saved.Message);
			return false;
		}
		UnloadPackage(AssetPath);
		return true;
	}

	auto FTerrainHeightmapImportDialog::SetError(std::string Message) const -> void
	{
		Callbacks.Report(std::move(Message));
	}
}
