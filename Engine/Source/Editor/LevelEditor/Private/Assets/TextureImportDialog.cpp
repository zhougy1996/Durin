#include "Assets/TextureImportDialog.h"

#include "Assets/MountedSourceImport.h"
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
	namespace
	{
		auto FindOwningMount(std::string_view VirtualPath)
			-> const PathUtilities::FMountPoint*
		{
			const PathUtilities::FMountLookupResult Lookup =
				PathUtilities::FindMountForVirtualPath(VirtualPath);
			return Lookup ? Lookup.Mount : nullptr;
		}

		auto Lowercase(std::string Value) -> std::string
		{
			std::ranges::transform(Value, Value.begin(), [](char Character) {
				return static_cast<char>(std::tolower(static_cast<unsigned char>(Character)));
			});
			return Value;
		}
	} // namespace

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
		SourceDestinationBuffer.fill(0);
		LastSuggestedAssetPath.clear();
		LastSuggestedSourceDestination.clear();
		Usage = ETextureUsage::Color;
		SourceMode = EMountedSourceImportMode::IngestExternal;
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
		ImGui::TextDisabled("Reference a mounted source in place, or ingest an external file into SourceAssets.");
		ImGui::Spacing();
		ImGui::SeparatorText("Source image");
		if (ImGui::RadioButton("Reference Existing Source",
			SourceMode == EMountedSourceImportMode::ReferenceExisting))
			SourceMode = EMountedSourceImportMode::ReferenceExisting;
		ImGui::SameLine();
		if (ImGui::RadioButton("Ingest External Source",
			SourceMode == EMountedSourceImportMode::IngestExternal))
			SourceMode = EMountedSourceImportMode::IngestExternal;
		ImGui::TextDisabled(SourceMode == EMountedSourceImportMode::ReferenceExisting
			? "Keeps a source already inside an allowed mounted SourceAssets domain; no copy is created."
			: "Copies an external file transactionally to the explicit mounted source path.");
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

		if (SourceMode == EMountedSourceImportMode::IngestExternal)
		{
			ImGui::TextUnformatted("Source virtual path");
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - BrowseButtonWidth - ImGui::GetStyle().ItemSpacing.x);
			ImGui::InputTextWithHint(
				"##TextureImportSourceDestination",
				"/Game/Textures/AssetName.png",
				SourceDestinationBuffer.data(),
				SourceDestinationBuffer.size());
			ImGui::SameLine();
			if (ImGui::Button("Choose source...", ImVec2(BrowseButtonWidth, 0.0f)))
				BrowseSourceDestination();
		}

		FAssetPath ParsedAssetPath;
		std::string AssetPathError;
		const bool bAssetPathValid = FAssetPath::TryCreate(AssetPathBuffer.data(), ParsedAssetPath, &AssetPathError);
		const bool bMountedDestination = bAssetPathValid;
		const bool bAssetExists = bAssetPathValid && (Asset::GetAssetRegistry().FindAsset(ParsedAssetPath) || Asset::FindLoadedPackage(ParsedAssetPath));
		const FMountedSourceImportDiagnostic SourceDiagnostic = bAssetPathValid
			? InspectMountedSourceImport(
				SourcePathBuffer.data(), ParsedAssetPath.GetView(),
				SourceDestinationBuffer.data(), SourceMode)
			: FMountedSourceImportDiagnostic{};
		const std::filesystem::path SourceDestination(
			SourceDiagnostic.VirtualPath.empty()
				? SourceDestinationBuffer.data()
				: SourceDiagnostic.VirtualPath);
		const bool bSourceExtensionMatches = bHasSource
			&& SourceMode == EMountedSourceImportMode::IngestExternal
			&& Lowercase(SourceDestination.extension().generic_string())
				== Lowercase(SourcePath.extension().generic_string());

		if (bAssetPathValid && bMountedDestination && bHasSource && SourceDiagnostic.bValid)
		{
			ImGui::BeginChild("TextureImportOutputPreview", ImVec2(0.0f, MonaImGui::ScaleUI(78.0f)), ImGuiChildFlags_Borders);
			ImGui::TextDisabled("Asset destination");
			ImGui::TextUnformatted(ParsedAssetPath.ToString().c_str());
			ImGui::TextDisabled("Source virtual path");
			ImGui::TextUnformatted(SourceDiagnostic.VirtualPath.c_str());
			ImGui::EndChild();
			ImGui::TextDisabled("Mount: %s (%s)  |  %s  |  dependency allowed",
				SourceDiagnostic.Mount->VirtualRoot.c_str(),
				DescribeMountOwner(SourceDiagnostic.Mount->Owner),
				SourceDiagnostic.Mount->bSourceWritable ? "writable" : "read-only");
		}

		ImGui::Spacing();
		ImGui::SeparatorText("Build settings");
		const char* UsageNames[] = {"Color", "Normal", "Data / Mask"};
		int UsageIndex = static_cast<int>(Usage);
		if (ImGui::Combo("Usage", &UsageIndex, UsageNames, static_cast<int>(std::size(UsageNames)))) Usage = static_cast<ETextureUsage>(UsageIndex);
		ImGui::TextDisabled(Usage == ETextureUsage::Color
			? "sRGB color sampling with color-aware mip filtering."
			: Usage == ETextureUsage::Normal
				? "Linear sampling with normalized-vector mip filtering."
				: "Linear sampling with independent-channel mip filtering.");

		std::string ValidationMessage;
		if (!bHasSource) ValidationMessage = "Select a source image to continue.";
		else if (!bSourceExists) ValidationMessage = "The selected source file no longer exists.";
		else if (!bSupportedSource) ValidationMessage = "Supported texture formats are PNG, JPEG, BMP, and TGA.";
		else if (!bAssetPathValid) ValidationMessage = AssetPathError;
		else if (!bMountedDestination) ValidationMessage = "Choose a destination inside a mounted Content directory.";
		else if (bAssetExists) ValidationMessage = "An asset already exists at this path.";
		else if (!SourceDiagnostic.bValid)
			ValidationMessage = SourceDiagnostic.Message;
		else if (SourceMode == EMountedSourceImportMode::IngestExternal
			&& !bSourceExtensionMatches)
			ValidationMessage = "The source copy must keep the selected image's file extension.";

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
		if (const FProjectInfo* Project = GetCurrentProject()) Request.InitialDirectory = Project->ProjectDir;
		if (SourceMode == EMountedSourceImportMode::ReferenceExisting)
		{
			const PathUtilities::FMountLookupResult Lookup =
				PathUtilities::FindMountForVirtualPath(AssetPathBuffer.data());
			if (Lookup && Lookup.Mount->SourceAssetsRoot)
				Request.InitialDirectory = Lookup.Mount->SourceAssetsRoot->generic_string();
		}
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
		const std::string PreviousSourceDestination = SourceDestinationBuffer.data();
		const std::string SuggestedSourceDestination = MakeDefaultSourceVirtualPath(
			AssetPathBuffer.data(), "Textures",
			AssetName + std::filesystem::path(Result.FilePath).extension().generic_string());
		if (PreviousSourceDestination.empty()
			|| PreviousSourceDestination == LastSuggestedSourceDestination)
		{
			SourceDestinationBuffer.fill(0);
			std::memcpy(
				SourceDestinationBuffer.data(),
				SuggestedSourceDestination.data(),
				std::min(
					SuggestedSourceDestination.size(),
					SourceDestinationBuffer.size() - 1));
		}
		LastSuggestedSourceDestination = SuggestedSourceDestination;
	}

	auto FTextureImportDialog::BrowseDestination() -> void
	{
		FFileDialogRequest Request;
		Request.ParentWindowHandle = ImGui::GetMainViewport()->PlatformHandleRaw;
		Request.Title = "Choose a Texture Asset Path";
		Request.Filters = {{"Durin Asset", "*.dasset"}};
		Request.DefaultFileName = SourcePathBuffer[0] != '\0' ? StringUtils::SanitizeFileName(std::filesystem::path(SourcePathBuffer.data()).stem().generic_string(), "Texture") + ".dasset" : "Texture.dasset";
		if (const FProjectInfo* Project = GetCurrentProject())
		{
			const PathUtilities::FMountLookupResult Lookup =
				PathUtilities::FindMountForVirtualPath(Project->MountRoot + std::string("Destination"));
			if (Lookup && Lookup.Mount->ContentRoot)
				Request.InitialDirectory = Lookup.Mount->ContentRoot->generic_string();
		}

		const FFileDialogResult Result = SaveFileDialog(Request);
		if (Result.Status == EFileDialogStatus::Cancelled) return;
		if (Result.Status == EFileDialogStatus::Error) { SetError(Result.ErrorMessage); return; }
		const PathUtilities::FContentPathResult Classified =
			PathUtilities::ClassifyContentPath(Result.FilePath);
		if (Classified)
		{
			std::filesystem::path VirtualPath(Classified.NormalizedVirtualPath);
			VirtualPath.replace_extension();
			const std::string VirtualPathString = VirtualPath.generic_string();
			if (VirtualPathString.size() >= AssetPathBuffer.size()) { SetError("The selected asset path is too long for the import form."); return; }
			AssetPathBuffer.fill(0);
			std::memcpy(AssetPathBuffer.data(), VirtualPathString.data(), VirtualPathString.size());
			LastSuggestedAssetPath.clear();
			return;
		}
		SetError("Texture assets must be saved inside a mounted Content directory.");
	}

	auto FTextureImportDialog::BrowseSourceDestination() -> void
	{
		FAssetPath AssetPath;
		std::string Error;
		if (!FAssetPath::TryCreate(AssetPathBuffer.data(), AssetPath, &Error))
		{
			SetError("Choose a valid asset path before selecting the source copy destination.");
			return;
		}
		const PathUtilities::FMountPoint* Mount = FindOwningMount(AssetPath.GetView());
		if (!Mount)
		{
			SetError("Choose an asset destination inside a mounted Content directory first.");
			return;
		}

		if (!Mount->SourceAssetsRoot)
		{
			SetError("The selected asset mount has no SourceAssets domain.");
			return;
		}
		const std::filesystem::path& SourceRoot = *Mount->SourceAssetsRoot;
		FFileDialogRequest Request;
		Request.ParentWindowHandle = ImGui::GetMainViewport()->PlatformHandleRaw;
		Request.Title = "Choose Texture Source Copy Destination";
		Request.Filters = {
			{"All Supported Images", "*.png;*.jpg;*.jpeg;*.bmp;*.tga"},
			{"PNG", "*.png"}, {"JPEG", "*.jpg;*.jpeg"}, {"Bitmap", "*.bmp"},
			{"Targa", "*.tga"}
		};
		Request.InitialDirectory = SourceRoot.generic_string();
		Request.DefaultFileName = SourceDestinationBuffer[0] != '\0'
			? std::filesystem::path(SourceDestinationBuffer.data()).filename().generic_string()
			: SourcePathBuffer[0] != '\0'
				? std::filesystem::path(SourcePathBuffer.data()).filename().generic_string()
				: "Texture.png";

		const FFileDialogResult Result = SaveFileDialog(Request);
		if (Result.Status == EFileDialogStatus::Cancelled) return;
		if (Result.Status == EFileDialogStatus::Error)
		{
			SetError(Result.ErrorMessage);
			return;
		}

		const PathUtilities::FSourcePathResult Classified =
			PathUtilities::ClassifySourcePath(Result.FilePath);
		if (!Classified || Classified.Mount != Mount)
		{
			SetError("Texture source copies must stay beneath this mount's SourceAssets directory.");
			return;
		}
		const std::string& VirtualPath = Classified.NormalizedVirtualPath;
		if (VirtualPath.size() >= SourceDestinationBuffer.size())
		{
			SetError("The selected source destination is too long for the import form.");
			return;
		}
		SourceDestinationBuffer.fill(0);
		std::memcpy(SourceDestinationBuffer.data(), VirtualPath.data(), VirtualPath.size());
		LastSuggestedSourceDestination.clear();
	}

	auto FTextureImportDialog::Import() -> bool
	{
		if (ClearError) ClearError();
		FTexture2DImportSettings Settings;
		if (SourceMode == EMountedSourceImportMode::IngestExternal)
			Settings.SourceDestination = SourceDestinationBuffer.data();
		Settings.Usage = Usage;
		FTexture2DImportResult Result = DTexture2D::ImportAsset(SourcePathBuffer.data(), AssetPathBuffer.data(), Settings);
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
