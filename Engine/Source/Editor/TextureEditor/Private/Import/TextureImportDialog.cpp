#include "Import/TextureImportDialog.h"

#include "Editor/Import/AssetDestinationValidation.h"
#include "AssetTools/IAssetTools.h"
#include "Asset.h"
#include "Asset/AssetOperations.h"
#include "DObject/DObjectGlobals.h"
#include "Dialogs/FileDialog.h"
#include "Misc/Paths.h"
#include "Misc/Project.h"
#include "Misc/StringConvert.h"
#include "Misc/StringHelper.h"
#include "MonaImGui.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureBuildOperations.h"
#include "AssetForge/Builtins/Texture2DFactory.h"
#include "AssetForge/Builtins/Texture2DImport.h"
#include "AssetForge/Builtins/VolumeTextureFactory.h"
#include "AssetForge/Builtins/VolumeTextureImport.h"

namespace Durin::Editor::Texture
{
	namespace
	{
		auto DescribeAssetType(ETextureImportAssetType AssetType) -> const char*
		{
			switch (AssetType)
			{
			case ETextureImportAssetType::Texture2D:
				return "Create a Texture2D asset from an image file.";
			case ETextureImportAssetType::TextureCube:
				return "Create a TextureCube asset from six faces or a 2:1 panorama.";
			case ETextureImportAssetType::VolumeTexture:
				return "Create a Volume Texture asset directly from one PNG slice atlas.";
			}
			return "Create a texture asset from authored image sources.";
		}

		auto ImportButtonLabel(ETextureImportAssetType AssetType) -> const char*
		{
			switch (AssetType)
			{
			case ETextureImportAssetType::Texture2D: return "Import Texture2D";
			case ETextureImportAssetType::TextureCube: return "Import Texture Cube";
			case ETextureImportAssetType::VolumeTexture: return "Import Volume Texture";
			}
			return "Import Texture";
		}
	} // namespace

	FTextureImportDialog::FTextureImportDialog(FImportDialogCallbacks InCallbacks)
		: Callbacks(std::move(InCallbacks))
	{
	}

	auto FTextureImportDialog::Open(std::string_view DestinationDirectory) -> void
	{
		State.Reset();
		Destination.Reset(DestinationDirectory);
		VolumeInspection = {};
		InspectedVolumeSourcePath.clear();
		SubmissionError.clear();
		SelectedVolumeLayout = -1;
		ModalState.RequestOpen();
	}

	auto FTextureImportDialog::Draw(bool bAllowAssetMutation) -> void
	{
		ModalState.OpenPopupIfRequested("Import Texture");

		const MonaImGui::FUIStyleMetrics Metrics = MonaImGui::GetUIStyleMetrics();
		ImGui::SetNextWindowSize(
			ImVec2(Metrics.WidePopupWidth, 0.0f), ImGuiCond_Appearing);
		if (!ImGui::BeginPopupModal("Import Texture", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings))
			return;

		const ETextureImportAssetType AssetType = State.GetAssetType();
		ImGui::TextUnformatted(DescribeAssetType(AssetType));
		ImGui::TextDisabled(
			"The selected image remains in place and is retained as a source filename.");
		const char* AssetTypeNames[] = {
			"Texture2D", "Texture Cube", "Volume Texture"};
		int AssetTypeIndex = static_cast<int>(AssetType);
		if (ImGui::Combo("Asset type", &AssetTypeIndex, AssetTypeNames,
			static_cast<int>(std::size(AssetTypeNames))))
		{
			State.SetAssetType(
				static_cast<ETextureImportAssetType>(AssetTypeIndex));
			SubmissionError.clear();
			if (State.GetAssetType() == ETextureImportAssetType::VolumeTexture)
				InspectVolumeTextureSource();
		}

		ImGui::Spacing();
		ImGui::SeparatorText("Source");
		const float BrowseButtonWidth = Metrics.StandardButtonWidth;
		if (State.GetAssetType() == ETextureImportAssetType::TextureCube)
			DrawTextureCubeSource();
		else
			DrawSingleSource(BrowseButtonWidth);

		ImGui::Spacing();
		ImGui::SeparatorText("Destination");
		if (Destination.DrawRow("Asset path (one .dasset)",
			"##TextureImportAssetPath", "/Project/Textures/AssetName",
			"Choose...", BrowseButtonWidth))
			BrowseDestination();
		if (State.GetAssetType() != ETextureImportAssetType::TextureCube)
			DrawSingleSettings();

		std::string ValidationMessage =
			State.GetAssetType() == ETextureImportAssetType::TextureCube
				? ValidateAndDrawTextureCubeDestination()
				: ValidateAndDrawSingleDestination();
		DrawImportDialogWarning(ValidationMessage);
		if (!SubmissionError.empty())
			DrawImportDialogWarning(SubmissionError);
		if (!bAllowAssetMutation)
			DrawImportDialogWarning("Asset imports are unavailable during Play.");

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::BeginDisabled(!bAllowAssetMutation || !ValidationMessage.empty());
		if (ImGui::Button(ImportButtonLabel(State.GetAssetType()),
			ImVec2(MonaImGui::ScaleUI(180.0f), 0.0f)) &&
			ImportSelectedTexture())
			ImGui::CloseCurrentPopup();
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (MonaImGui::DialogButton("Cancel", true)) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	auto FTextureImportDialog::GetSelectedSingleSourcePath()
		-> std::array<char, 512>&
	{
		return State.GetAssetType() == ETextureImportAssetType::VolumeTexture
			? State.GetVolumeTexture().SourcePathBuffer
			: State.GetTexture2D().SourcePathBuffer;
	}

	auto FTextureImportDialog::DrawSingleSource(float BrowseButtonWidth) -> void
	{
		auto& SourcePathBuffer = GetSelectedSingleSourcePath();
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - BrowseButtonWidth
			- ImGui::GetStyle().ItemSpacing.x);
		ImGui::InputTextWithHint("##TextureImportSource",
			State.GetAssetType() == ETextureImportAssetType::VolumeTexture
				? "Choose a PNG slice atlas..." : "Choose an image file...",
			SourcePathBuffer.data(), SourcePathBuffer.size(),
			ImGuiInputTextFlags_ReadOnly);
		ImGui::SameLine();
		if (ImGui::Button("Browse...", ImVec2(BrowseButtonWidth, 0.0f)))
			BrowseSingleSource();

		const char* SourcePath = SourcePathBuffer.data();
		if (SourcePath[0] != '\0')
			ImGui::TextDisabled("%s", std::filesystem::path(SourcePath)
				.filename().generic_string().c_str());
	}

	auto FTextureImportDialog::DrawSingleSettings() -> void
	{
		ImGui::Spacing();
		if (State.GetAssetType() == ETextureImportAssetType::Texture2D)
		{
			ImGui::SeparatorText("Build settings");
			const char* UsageNames[] = {"Color", "Normal", "Data / Mask"};
			FTexture2DImportFormState& Texture2D = State.GetTexture2D();
			int UsageIndex = static_cast<int>(Texture2D.Usage);
			if (ImGui::Combo("Usage", &UsageIndex, UsageNames,
				static_cast<int>(std::size(UsageNames))))
				Texture2D.Usage = static_cast<ETextureUsage>(UsageIndex);
			ImGui::TextDisabled(Texture2D.Usage == ETextureUsage::Color
				? "sRGB color sampling with color-aware mip filtering."
				: Texture2D.Usage == ETextureUsage::Normal
					? "Linear sampling with normalized-vector mip filtering."
					: "Linear sampling with independent-channel mip filtering.");
			return;
		}

		ImGui::SeparatorText("Volume atlas interpretation");
		FVolumeTextureImportFormState& Volume = State.GetVolumeTexture();
		if (VolumeInspection &&
			InspectedVolumeSourcePath == Volume.SourcePathBuffer.data())
		{
			ImGui::Text("PNG: %u x %u, %u source channel%s",
				VolumeInspection.AtlasWidth, VolumeInspection.AtlasHeight,
				VolumeInspection.SourceChannelCount,
				VolumeInspection.SourceChannelCount == 1 ? "" : "s");
			ImGui::TextDisabled("%s", VolumeInspection.Message.c_str());
			if (!VolumeInspection.SuggestedLayouts.empty())
			{
				const std::string CurrentLayout = SelectedVolumeLayout >= 0
					? std::format("{} x {} x {}  ({} x {} tiles)",
						Volume.SliceWidth, Volume.SliceHeight, Volume.Depth,
						Volume.TilesX, Volume.TilesY)
					: "Custom";
				if (ImGui::BeginCombo("Detected layout", CurrentLayout.c_str()))
				{
					for (size_t Index = 0;
						Index < VolumeInspection.SuggestedLayouts.size(); ++Index)
					{
						const auto& Candidate =
							VolumeInspection.SuggestedLayouts[Index];
						const std::string Label = std::format(
							"{} x {} x {}  ({} x {} tiles)",
							Candidate.SliceWidth, Candidate.SliceHeight,
							Candidate.Depth, Candidate.TilesX, Candidate.TilesY);
						if (ImGui::Selectable(Label.c_str(),
							SelectedVolumeLayout == static_cast<int>(Index)))
							ApplyVolumeTextureLayoutSuggestion(Index);
					}
					ImGui::EndCombo();
				}
			}
		}
		else if (!InspectedVolumeSourcePath.empty() && !SubmissionError.empty())
		{
			ImGui::TextDisabled("The selected PNG could not be inspected.");
		}

		const bool bOpenAdvanced = VolumeInspection.SuggestedLayouts.empty();
		if (!ImGui::CollapsingHeader("Advanced settings",
			bOpenAdvanced ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None))
			return;
		ImGui::TextDisabled("Import format: PNG Row-Major Atlas");
		const char* ChannelNames[] = {
			"Red", "Green", "Blue", "Alpha", "Luminance", "RGBA"};
		int ChannelIndex = static_cast<int>(Volume.Channels);
		if (ImGui::Combo("Channels", &ChannelIndex, ChannelNames,
			static_cast<int>(std::size(ChannelNames))))
		{
			Volume.Channels =
				static_cast<EVolumeTextureSourceChannels>(ChannelIndex);
			SelectedVolumeLayout = -1;
		}
		bool bLayoutEdited = false;
		bLayoutEdited |= ImGui::InputScalar(
			"Slice width", ImGuiDataType_U32, &Volume.SliceWidth);
		bLayoutEdited |= ImGui::InputScalar(
			"Slice height", ImGuiDataType_U32, &Volume.SliceHeight);
		bLayoutEdited |= ImGui::InputScalar("Depth", ImGuiDataType_U32, &Volume.Depth);
		bLayoutEdited |= ImGui::InputScalar(
			"Tile columns", ImGuiDataType_U32, &Volume.TilesX);
		bLayoutEdited |= ImGui::InputScalar(
			"Tile rows", ImGuiDataType_U32, &Volume.TilesY);
		if (bLayoutEdited) SelectedVolumeLayout = -1;
		ImGui::TextDisabled(
			"Slices are read left-to-right, then top-to-bottom; unused tail cells are ignored.");
	}

	auto FTextureImportDialog::ValidateAndDrawSingleDestination() -> std::string
	{
		const auto& SourcePathBuffer = GetSelectedSingleSourcePath();
		const std::filesystem::path SourcePath(SourcePathBuffer.data());
		const bool bHasSource = SourcePathBuffer[0] != '\0';
		const bool bSourceExists = bHasSource &&
			std::filesystem::is_regular_file(SourcePath);
		const std::string SourceExtension =
			StringUtils::FoldAscii(SourcePath.extension().generic_string());
		const bool bVolume =
			State.GetAssetType() == ETextureImportAssetType::VolumeTexture;
		const bool bSupportedSource = bHasSource && (bVolume
			? SourceExtension == ".png"
			: AssetForge::Builtins::IsTexture2DSourceExtension(SourceExtension));

		const FAssetDestinationValidation DestinationValidation =
			Destination.Inspect();
		if (DestinationValidation.bAssetPathValid
			&& DestinationValidation.bMountedDestination && bSourceExists
			&& bSupportedSource)
		{
			ImGui::BeginChild("TextureImportOutputPreview",
				ImVec2(0.0f, MonaImGui::ScaleUI(88.0f)), ImGuiChildFlags_Borders);
			ImGui::TextDisabled("Asset identity");
			ImGui::TextUnformatted(
				DestinationValidation.AssetPath.ToString().c_str());
			ImGui::TextDisabled("Source filename");
			ImGui::TextUnformatted(SourcePath.generic_string().c_str());
			ImGui::EndChild();
		}

		std::string ValidationMessage;
		if (!bHasSource)
			ValidationMessage = "Select a source image to continue.";
		else if (!bSourceExists)
			ValidationMessage = "The selected source file no longer exists.";
		else if (!bSupportedSource)
			ValidationMessage = bVolume
				? "Volume Texture atlas sources must be PNG files."
				: "Supported Texture2D formats are PNG, JPEG, BMP, and TGA.";
		else if (!DestinationValidation)
			ValidationMessage = DestinationValidation.Message;
		else if (bVolume)
		{
			const FVolumeTextureImportFormState& Volume =
				State.GetVolumeTexture();
			if (InspectedVolumeSourcePath == SourcePathBuffer.data()
				&& !VolumeInspection)
			{
				return VolumeInspection.Message;
			}
			AssetForge::Builtins::FVolumeTextureImportSettings Settings{
				.Channels = Volume.Channels,
				.SliceWidth = Volume.SliceWidth,
				.SliceHeight = Volume.SliceHeight,
				.Depth = Volume.Depth,
				.TilesX = Volume.TilesX,
				.TilesY = Volume.TilesY};
			if (Settings.IsValid(&ValidationMessage) && VolumeInspection &&
				InspectedVolumeSourcePath == SourcePathBuffer.data())
			{
				const uint64 ExpectedWidth =
					static_cast<uint64>(Volume.SliceWidth) * Volume.TilesX;
				const uint64 ExpectedHeight =
					static_cast<uint64>(Volume.SliceHeight) * Volume.TilesY;
				if (ExpectedWidth != VolumeInspection.AtlasWidth
					|| ExpectedHeight != VolumeInspection.AtlasHeight)
					ValidationMessage = std::format(
						"The selected layout expects a {}x{} atlas, but the PNG is {}x{}.",
						ExpectedWidth, ExpectedHeight, VolumeInspection.AtlasWidth,
						VolumeInspection.AtlasHeight);
			}
		}
		return ValidationMessage;
	}

	auto FTextureImportDialog::BrowseSingleSource() -> void
	{
		auto& SourcePathBuffer = GetSelectedSingleSourcePath();
		FFileDialogRequest Request;
		Request.ParentWindowHandle =
			ImGui::GetMainViewport()->PlatformHandleRaw;
		Request.Title = State.GetAssetType() ==
			ETextureImportAssetType::VolumeTexture
			? "Select a Volume Texture PNG Atlas"
			: "Select a Texture2D Source File";
		Request.Filters = {
			{"All Supported Textures", "*.png;*.jpg;*.jpeg;*.bmp;*.tga"},
			{"PNG", "*.png"}, {"JPEG", "*.jpg;*.jpeg"},
			{"Bitmap", "*.bmp"}, {"Targa", "*.tga"}, {"All Files", "*.*"}};
		if (const FProjectInfo* Project = GetCurrentProject())
			Request.InitialDirectory = Project->ProjectDir;
		if (SourcePathBuffer[0] != '\0')
			Request.InitialDirectory = std::filesystem::path(
				SourcePathBuffer.data()).parent_path().generic_string();
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
		std::memcpy(SourcePathBuffer.data(), Result.FilePath.data(),
			Result.FilePath.size());
		const std::string AssetName = StringUtils::SanitizeFileName(
			std::filesystem::path(Result.FilePath).stem().generic_string(),
			"Texture");
		const FProjectInfo* Project = GetCurrentProject();
		Destination.SuggestPath(Destination.MakeSuggestedPath(AssetName,
			(Project ? Project->MountRoot : "/") + std::string("Textures/")));
		SubmissionError.clear();
		if (State.GetAssetType() == ETextureImportAssetType::VolumeTexture)
			InspectVolumeTextureSource();
	}

	auto FTextureImportDialog::InspectVolumeTextureSource() -> void
	{
		if (State.GetAssetType() != ETextureImportAssetType::VolumeTexture) return;
		FVolumeTextureImportFormState& Volume = State.GetVolumeTexture();
		const std::string SourcePath = Volume.SourcePathBuffer.data();
		if (SourcePath.empty()
			|| (SourcePath == InspectedVolumeSourcePath && VolumeInspection)) return;

		InspectedVolumeSourcePath = SourcePath;
		SelectedVolumeLayout = -1;
		VolumeInspection = AssetForge::Builtins::InspectVolumeTextureAtlasSource(SourcePath);
		if (!VolumeInspection)
		{
			SubmissionError.clear();
			return;
		}
		SubmissionError.clear();
		Volume.Channels = VolumeInspection.SuggestedChannels;
		if (VolumeInspection.bHasConfidentLayout)
			ApplyVolumeTextureLayoutSuggestion(0);
	}

	auto FTextureImportDialog::ApplyVolumeTextureLayoutSuggestion(size_t Index) -> void
	{
		if (Index >= VolumeInspection.SuggestedLayouts.size()) return;
		const AssetForge::Builtins::FVolumeTextureImportSettings& Suggested =
			VolumeInspection.SuggestedLayouts[Index];
		FVolumeTextureImportFormState& Volume = State.GetVolumeTexture();
		Volume.Channels = Suggested.Channels;
		Volume.SliceWidth = Suggested.SliceWidth;
		Volume.SliceHeight = Suggested.SliceHeight;
		Volume.Depth = Suggested.Depth;
		Volume.TilesX = Suggested.TilesX;
		Volume.TilesY = Suggested.TilesY;
		SelectedVolumeLayout = static_cast<int>(Index);
		SubmissionError.clear();
	}

	auto FTextureImportDialog::BrowseDestination() -> void
	{
		FAssetPath CurrentAssetPath;
		const std::string DefaultFileName =
			FAssetPath::TryCreate(Destination.GetPath(), CurrentAssetPath)
				? std::string(CurrentAssetPath.GetAssetName()) + ".dasset"
				: "Texture.dasset";
		Destination.Browse("Choose a Texture Asset Path", DefaultFileName,
			"The selected asset path is too long for the import form.",
			"Texture assets must be saved inside a package-enabled mount.",
			Callbacks);
	}

	auto FTextureImportDialog::ImportSelectedTexture() -> bool
	{
		Callbacks.Clear();
		SubmissionError.clear();
		const bool bImported =
			State.GetAssetType() == ETextureImportAssetType::TextureCube
				? ImportTextureCube()
				: ImportSingleTexture();
		return bImported;
	}

	auto FTextureImportDialog::ImportSingleTexture() -> bool
	{
		const auto& SourcePathBuffer = GetSelectedSingleSourcePath();
		const FAssetDestinationValidation DestinationValidation =
			Destination.Inspect();
		if (!DestinationValidation)
		{
			SetError(DestinationValidation.Message);
			return false;
		}
		const FAssetPath& AssetPath = DestinationValidation.AssetPath;
		const std::string Path = AssetPath.ToString();
		const FImportDialogCallbacks CompletionCallbacks = Callbacks;
		if (State.GetAssetType() == ETextureImportAssetType::VolumeTexture)
		{
			const FVolumeTextureImportFormState& Volume =
				State.GetVolumeTexture();
			AssetForge::Builtins::FVolumeTextureImportSettings Settings;
			Settings.Channels = Volume.Channels;
			Settings.SliceWidth = Volume.SliceWidth;
			Settings.SliceHeight = Volume.SliceHeight;
			Settings.Depth = Volume.Depth;
			Settings.TilesX = Volume.TilesX;
			Settings.TilesY = Volume.TilesY;
			auto* Factory = NewObject<AssetForge::Builtins::DVolumeTextureFactory>(
				nullptr, "VolumeTextureDialogFactory", EObjectFlags::Transient);
			Factory->SetImportSettings(Settings);
			const FAssetToolsResult Result = IAssetTools::Get().ImportAsset(
				AssetPath, DVolumeTexture::StaticClass(),
				SourcePathBuffer.data(), Factory);
			if (!Result)
			{
				SetError(Result.Message.empty()
					? "VolumeTexture import failed." : Result.Message);
				return false;
			}
			if (const FAssetOperationResult Saved = IAssetTools::Get().SaveAssets({
					.AssetPaths = {AssetPath},
					.Publish = [CompletionCallbacks, Path](const FAssetOperationNotification&) {
						CompletionCallbacks.NotifyAssetCreated(Path);
					}});
				!Saved)
			{
				SetError(Saved.Message.empty()
					? "VolumeTexture was created but its package could not be saved."
					: Saved.Message);
				return false;
			}
			Asset::UnloadPackage(AssetPath);
			return true;
		}

		FTexture2DImportSettings Settings;
		Settings.Usage = State.GetTexture2D().Usage;
		auto* Factory = NewObject<AssetForge::Builtins::DTexture2DFactory>(
			nullptr, "Texture2DDialogFactory", EObjectFlags::Transient);
		Factory->SetImportSettings(Settings);
		const FAssetToolsResult Result = IAssetTools::Get().ImportAsset(
			AssetPath, DTexture2D::StaticClass(), SourcePathBuffer.data(), Factory);
		if (!Result)
		{
			SetError(Result.Message.empty()
				? "Texture2D import failed." : Result.Message);
			return false;
		}
		if (const FAssetOperationResult Saved = IAssetTools::Get().SaveAssets({
				.AssetPaths = {AssetPath},
				.Publish = [CompletionCallbacks, Path](const FAssetOperationNotification&) {
					CompletionCallbacks.NotifyAssetCreated(Path);
				}});
			!Saved)
		{
			SetError(Saved.Message.empty()
				? "Texture2D was created but its package could not be saved."
				: Saved.Message);
			return false;
		}
		Asset::UnloadPackage(AssetPath);
		return true;
	}

	auto FTextureImportDialog::SetError(std::string Message) -> void
	{
		SubmissionError = std::move(Message);
	}
} // namespace Durin::Editor::Texture
