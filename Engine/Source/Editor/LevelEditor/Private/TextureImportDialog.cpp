#include "TextureImportDialog.h"

#include "AssetSystem.h"
#include "Dialogs/FileDialog.h"
#include "ImageDecoder.h"
#include "Misc/Paths.h"
#include "Misc/Project.h"
#include "Misc/StringConvert.h"
#include "MonaImGui.h"
#include "Texture/Texture2D.h"

namespace Durin
{
	FTextureImportDialog::FTextureImportDialog(std::function<void()> InClearError, std::function<void(std::string)> InReportError, std::function<void(std::string)> InImported)
		: ClearError(std::move(InClearError))
		, ReportError(std::move(InReportError))
		, Imported(std::move(InImported))
	{
	}

	auto FTextureImportDialog::Open(std::string_view DestinationDirectory) -> void
	{
		SourcePathBuffer.fill(0);
		AssetPathBuffer.fill(0);
		LastSuggestedAssetPath.clear();
		PreferredDestinationDirectory = DestinationDirectory;
		if (!PreferredDestinationDirectory.empty() && !PreferredDestinationDirectory.ends_with('/')) PreferredDestinationDirectory += '/';
		bOpenRequested = true;
	}

	auto FTextureImportDialog::Draw() -> void
	{
		if (bOpenRequested)
		{
			ImGui::OpenPopup("Import Texture");
			bOpenRequested = false;
		}

		const MonaImGui::FUIStyleMetrics Metrics = MonaImGui::GetUIStyleMetrics();
		ImGui::SetNextWindowSize(ImVec2(Metrics.WidePopupWidth, 0.0f), ImGuiCond_Appearing);
		if (!ImGui::BeginPopupModal("Import Texture", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings))
			return;

		ImGui::TextUnformatted("Create a Texture2D asset from an image file.");
		ImGui::TextDisabled("The source image is copied next to the .dasset package for thumbnails and rebuilds.");
		ImGui::Spacing();
		ImGui::SeparatorText("Source image");
		const float BrowseButtonWidth = Metrics.StandardButtonWidth;
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - BrowseButtonWidth - ImGui::GetStyle().ItemSpacing.x);
		ImGui::InputTextWithHint("##TextureImportSource", "Choose a PNG, JPEG, BMP, or TGA image...", SourcePathBuffer.data(), SourcePathBuffer.size(), ImGuiInputTextFlags_ReadOnly);
		ImGui::SameLine();
		if (ImGui::Button("Browse...", ImVec2(BrowseButtonWidth, 0.0f))) BrowseSource();

		const std::filesystem::path SourcePath(SourcePathBuffer.data());
		const bool bHasSource = SourcePathBuffer[0] != '\0';
		const bool bSourceExists = bHasSource && std::filesystem::is_regular_file(SourcePath);
		const bool bSupportedSource = bHasSource && Asset::IsSupportedImageExtension(SourcePath.extension().generic_string());
		if (bHasSource) ImGui::TextDisabled("%s", SourcePath.filename().generic_string().c_str());

		ImGui::Spacing();
		ImGui::SeparatorText("Destination");
		ImGui::TextUnformatted("Asset path");
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - BrowseButtonWidth - ImGui::GetStyle().ItemSpacing.x);
		ImGui::InputTextWithHint("##TextureImportAssetPath", "/Project/Textures/AssetName", AssetPathBuffer.data(), AssetPathBuffer.size());
		ImGui::SameLine();
		if (ImGui::Button("Choose...", ImVec2(BrowseButtonWidth, 0.0f))) BrowseDestination();

		FAssetPath ParsedAssetPath;
		std::string AssetPathError;
		const bool bAssetPathValid = FAssetPath::TryCreate(AssetPathBuffer.data(), ParsedAssetPath, &AssetPathError);
		bool bMountedDestination = false;
		if (bAssetPathValid)
			for (const PathUtilities::FMountPoint& Mount : PathUtilities::GetRegisteredMountPoints())
				if (ParsedAssetPath.GetView().starts_with(Mount.VirtualRoot)) { bMountedDestination = true; break; }
		const bool bAssetExists = bAssetPathValid && (Asset::GetAssetRegistry().FindAsset(ParsedAssetPath) || Asset::FindLoadedPackage(ParsedAssetPath));

		if (bAssetPathValid && bMountedDestination && bHasSource)
		{
			const std::string SourceFileName = std::string(ParsedAssetPath.GetAssetName()) + SourcePath.extension().generic_string();
			ImGui::BeginChild("TextureImportOutputPreview", ImVec2(0.0f, MonaImGui::ScaleUI(58.0f)), ImGuiChildFlags_Borders);
			ImGui::TextDisabled("Files to create");
			ImGui::TextUnformatted(std::format("{}.dasset   +   {}", ParsedAssetPath.GetAssetName(), SourceFileName).c_str());
			ImGui::EndChild();
		}

		std::string ValidationMessage;
		if (!bHasSource) ValidationMessage = "Select a source image to continue.";
		else if (!bSourceExists) ValidationMessage = "The selected source file no longer exists.";
		else if (!bSupportedSource) ValidationMessage = "Supported texture formats are PNG, JPEG, BMP, and TGA.";
		else if (!bAssetPathValid) ValidationMessage = AssetPathError;
		else if (!bMountedDestination) ValidationMessage = "Choose a destination inside a mounted Content directory.";
		else if (bAssetExists) ValidationMessage = "An asset already exists at this path.";

		if (!ValidationMessage.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::Warning));
			ImGui::TextWrapped("%s", ValidationMessage.c_str());
			ImGui::PopStyleColor();
		}

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::BeginDisabled(!ValidationMessage.empty());
		if (ImGui::Button("Import Texture", ImVec2(MonaImGui::ScaleUI(150.0f), 0.0f)) && Import()) ImGui::CloseCurrentPopup();
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (MonaImGui::DialogButton("Cancel", true)) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	auto FTextureImportDialog::BrowseSource() -> void
	{
		FFileDialogRequest Request;
		Request.ParentWindowHandle = ImGui::GetMainViewport()->PlatformHandleRaw;
		Request.Title = "Select a Texture Source File";
		Request.Filters = {
			{"All Supported Images", "*.png;*.jpg;*.jpeg;*.bmp;*.tga"},
			{"PNG", "*.png"}, {"JPEG", "*.jpg;*.jpeg"}, {"Bitmap", "*.bmp"}, {"Targa", "*.tga"}, {"All Files", "*.*"}
		};
		if (SourcePathBuffer[0] != '\0') Request.InitialDirectory = std::filesystem::path(SourcePathBuffer.data()).parent_path().generic_string();
		const FFileDialogResult Result = OpenFileDialog(Request);
		if (Result.Status == EFileDialogStatus::Cancelled) return;
		if (Result.Status == EFileDialogStatus::Error) { SetError(Result.ErrorMessage); return; }
		if (Result.FilePath.size() >= SourcePathBuffer.size()) { SetError("The selected file path is too long for the import form."); return; }

		const std::string PreviousAssetPath = AssetPathBuffer.data();
		SourcePathBuffer.fill(0);
		std::memcpy(SourcePathBuffer.data(), Result.FilePath.data(), Result.FilePath.size());
		const std::string AssetName = StringUtils::SanitizeFileName(std::filesystem::path(Result.FilePath).stem().generic_string(), "Texture");
		const FProjectInfo* Project = GetCurrentProject();
		const std::string SuggestedPath = !PreferredDestinationDirectory.empty() ? PreferredDestinationDirectory + AssetName : (Project ? Project->MountRoot : "/") + "Textures/" + AssetName;
		if (PreviousAssetPath.empty() || PreviousAssetPath == LastSuggestedAssetPath)
		{
			AssetPathBuffer.fill(0);
			std::memcpy(AssetPathBuffer.data(), SuggestedPath.data(), std::min(SuggestedPath.size(), AssetPathBuffer.size() - 1));
		}
		LastSuggestedAssetPath = SuggestedPath;
	}

	auto FTextureImportDialog::BrowseDestination() -> void
	{
		FFileDialogRequest Request;
		Request.ParentWindowHandle = ImGui::GetMainViewport()->PlatformHandleRaw;
		Request.Title = "Choose a Texture Asset Path";
		Request.Filters = {{"Durin Asset", "*.dasset"}};
		Request.DefaultFileName = SourcePathBuffer[0] != '\0' ? StringUtils::SanitizeFileName(std::filesystem::path(SourcePathBuffer.data()).stem().generic_string(), "Texture") + ".dasset" : "Texture.dasset";
		for (const PathUtilities::FMountPoint& Mount : PathUtilities::GetRegisteredMountPoints())
			if (const FProjectInfo* Project = GetCurrentProject(); Project && Mount.VirtualRoot == Project->MountRoot) { Request.InitialDirectory = Mount.PhysicalPath; break; }

		const FFileDialogResult Result = SaveFileDialog(Request);
		if (Result.Status == EFileDialogStatus::Cancelled) return;
		if (Result.Status == EFileDialogStatus::Error) { SetError(Result.ErrorMessage); return; }
		std::string SelectedPath = std::filesystem::absolute(Result.FilePath).lexically_normal().generic_string();
		std::ranges::transform(SelectedPath, SelectedPath.begin(), [](unsigned char Character) { return static_cast<char>(std::tolower(Character)); });
		for (const PathUtilities::FMountPoint& Mount : PathUtilities::GetRegisteredMountPoints())
		{
			std::string MountPath = std::filesystem::absolute(Mount.PhysicalPath).lexically_normal().generic_string();
			if (!MountPath.ends_with('/')) MountPath += '/';
			std::ranges::transform(MountPath, MountPath.begin(), [](unsigned char Character) { return static_cast<char>(std::tolower(Character)); });
			if (!SelectedPath.starts_with(MountPath)) continue;
			std::filesystem::path RelativePath = std::filesystem::path(Result.FilePath).lexically_relative(std::filesystem::path(Mount.PhysicalPath));
			RelativePath.replace_extension();
			const std::string VirtualPath = Mount.VirtualRoot + RelativePath.generic_string();
			if (VirtualPath.size() >= AssetPathBuffer.size()) { SetError("The selected asset path is too long for the import form."); return; }
			AssetPathBuffer.fill(0);
			std::memcpy(AssetPathBuffer.data(), VirtualPath.data(), VirtualPath.size());
			LastSuggestedAssetPath.clear();
			return;
		}
		SetError("Texture assets must be saved inside a mounted Content directory.");
	}

	auto FTextureImportDialog::Import() -> bool
	{
		if (ClearError) ClearError();
		FTexture2DImportResult Result = DTexture2D::ImportAsset(SourcePathBuffer.data(), AssetPathBuffer.data());
		if (!Result) { SetError(Result.Message); return false; }
		if (Imported) Imported(AssetPathBuffer.data());
		FAssetPath ImportedPath;
		if (FAssetPath::TryCreate(AssetPathBuffer.data(), ImportedPath)) Asset::UnloadPackage(ImportedPath);
		return true;
	}

	auto FTextureImportDialog::SetError(std::string Message) const -> void
	{
		if (ReportError) ReportError(std::move(Message));
	}
} // namespace Durin
