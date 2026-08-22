#include "Assets/TextureImportDialog.h"

#include "Assets/AssetDestinationValidation.h"
#include "Assets/MountedSourceImport.h"
#include "AssetAuthoring.h"
#include "Dialogs/FileDialog.h"
#include "Misc/Paths.h"
#include "Misc/Project.h"
#include "Misc/StringConvert.h"
#include "MonaImGui.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureBuildOperations.h"
#include "Texture2DSourceTranslation.h"
#include "VolumeTextureSourceTranslation.h"

namespace Durin::Editor::Level
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

	FTextureImportDialog::FTextureImportDialog(FImportDialogCallbacks InCallbacks)
		: Callbacks(std::move(InCallbacks))
	{
	}

	auto FTextureImportDialog::Open(std::string_view DestinationDirectory) -> void
	{
		SourceForm.Reset();
		Usage = ETextureUsage::Color;
		bImportVolume = false;
		VolumeChannels = EVolumeTextureSourceChannels::Red;
		VolumeSliceWidth = 128;
		VolumeSliceHeight = 128;
		VolumeDepth = 128;
		VolumeTilesX = 12;
		VolumeTilesY = 12;
		Destination.Reset(DestinationDirectory);
		ModalState.RequestOpen();
	}

	auto FTextureImportDialog::Draw() -> void
	{
		ModalState.OpenPopupIfRequested("Import Texture");

		const MonaImGui::FUIStyleMetrics Metrics = MonaImGui::GetUIStyleMetrics();
		ImGui::SetNextWindowSize(ImVec2(Metrics.WidePopupWidth, 0.0f), ImGuiCond_Appearing);
		if (!ImGui::BeginPopupModal("Import Texture", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings))
			return;

		const bool bVolumeSource = bImportVolume;
		ImGui::TextUnformatted(bVolumeSource
			? "Create a Volume Texture asset directly from one PNG slice atlas."
			: "Create a Texture2D asset from an image file.");
		ImGui::TextDisabled("Reference a mounted source in place, or ingest an external file into a writable mount.");
		const char* AssetTypeNames[] = {"Texture2D", "Volume Texture"};
		int AssetType = bVolumeSource ? 1 : 0;
		if (ImGui::Combo("Asset type", &AssetType, AssetTypeNames, 2))
		{
			bImportVolume = AssetType == 1;
			SuggestSourceDestination();
		}
		ImGui::Spacing();
		ImGui::SeparatorText("Source image");
		SourceForm.DrawMode(
			"Copies an external file transactionally to the explicit mounted source path.");
		const float BrowseButtonWidth = Metrics.StandardButtonWidth;
		if (SourceForm.DrawSourceRow("##TextureImportSource",
			"Choose an image file...", BrowseButtonWidth)) BrowseSource();

		const std::filesystem::path SourcePath(SourcePathBuffer.data());
		const bool bHasSource = SourcePathBuffer[0] != '\0';
		const bool bSourceExists = bHasSource && std::filesystem::is_regular_file(SourcePath);
		const std::string SourceExtension = Lowercase(SourcePath.extension().generic_string());
		const bool bSupportedSource = bHasSource && (bVolumeSource
			? SourceExtension == ".png"
			: Asset::Forge::IsTexture2DSourceExtension(SourceExtension));
		if (bHasSource) ImGui::TextDisabled("%s", SourcePath.filename().generic_string().c_str());

		ImGui::Spacing();
		ImGui::SeparatorText("Destination");
		if (Destination.DrawRow("Asset path (one .dasset)", "##TextureImportAssetPath",
			"/Project/Textures/AssetName", "Choose...", BrowseButtonWidth))
			BrowseDestination();

		if (SourceForm.DrawDestinationRow("##TextureImportSourceDestination",
			"/Project/Sources/Textures/AssetName.png", BrowseButtonWidth))
			BrowseSourceDestination();

		const FAssetDestinationValidation DestinationValidation =
			Destination.Inspect();
		const bool bEngineAuthoringContext = DestinationValidation.Mount
			&& DestinationValidation.Mount->Owner == PathUtilities::EMountOwner::Engine;
		const FMountedSourceImportDiagnostic SourceDiagnostic = DestinationValidation.bAssetPathValid
			? SourceForm.Inspect(
				DestinationValidation.AssetPath.GetView(), bEngineAuthoringContext)
			: FMountedSourceImportDiagnostic{};
		const std::filesystem::path SourceDestination(
			SourceDiagnostic.VirtualPath.empty()
				? SourceDestinationBuffer.data()
				: SourceDiagnostic.VirtualPath);
		const bool bSourceExtensionMatches = bHasSource
			&& SourceMode == EMountedSourceImportMode::IngestExternal
			&& Lowercase(SourceDestination.extension().generic_string())
				== Lowercase(SourcePath.extension().generic_string());

		if (DestinationValidation.bAssetPathValid
			&& DestinationValidation.bMountedDestination && bHasSource
			&& SourceDiagnostic.bValid)
		{
			ImGui::BeginChild("TextureImportOutputPreview",
				ImVec2(0.0f, MonaImGui::ScaleUI(112.0f)), ImGuiChildFlags_Borders);
			ImGui::TextDisabled("Asset identity");
			ImGui::TextUnformatted(
				DestinationValidation.AssetPath.ToString().c_str());
			ImGui::TextDisabled("Package file");
			ImGui::TextUnformatted(std::format("{}.dasset",
				DestinationValidation.AssetPath.ToString()).c_str());
			ImGui::TextDisabled("Source virtual path");
			ImGui::TextUnformatted(SourceDiagnostic.VirtualPath.c_str());
			ImGui::EndChild();
			ImGui::TextDisabled("Mount: %s (%s)  |  %s  |  dependency allowed",
				SourceDiagnostic.Mount->VirtualRoot.c_str(),
				DescribeMountOwner(SourceDiagnostic.Mount->Owner),
				SourceDiagnostic.Mount->bAuthoringWritable ? "writable" : "read-only");
			if (bEngineAuthoringContext)
				ImGui::TextDisabled("Engine authoring: this import writes shared Engine content.");
		}

		if (!bVolumeSource)
		{
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
		}
		else
		{
			ImGui::Spacing();
			ImGui::SeparatorText("Volume atlas settings");
			ImGui::TextDisabled("Import format: PNG Row-Major Atlas");
			const char* ChannelNames[] = {"Red", "Green", "Blue", "Alpha", "Luminance", "RGBA"};
			int ChannelIndex = static_cast<int>(VolumeChannels);
			if (ImGui::Combo("Channels", &ChannelIndex, ChannelNames, 6))
				VolumeChannels = static_cast<EVolumeTextureSourceChannels>(ChannelIndex);
			ImGui::InputScalar("Slice width", ImGuiDataType_U32, &VolumeSliceWidth);
			ImGui::InputScalar("Slice height", ImGuiDataType_U32, &VolumeSliceHeight);
			ImGui::InputScalar("Depth", ImGuiDataType_U32, &VolumeDepth);
			ImGui::InputScalar("Tile columns", ImGuiDataType_U32, &VolumeTilesX);
			ImGui::InputScalar("Tile rows", ImGuiDataType_U32, &VolumeTilesY);
			ImGui::TextDisabled("Slices are read left-to-right, then top-to-bottom; unused tail cells are ignored.");
		}

		std::string ValidationMessage;
		if (!bHasSource) ValidationMessage = "Select a source image to continue.";
		else if (!bSourceExists) ValidationMessage = "The selected source file no longer exists.";
		else if (!bSupportedSource) ValidationMessage = bVolumeSource
			? "Volume Texture atlas sources must be PNG files."
			: "Supported Texture2D formats are PNG, JPEG, BMP, and TGA.";
		else if (!DestinationValidation)
			ValidationMessage = DestinationValidation.Message;
		else if (!SourceDiagnostic.bValid)
			ValidationMessage = SourceDiagnostic.Message;
		else if (SourceMode == EMountedSourceImportMode::IngestExternal
			&& !bSourceExtensionMatches)
			ValidationMessage = "The source copy must keep the selected image's file extension.";
		else if (bVolumeSource)
		{
			Asset::Forge::FVolumeTextureImportSettings Settings{
				.Channels = VolumeChannels, .SliceWidth = VolumeSliceWidth,
				.SliceHeight = VolumeSliceHeight, .Depth = VolumeDepth,
				.TilesX = VolumeTilesX, .TilesY = VolumeTilesY};
			Settings.IsValid(&ValidationMessage);
		}

		DrawImportDialogWarning(ValidationMessage);

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::BeginDisabled(!ValidationMessage.empty());
		if (ImGui::Button(bVolumeSource ? "Import Volume Texture" : "Import Texture",
			ImVec2(MonaImGui::ScaleUI(bVolumeSource ? 180.0f : 150.0f), 0.0f))
			&& Import()) ImGui::CloseCurrentPopup();
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
			{"All Supported Textures", "*.png;*.jpg;*.jpeg;*.bmp;*.tga"},
			{"PNG", "*.png"}, {"JPEG", "*.jpg;*.jpeg"}, {"Bitmap", "*.bmp"}, {"Targa", "*.tga"}, {"All Files", "*.*"}
		};
		if (const FProjectInfo* Project = GetCurrentProject()) Request.InitialDirectory = Project->ProjectDir;
		if (SourceMode == EMountedSourceImportMode::ReferenceExisting)
		{
			const PathUtilities::FMountLookupResult Lookup =
				PathUtilities::FindMountForVirtualPath(Destination.GetPath());
			if (Lookup)
				Request.InitialDirectory = Lookup.Mount->GetContentDir().generic_string();
		}
		if (SourcePathBuffer[0] != '\0') Request.InitialDirectory = std::filesystem::path(SourcePathBuffer.data()).parent_path().generic_string();
		const FFileDialogResult Result = OpenFileDialog(Request);
		if (Result.Status == EFileDialogStatus::Cancelled) return;
		if (Result.Status == EFileDialogStatus::Error) { SetError(Result.ErrorMessage); return; }
		if (Result.FilePath.size() >= SourcePathBuffer.size()) { SetError("The selected file path is too long for the import form."); return; }

		SourcePathBuffer.fill(0);
		std::memcpy(SourcePathBuffer.data(), Result.FilePath.data(), Result.FilePath.size());
		const std::string AssetName = StringUtils::SanitizeFileName(std::filesystem::path(Result.FilePath).stem().generic_string(), "Texture");
		const FProjectInfo* Project = GetCurrentProject();
		Destination.SuggestPath(Destination.MakeSuggestedPath(AssetName,
			(Project ? Project->MountRoot : "/") + std::string("Textures/")));
		SuggestSourceDestination();
	}

	auto FTextureImportDialog::SuggestSourceDestination() -> void
	{
		if (SourcePathBuffer[0] == '\0') return;
		const std::filesystem::path SourcePath(SourcePathBuffer.data());
		const std::string AssetName = StringUtils::SanitizeFileName(
			SourcePath.stem().generic_string(), "Texture");
		const bool bVolumeSource = bImportVolume;
		const std::string SuggestedSourceDestination = MakeDefaultImportedSourceVirtualPath(
			Destination.GetPath(), bVolumeSource ? "VolumeTextures" : "Textures",
			AssetName + SourcePath.extension().generic_string(),
			bVolumeSource ? AssetName : std::string_view{});
		SourceForm.SuggestDestination(SuggestedSourceDestination);
	}

	auto FTextureImportDialog::BrowseDestination() -> void
	{
		const std::string DefaultFileName = SourcePathBuffer[0] != '\0'
			? StringUtils::SanitizeFileName(
				std::filesystem::path(SourcePathBuffer.data()).stem().generic_string(),
				"Texture") + ".dasset"
			: "Texture.dasset";
		if (Destination.Browse("Choose a Texture Asset Path", DefaultFileName,
			"The selected asset path is too long for the import form.",
			"Texture assets must be saved inside a package-enabled mount.",
			Callbacks))
			SuggestSourceDestination();
	}

	auto FTextureImportDialog::BrowseSourceDestination() -> void
	{
		FAssetPath AssetPath;
		std::string Error;
		if (!FAssetPath::TryCreate(Destination.GetPath(), AssetPath, &Error))
		{
			SetError("Choose a valid asset path before selecting the source copy destination.");
			return;
		}
		const PathUtilities::FMountPoint* Mount = FindOwningMount(AssetPath.GetView());
		if (!Mount)
		{
			SetError("Choose an asset destination inside a package-enabled mount first.");
			return;
		}

		const std::filesystem::path SourceRoot = Mount->GetContentDir();
		FFileDialogRequest Request;
		Request.ParentWindowHandle = ImGui::GetMainViewport()->PlatformHandleRaw;
		Request.Title = "Choose Texture Source Copy Destination";
		Request.Filters = {
			{"All Supported Textures", "*.png;*.jpg;*.jpeg;*.bmp;*.tga"},
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
			SetError("Texture source copies must stay inside the selected mount.");
			return;
		}
		const std::string& VirtualPath = Classified.NormalizedVirtualPath;
		if (VirtualPath.size() >= SourceDestinationBuffer.size())
		{
			SetError("The selected source destination is too long for the import form.");
			return;
		}
		SourceForm.SetDestination(VirtualPath);
	}

	auto FTextureImportDialog::Import() -> bool
	{
		Callbacks.Clear();
		const bool bVolumeSource = bImportVolume;
		if (bVolumeSource)
		{
			Asset::Forge::FVolumeTextureImportSettings Settings;
			if (SourceMode == EMountedSourceImportMode::IngestExternal)
				Settings.SourceDestination = SourceDestinationBuffer.data();
			Settings.Channels = VolumeChannels;
			Settings.SliceWidth = VolumeSliceWidth;
			Settings.SliceHeight = VolumeSliceHeight;
			Settings.Depth = VolumeDepth;
			Settings.TilesX = VolumeTilesX;
			Settings.TilesY = VolumeTilesY;
			const Asset::Forge::FVolumeTextureImportResult Result =
				Asset::Forge::ImportVolumeTextureAsset(SourcePathBuffer.data(),
					Destination.GetPath(), Settings,
					IsEngineAuthoringDestination(Destination.GetPath()));
			if (!Result) { SetError(Result.Message); return false; }
			Callbacks.NotifyImported(Destination.GetPath());
			FAssetPath ImportedPath;
			if (FAssetPath::TryCreate(Destination.GetPath(), ImportedPath))
				Asset::UnloadPackage(ImportedPath);
			return true;
		}
		FTexture2DImportSettings Settings;
		if (SourceMode == EMountedSourceImportMode::IngestExternal)
			Settings.SourceDestination = SourceDestinationBuffer.data();
		Settings.Usage = Usage;
		FTexture2DImportResult Result = Asset::Forge::ImportTexture2DAsset(
			SourcePathBuffer.data(), Destination.GetPath(), Settings,
			IsEngineAuthoringDestination(Destination.GetPath()));
		if (!Result) { SetError(Result.Message); return false; }
		Callbacks.NotifyImported(Destination.GetPath());
		FAssetPath ImportedPath;
		if (FAssetPath::TryCreate(Destination.GetPath(), ImportedPath))
			Asset::UnloadPackage(ImportedPath);
		return true;
	}

	auto FTextureImportDialog::SetError(std::string Message) const -> void
	{
		Callbacks.Report(std::move(Message));
	}
} // namespace Durin::Editor::Level
