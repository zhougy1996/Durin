#include "Assets/TextureCubeImportDialog.h"

#include "AssetSystem.h"
#include "Dialogs/FileDialog.h"
#include "Misc/Paths.h"
#include "Misc/Project.h"
#include "Misc/StringConvert.h"
#include "MonaImGui.h"
#include "PixelFormat.h"
#include "Texture/TextureCube.h"

namespace Durin
{
	namespace
	{
		constexpr std::array<std::string_view, TextureCubeFaceCount> FaceLabels = {
			"+X", "-X", "+Y", "-Y", "+Z", "-Z"};
		constexpr std::array<std::string_view, TextureCubeFaceCount> FaceDirections = {
			"Forward", "Backward", "Right", "Left", "Up", "Down"};
		constexpr std::array<std::string_view, TextureCubeFaceCount> FaceOrientationHints = {
			"top +Y, right -Z", "top +Y, right +Z", "top -Z, right +X",
			"top +Z, right +X", "top +Y, right +X", "top +Y, right -X"};

		auto FaceIndex(ETextureCubeFace Face) -> size_t
		{
			return static_cast<size_t>(Face);
		}

		auto IsRadianceHDRPath(std::string_view Path) -> bool
		{
			std::string Extension = std::filesystem::path(Path).extension().generic_string();
			std::ranges::transform(Extension, Extension.begin(),
				[](unsigned char Character) { return static_cast<char>(std::tolower(Character)); });
			return Extension == ".hdr";
		}
	}

	FTextureCubeImportDialog::FTextureCubeImportDialog(std::function<void()> InClearError,
		std::function<void(std::string)> InReportError, std::function<void(std::string)> InImported)
		: ClearError(std::move(InClearError))
		, ReportError(std::move(InReportError))
		, Imported(std::move(InImported))
	{
	}

	auto FTextureCubeImportDialog::Open(std::string_view DestinationDirectory) -> void
	{
		for (auto& Buffer : FacePathBuffers) Buffer.fill(0);
		PanoramaPathBuffer.fill(0);
		AssetPathBuffer.fill(0);
		SourceLayout = ETextureCubeSourceLayout::SixFaces;
		PanoramaFaceDimension = 0;
		PanoramaExposureEV = 0.0f;
		SourceValidationMessage = "Select all six face images to continue.";
		ValidatedSourceWidth = 0;
		ValidatedSourceHeight = 0;
		ValidatedDimension = 0;
		ValidatedMipCount = 0;
		ValidatedPixelFormat = EPixelFormat::Unknown;
		bValidatedHDR = false;
		bSourcesValid = false;
		PreferredDestinationDirectory = DestinationDirectory;
		if (!PreferredDestinationDirectory.empty() && !PreferredDestinationDirectory.ends_with('/'))
			PreferredDestinationDirectory += '/';
		bOpenRequested = true;
	}

	auto FTextureCubeImportDialog::Draw() -> void
	{
		if (bOpenRequested)
		{
			ImGui::OpenPopup("Import Texture Cube");
			bOpenRequested = false;
		}

		const MonaImGui::FUIStyleMetrics Metrics = MonaImGui::GetUIStyleMetrics();
		ImGui::SetNextWindowSize(ImVec2(Metrics.WidePopupWidth, 0.0f), ImGuiCond_Appearing);
		if (!ImGui::BeginPopupModal("Import Texture Cube", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings))
			return;

		ImGui::TextUnformatted("Create a TextureCube asset from six faces or a 2:1 panorama.");
		ImGui::TextDisabled("Authoritative source images are copied beside the .dasset package.");
		ImGui::Spacing();
		ImGui::SeparatorText("Source");
		const char* SourceMode = SourceLayout == ETextureCubeSourceLayout::SixFaces
			? "Six Faces" : "Equirectangular Panorama";
		if (ImGui::BeginCombo("Source mode", SourceMode))
		{
			for (const ETextureCubeSourceLayout Layout :
				{ETextureCubeSourceLayout::SixFaces, ETextureCubeSourceLayout::EquirectangularPanorama})
			{
				const char* Label = Layout == ETextureCubeSourceLayout::SixFaces
					? "Six Faces" : "Equirectangular Panorama";
				const bool bSelected = SourceLayout == Layout;
				if (ImGui::Selectable(Label, bSelected))
				{
					SourceLayout = Layout;
					RevalidateSources();
				}
				if (bSelected) ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}

		if (SourceLayout == ETextureCubeSourceLayout::SixFaces)
		{
			ImGui::TextDisabled("Images use normal top-to-bottom row order.");
			if (ImGui::BeginTable("TextureCubeFaces", 4,
				ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg))
			{
				ImGui::TableSetupColumn("Face", ImGuiTableColumnFlags_WidthFixed, MonaImGui::ScaleUI(42.0f));
				ImGui::TableSetupColumn("Direction", ImGuiTableColumnFlags_WidthFixed, MonaImGui::ScaleUI(72.0f));
				ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableSetupColumn("Orientation", ImGuiTableColumnFlags_WidthFixed, MonaImGui::ScaleUI(132.0f));
				ImGui::TableHeadersRow();
				for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
				{
					ImGui::PushID(static_cast<int>(Index));
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(FaceLabels[Index].data());
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(FaceDirections[Index].data());
					ImGui::TableNextColumn();
					const char* Path = FacePathBuffers[Index].data();
					const std::string FileName = Path[0] == '\0'
						? std::string("Choose image...")
						: std::filesystem::path(Path).filename().generic_string();
					ImGui::SetNextItemWidth(-FLT_MIN);
					if (ImGui::Button(FileName.c_str(), ImVec2(-FLT_MIN, 0.0f)))
						BrowseFace(static_cast<ETextureCubeFace>(Index));
					if (Path[0] != '\0' && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Path);
					ImGui::TableNextColumn();
					ImGui::TextDisabled("%s", FaceOrientationHints[Index].data());
					ImGui::PopID();
				}
				ImGui::EndTable();
			}
		}
		else
		{
			const char* Path = PanoramaPathBuffer.data();
			const std::string FileName = Path[0] == '\0'
				? std::string("Choose panorama...")
				: std::filesystem::path(Path).filename().generic_string();
			ImGui::TextUnformatted("Panorama");
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (ImGui::Button(FileName.c_str(), ImVec2(-FLT_MIN, 0.0f))) BrowsePanorama();
			if (Path[0] != '\0' && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Path);

			int FaceDimension = static_cast<int>(PanoramaFaceDimension);
			if (ImGui::DragInt("Face dimension", &FaceDimension, 1.0f, 0, 4096,
				"%d px", ImGuiSliderFlags_AlwaysClamp))
			{
				PanoramaFaceDimension = static_cast<uint32>(FaceDimension);
				RevalidateSources();
			}
			ImGui::TextDisabled("0 (Auto) derives one quarter of the panorama width.");

			const bool bHDRSource = IsRadianceHDRPath(PanoramaPathBuffer.data());
			ImGui::BeginDisabled(!bHDRSource);
			if (ImGui::DragFloat("Exposure", &PanoramaExposureEV, 0.1f, -16.0f, 16.0f,
				"%+.1f EV", ImGuiSliderFlags_AlwaysClamp))
				RevalidateSources();
			ImGui::EndDisabled();
			if (!bHDRSource) ImGui::TextDisabled("Exposure applies only to Radiance HDR sources.");
		}

		if (bSourcesValid)
		{
			if (SourceLayout == ETextureCubeSourceLayout::EquirectangularPanorama)
			{
				ImGui::SeparatorText("Projection summary");
				ImGui::TextDisabled("Source: %ux%u %s panorama.", ValidatedSourceWidth,
					ValidatedSourceHeight, bValidatedHDR ? "Radiance HDR" : "LDR");
				ImGui::TextDisabled("Projection: longitude wraps; +Y is the north pole; pixel-center bilinear sampling.");
			}
			ImGui::TextDisabled("Output: 6 faces, %ux%u, %u mips, %s (LDR).",
				ValidatedDimension, ValidatedDimension, ValidatedMipCount,
				GetPixelFormatInfo(ValidatedPixelFormat).Name);
		}
		else
		{
			ImGui::PushStyleColor(ImGuiCol_Text, MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::Warning));
			ImGui::TextWrapped("%s", SourceValidationMessage.c_str());
			ImGui::PopStyleColor();
		}

		ImGui::Spacing();
		ImGui::SeparatorText("Destination");
		const float BrowseButtonWidth = Metrics.StandardButtonWidth;
		ImGui::TextUnformatted("Asset path");
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - BrowseButtonWidth - ImGui::GetStyle().ItemSpacing.x);
		ImGui::InputTextWithHint("##TextureCubeAssetPath", "/Game/Textures/Sky", AssetPathBuffer.data(), AssetPathBuffer.size());
		ImGui::SameLine();
		if (ImGui::Button("Choose...", ImVec2(BrowseButtonWidth, 0.0f))) BrowseDestination();

		FAssetPath ParsedAssetPath;
		std::string AssetPathError;
		const bool bAssetPathValid = FAssetPath::TryCreate(AssetPathBuffer.data(), ParsedAssetPath, &AssetPathError);
		bool bMountedDestination = false;
		if (bAssetPathValid)
			for (const PathUtilities::FMountPoint& Mount : PathUtilities::GetRegisteredMountPoints())
				if (ParsedAssetPath.GetView().starts_with(Mount.VirtualRoot))
				{
					bMountedDestination = true;
					break;
				}
		const bool bAssetExists = bAssetPathValid &&
			(Asset::GetAssetRegistry().FindAsset(ParsedAssetPath) || Asset::FindLoadedPackage(ParsedAssetPath));

		std::string DestinationValidation;
		if (!bAssetPathValid) DestinationValidation = AssetPathError;
		else if (!bMountedDestination) DestinationValidation = "Choose a destination inside a mounted Content directory.";
		else if (bAssetExists) DestinationValidation = "An asset already exists at this path.";
		if (!DestinationValidation.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::Warning));
			ImGui::TextWrapped("%s", DestinationValidation.c_str());
			ImGui::PopStyleColor();
		}

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::BeginDisabled(!bSourcesValid || !DestinationValidation.empty());
		if (ImGui::Button("Import Texture Cube", ImVec2(MonaImGui::ScaleUI(170.0f), 0.0f)) && Import())
			ImGui::CloseCurrentPopup();
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (MonaImGui::DialogButton("Cancel", true)) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	auto FTextureCubeImportDialog::BrowseFace(ETextureCubeFace Face) -> void
	{
		const size_t Index = FaceIndex(Face);
		FFileDialogRequest Request;
		Request.ParentWindowHandle = ImGui::GetMainViewport()->PlatformHandleRaw;
		Request.Title = std::format("Select {} ({}) Cube Face", FaceLabels[Index], FaceDirections[Index]);
		Request.Filters = {
			{"All Supported Images", "*.png;*.jpg;*.jpeg;*.bmp;*.tga"},
			{"PNG", "*.png"}, {"JPEG", "*.jpg;*.jpeg"}, {"Bitmap", "*.bmp"},
			{"Targa", "*.tga"}, {"All Files", "*.*"}};
		if (const FProjectInfo* Project = GetCurrentProject()) Request.InitialDirectory = Project->ProjectDir;
		if (FacePathBuffers[Index][0] != '\0')
			Request.InitialDirectory = std::filesystem::path(FacePathBuffers[Index].data()).parent_path().generic_string();
		const FFileDialogResult Result = OpenFileDialog(Request);
		if (Result.Status == EFileDialogStatus::Cancelled) return;
		if (Result.Status == EFileDialogStatus::Error) { SetError(Result.ErrorMessage); return; }
		if (Result.FilePath.size() >= FacePathBuffers[Index].size())
		{
			SetError("The selected file path is too long for the cube import form.");
			return;
		}

		FacePathBuffers[Index].fill(0);
		std::memcpy(FacePathBuffers[Index].data(), Result.FilePath.data(), Result.FilePath.size());
		if (AssetPathBuffer[0] == '\0')
			SuggestAssetPath(Result.FilePath);
		RevalidateSources();
	}

	auto FTextureCubeImportDialog::BrowsePanorama() -> void
	{
		FFileDialogRequest Request;
		Request.ParentWindowHandle = ImGui::GetMainViewport()->PlatformHandleRaw;
		Request.Title = "Select an Equirectangular Panorama";
		Request.Filters = {
			{"All Supported Panoramas", "*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.hdr"},
			{"Radiance HDR", "*.hdr"}, {"PNG", "*.png"}, {"JPEG", "*.jpg;*.jpeg"},
			{"Bitmap", "*.bmp"}, {"Targa", "*.tga"}, {"All Files", "*.*"}};
		if (const FProjectInfo* Project = GetCurrentProject()) Request.InitialDirectory = Project->ProjectDir;
		if (PanoramaPathBuffer[0] != '\0')
			Request.InitialDirectory = std::filesystem::path(PanoramaPathBuffer.data()).parent_path().generic_string();
		const FFileDialogResult Result = OpenFileDialog(Request);
		if (Result.Status == EFileDialogStatus::Cancelled) return;
		if (Result.Status == EFileDialogStatus::Error) { SetError(Result.ErrorMessage); return; }
		if (Result.FilePath.size() >= PanoramaPathBuffer.size())
		{
			SetError("The selected file path is too long for the panorama import form.");
			return;
		}

		PanoramaPathBuffer.fill(0);
		std::memcpy(PanoramaPathBuffer.data(), Result.FilePath.data(), Result.FilePath.size());
		if (AssetPathBuffer[0] == '\0') SuggestAssetPath(Result.FilePath);
		RevalidateSources();
	}

	auto FTextureCubeImportDialog::BrowseDestination() -> void
	{
		FFileDialogRequest Request;
		Request.ParentWindowHandle = ImGui::GetMainViewport()->PlatformHandleRaw;
		Request.Title = "Choose a Texture Cube Asset Path";
		Request.Filters = {{"Durin Asset", "*.dasset"}};
		FAssetPath CurrentAssetPath;
		Request.DefaultFileName = FAssetPath::TryCreate(AssetPathBuffer.data(), CurrentAssetPath)
			? std::string(CurrentAssetPath.GetAssetName()) + ".dasset" : "TextureCube.dasset";
		for (const PathUtilities::FMountPoint& Mount : PathUtilities::GetRegisteredMountPoints())
			if (const FProjectInfo* Project = GetCurrentProject(); Project && Mount.VirtualRoot == Project->MountRoot)
			{
				Request.InitialDirectory = Mount.PhysicalPath;
				break;
			}
		const FFileDialogResult Result = SaveFileDialog(Request);
		if (Result.Status == EFileDialogStatus::Cancelled) return;
		if (Result.Status == EFileDialogStatus::Error) { SetError(Result.ErrorMessage); return; }
		for (const PathUtilities::FMountPoint& Mount : PathUtilities::GetRegisteredMountPoints())
		{
			const std::filesystem::path Selected = std::filesystem::absolute(Result.FilePath).lexically_normal();
			const std::filesystem::path Root = std::filesystem::absolute(Mount.PhysicalPath).lexically_normal();
			const std::filesystem::path Relative = Selected.lexically_relative(Root);
			if (Relative.empty() || Relative.generic_string().starts_with("..")) continue;
			std::filesystem::path VirtualRelative = Relative;
			VirtualRelative.replace_extension();
			const std::string VirtualPath = Mount.VirtualRoot + VirtualRelative.generic_string();
			if (VirtualPath.size() >= AssetPathBuffer.size())
			{
				SetError("The selected asset path is too long for the cube import form.");
				return;
			}
			AssetPathBuffer.fill(0);
			std::memcpy(AssetPathBuffer.data(), VirtualPath.data(), VirtualPath.size());
			return;
		}
		SetError("Texture Cube assets must be saved inside a mounted Content directory.");
	}

	auto FTextureCubeImportDialog::RevalidateSources() -> bool
	{
		FTextureCubeImportValidation Validation;
		if (SourceLayout == ETextureCubeSourceLayout::SixFaces)
		{
			std::array<std::string, TextureCubeFaceCount> Faces;
			for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
				Faces[Index] = FacePathBuffers[Index].data();
			Validation = DTextureCube::ValidateImportSources(Faces);
		}
		else
		{
			const FTextureCubePanoramaImportSettings Settings{
				.FaceDimension = PanoramaFaceDimension,
				.ExposureEV = IsRadianceHDRPath(PanoramaPathBuffer.data()) ? PanoramaExposureEV : 0.0f};
			Validation = DTextureCube::ValidatePanoramaImportSource(PanoramaPathBuffer.data(), Settings);
		}
		bSourcesValid = static_cast<bool>(Validation);
		SourceValidationMessage = Validation.Message;
		ValidatedSourceWidth = Validation.SourceWidth;
		ValidatedSourceHeight = Validation.SourceHeight;
		ValidatedDimension = Validation.Dimension;
		ValidatedMipCount = Validation.MipCount;
		ValidatedPixelFormat = Validation.PixelFormat;
		bValidatedHDR = Validation.bHDR;
		return bSourcesValid;
	}

	auto FTextureCubeImportDialog::Import() -> bool
	{
		if (ClearError) ClearError();
		if (!RevalidateSources()) return false;
		FTextureCubeImportResult Result;
		if (SourceLayout == ETextureCubeSourceLayout::SixFaces)
		{
			std::array<std::string, TextureCubeFaceCount> Faces;
			for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
				Faces[Index] = FacePathBuffers[Index].data();
			Result = DTextureCube::ImportAsset(Faces, AssetPathBuffer.data());
		}
		else
		{
			const FTextureCubePanoramaImportSettings Settings{
				.FaceDimension = PanoramaFaceDimension,
				.ExposureEV = bValidatedHDR ? PanoramaExposureEV : 0.0f};
			Result = DTextureCube::ImportPanoramaAsset(
				PanoramaPathBuffer.data(), AssetPathBuffer.data(), Settings);
		}
		if (!Result) { SetError(Result.Message); return false; }
		if (Imported) Imported(AssetPathBuffer.data());
		FAssetPath ImportedPath;
		if (FAssetPath::TryCreate(AssetPathBuffer.data(), ImportedPath)) Asset::UnloadPackage(ImportedPath);
		return true;
	}

	auto FTextureCubeImportDialog::SuggestAssetPath(std::string_view SourceFile) -> void
	{
		const std::string AssetName = StringUtils::SanitizeFileName(
			std::filesystem::path(SourceFile).stem().generic_string(), "TextureCube");
		const FProjectInfo* Project = GetCurrentProject();
		const std::string SuggestedPath = !PreferredDestinationDirectory.empty()
			? PreferredDestinationDirectory + AssetName
			: (Project ? Project->MountRoot : "/") + "Textures/" + AssetName;
		AssetPathBuffer.fill(0);
		std::memcpy(AssetPathBuffer.data(), SuggestedPath.data(),
			std::min(SuggestedPath.size(), AssetPathBuffer.size() - 1));
	}

	auto FTextureCubeImportDialog::SetError(std::string Message) const -> void
	{
		if (ReportError) ReportError(std::move(Message));
	}
}
