#include "Import/TextureImportDialog.h"

#include "AssetForge/Builtins/TextureCubeImport.h"
#include "AssetForge/Builtins/TextureCubeFactory.h"

#include "AssetTools/IAssetTools.h"
#include "Asset/AssetOperations.h"
#include "Editor/Import/AssetDestinationValidation.h"
#include "Asset.h"
#include "Dialogs/FileDialog.h"
#include "Misc/Paths.h"
#include "Misc/Project.h"
#include "Misc/StringConvert.h"
#include "Misc/StringHelper.h"
#include "MonaImGui.h"
#include "PixelFormat.h"

namespace Durin::Editor::Texture
{
	namespace
	{
		constexpr std::array<std::string_view, TextureCubeFaceCount> FaceLabels = {
			"+X", "-X", "+Y", "-Y", "+Z", "-Z"};
		constexpr std::array<std::string_view, TextureCubeFaceCount> FaceDirections = {
			"Forward", "Backward", "Right", "Left", "Up", "Down"};
		constexpr std::array<std::string_view, TextureCubeFaceCount>
			FaceOrientationHints = {
				"top +Y, right -Z", "top +Y, right +Z", "top -Z, right +X",
				"top +Z, right +X", "top +Y, right +X", "top +Y, right -X"};

		auto FaceIndex(ETextureCubeFace Face) -> size_t
		{
			return static_cast<size_t>(Face);
		}

		auto IsRadianceHDRPath(std::string_view Path) -> bool
		{
			const std::string Extension = StringUtils::FoldAscii(
				std::filesystem::path(Path).extension().generic_string());
			return Extension == ".hdr";
		}

	} // namespace

	auto FTextureImportDialog::DrawTextureCubeSource() -> void
	{
		FTextureCubeImportFormState& Cube = State.GetTextureCube();
		ImGui::TextUnformatted("Source format");
		if (ImGui::RadioButton("Panorama (2:1)",
			Cube.SourceLayout ==
				ETextureCubeSourceLayout::EquirectangularPanorama))
		{
			Cube.SourceLayout =
				ETextureCubeSourceLayout::EquirectangularPanorama;
			RevalidateTextureCubeSources();
		}
		ImGui::SameLine();
		if (ImGui::RadioButton("Six face images",
			Cube.SourceLayout == ETextureCubeSourceLayout::SixFaces))
		{
			Cube.SourceLayout = ETextureCubeSourceLayout::SixFaces;
			RevalidateTextureCubeSources();
		}

		if (Cube.SourceLayout == ETextureCubeSourceLayout::SixFaces)
		{
			ImGui::TextDisabled("Images use normal top-to-bottom row order.");
			if (ImGui::BeginTable("TextureCubeFaces", 4,
				ImGuiTableFlags_SizingStretchProp |
				ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg))
			{
				ImGui::TableSetupColumn("Face",
					ImGuiTableColumnFlags_WidthFixed,
					MonaImGui::ScaleUI(42.0f));
				ImGui::TableSetupColumn("Direction",
					ImGuiTableColumnFlags_WidthFixed,
					MonaImGui::ScaleUI(72.0f));
				ImGui::TableSetupColumn("Source",
					ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableSetupColumn("Orientation",
					ImGuiTableColumnFlags_WidthFixed,
					MonaImGui::ScaleUI(132.0f));
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
					const char* Path = Cube.FacePathBuffers[Index].data();
					const std::string FileName = Path[0] == '\0'
						? std::string("Choose image...")
						: std::filesystem::path(Path).filename().generic_string();
					ImGui::SetNextItemWidth(-FLT_MIN);
					if (ImGui::Button(FileName.c_str(), ImVec2(-FLT_MIN, 0.0f)))
						BrowseFace(static_cast<ETextureCubeFace>(Index));
					if (Path[0] != '\0' && ImGui::IsItemHovered())
						ImGui::SetTooltip("%s", Path);
					ImGui::TableNextColumn();
					ImGui::TextDisabled("%s", FaceOrientationHints[Index].data());
					ImGui::PopID();
				}
				ImGui::EndTable();
			}
		}
		else
		{
			const char* Path = Cube.PanoramaPathBuffer.data();
			const std::string FileName = Path[0] == '\0'
				? std::string("Choose panorama...")
				: std::filesystem::path(Path).filename().generic_string();
			ImGui::TextUnformatted("Panorama");
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (ImGui::Button(FileName.c_str(), ImVec2(-FLT_MIN, 0.0f)))
				BrowsePanorama();
			if (Path[0] != '\0' && ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", Path);

			bool bAutomaticFaceDimension = Cube.PanoramaFaceDimension == 0;
			if (ImGui::Checkbox("Automatic face size", &bAutomaticFaceDimension))
			{
				if (bAutomaticFaceDimension)
					Cube.PanoramaFaceDimension = 0;
				else
				{
					Cube.PanoramaCustomFaceDimension =
						Cube.PanoramaCustomFaceDimension > 0
							? Cube.PanoramaCustomFaceDimension
							: 1024;
					Cube.PanoramaFaceDimension =
						Cube.PanoramaCustomFaceDimension;
				}
				RevalidateTextureCubeSources();
			}
			if (bAutomaticFaceDimension)
			{
				if (Cube.bSourcesValid)
					ImGui::TextDisabled(
						"%u x %u px, derived from one quarter of the panorama width.",
						Cube.ValidatedDimension, Cube.ValidatedDimension);
				else
					ImGui::TextDisabled(
						"Derives the face size from one quarter of the panorama width.");
			}
			else
			{
				int FaceDimension =
					static_cast<int>(Cube.PanoramaFaceDimension);
				if (ImGui::DragInt("Custom face size", &FaceDimension,
					1.0f, 1, 4096, "%d px", ImGuiSliderFlags_AlwaysClamp))
				{
					Cube.PanoramaFaceDimension =
						static_cast<uint32>(FaceDimension);
					Cube.PanoramaCustomFaceDimension =
						Cube.PanoramaFaceDimension;
				}
				if (ImGui::IsItemDeactivatedAfterEdit())
					RevalidateTextureCubeSources();
			}

			const bool bHDRSource =
				IsRadianceHDRPath(Cube.PanoramaPathBuffer.data());
			ImGui::BeginDisabled(!bHDRSource);
			ImGui::DragFloat("Exposure", &Cube.PanoramaExposureEV,
				0.1f, -16.0f, 16.0f, "%+.1f EV",
				ImGuiSliderFlags_AlwaysClamp);
			if (ImGui::IsItemDeactivatedAfterEdit())
				RevalidateTextureCubeSources();
			ImGui::EndDisabled();
			if (!bHDRSource)
				ImGui::TextDisabled(
					"Exposure applies only to Radiance HDR sources.");
		}

		if (Cube.bSourcesValid)
			ImGui::TextDisabled("The built-in importer verified the TextureCube sources and output policy.");
		else
		{
			ImGui::PushStyleColor(ImGuiCol_Text,
				MonaImGui::GetThemeColor(
					MonaImGui::EUIThemeColor::Warning));
			ImGui::TextWrapped("%s", Cube.SourceValidationMessage.c_str());
			ImGui::PopStyleColor();
		}
	}

	auto FTextureImportDialog::ValidateAndDrawTextureCubeDestination()
		-> std::string
	{
		FTextureCubeImportFormState& Cube = State.GetTextureCube();
		const FAssetDestinationValidation DestinationValidation =
			Destination.Inspect();
		std::string ValidationMessage;
		if (!DestinationValidation)
			ValidationMessage = DestinationValidation.Message;
		else if (!Cube.bSourcesValid)
			ValidationMessage = Cube.SourceValidationMessage;

		if (DestinationValidation.bMountedDestination && Cube.bSourcesValid)
		{
			ImGui::BeginChild("TextureCubeOutputPreview",
				ImVec2(0.0f, MonaImGui::ScaleUI(84.0f)),
				ImGuiChildFlags_Borders);
			ImGui::TextDisabled("Asset identity");
			ImGui::TextUnformatted(
				DestinationValidation.AssetPath.ToString().c_str());
			ImGui::TextDisabled("Package file");
			ImGui::TextUnformatted(std::format("{}.dasset",
				DestinationValidation.AssetPath.ToString()).c_str());
			ImGui::EndChild();
		}
		return ValidationMessage;
	}

	auto FTextureImportDialog::BrowseFace(ETextureCubeFace Face) -> void
	{
		FTextureCubeImportFormState& Cube = State.GetTextureCube();
		const size_t Index = FaceIndex(Face);
		FFileDialogRequest Request;
		Request.ParentWindowHandle = ImGui::GetMainViewport()->PlatformHandleRaw;
		Request.Title = std::format("Select {} ({}) Cube Face",
			FaceLabels[Index], FaceDirections[Index]);
		Request.Filters = {
			{"All Supported Images", "*.png;*.jpg;*.jpeg;*.bmp;*.tga"},
			{"PNG", "*.png"}, {"JPEG", "*.jpg;*.jpeg"},
			{"Bitmap", "*.bmp"}, {"Targa", "*.tga"}, {"All Files", "*.*"}};
		if (const FProjectInfo* Project = GetCurrentProject())
			Request.InitialDirectory = Project->ProjectDir;
		if (Cube.FacePathBuffers[Index][0] != '\0')
			Request.InitialDirectory = std::filesystem::path(
				Cube.FacePathBuffers[Index].data())
				.parent_path().generic_string();
		const FFileDialogResult Result = OpenFileDialog(Request);
		if (Result.Status == EFileDialogStatus::Cancelled) return;
		if (Result.Status == EFileDialogStatus::Error)
		{
			SetError(Result.ErrorMessage);
			return;
		}
		if (Result.FilePath.size() >= Cube.FacePathBuffers[Index].size())
		{
			SetError("The selected file path is too long for the cube import form.");
			return;
		}

		Cube.FacePathBuffers[Index].fill(0);
		std::memcpy(Cube.FacePathBuffers[Index].data(),
			Result.FilePath.data(), Result.FilePath.size());
		if (Destination.GetPath().empty())
			SuggestTextureCubeAssetPath(Result.FilePath);
		RevalidateTextureCubeSources();
	}

	auto FTextureImportDialog::BrowsePanorama() -> void
	{
		FTextureCubeImportFormState& Cube = State.GetTextureCube();
		FFileDialogRequest Request;
		Request.ParentWindowHandle = ImGui::GetMainViewport()->PlatformHandleRaw;
		Request.Title = "Select an Equirectangular Panorama";
		Request.Filters = {
			{"All Supported Panoramas", "*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.hdr"},
			{"Radiance HDR", "*.hdr"}, {"PNG", "*.png"},
			{"JPEG", "*.jpg;*.jpeg"}, {"Bitmap", "*.bmp"},
			{"Targa", "*.tga"}, {"All Files", "*.*"}};
		if (const FProjectInfo* Project = GetCurrentProject())
			Request.InitialDirectory = Project->ProjectDir;
		if (Cube.PanoramaPathBuffer[0] != '\0')
			Request.InitialDirectory = std::filesystem::path(
				Cube.PanoramaPathBuffer.data())
				.parent_path().generic_string();
		const FFileDialogResult Result = OpenFileDialog(Request);
		if (Result.Status == EFileDialogStatus::Cancelled) return;
		if (Result.Status == EFileDialogStatus::Error)
		{
			SetError(Result.ErrorMessage);
			return;
		}
		if (Result.FilePath.size() >= Cube.PanoramaPathBuffer.size())
		{
			SetError("The selected file path is too long for the panorama import form.");
			return;
		}

		Cube.PanoramaPathBuffer.fill(0);
		std::memcpy(Cube.PanoramaPathBuffer.data(), Result.FilePath.data(),
			Result.FilePath.size());
		if (Destination.GetPath().empty())
			SuggestTextureCubeAssetPath(Result.FilePath);
		RevalidateTextureCubeSources();
	}

	auto FTextureImportDialog::RevalidateTextureCubeSources() -> bool
	{
		FTextureCubeImportFormState& Cube = State.GetTextureCube();
		std::array<std::string, TextureCubeFaceCount> Files;
		const size_t SourceCount = Cube.SourceLayout == ETextureCubeSourceLayout::SixFaces
			? TextureCubeFaceCount : 1;
		if (Cube.SourceLayout == ETextureCubeSourceLayout::SixFaces)
			for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
				Files[Index] = Cube.FacePathBuffers[Index].data();
		else Files[0] = Cube.PanoramaPathBuffer.data();
		if (std::ranges::any_of(std::span(Files).first(SourceCount),
			[](const std::string& File) { return File.empty(); }))
		{
			Cube.bSourcesValid = false;
			Cube.SourceValidationMessage = "Select every required TextureCube source.";
			return false;
		}
		Cube.bSourcesValid = true;
		Cube.SourceValidationMessage =
			"Sources selected. Compatibility will be checked when import starts.";
		return true;
	}

	auto FTextureImportDialog::ImportTextureCube() -> bool
	{
		FTextureCubeImportFormState& Cube = State.GetTextureCube();
		if (!RevalidateTextureCubeSources()) return false;
		const FAssetDestinationValidation DestinationValidation =
			Destination.Inspect();
		if (!DestinationValidation)
		{
			SetError(DestinationValidation.Message);
			return false;
		}
		const FAssetPath& AssetPath = DestinationValidation.AssetPath;
		std::array<std::string, TextureCubeFaceCount> Sources;
		AssetForge::Builtins::FTextureCubePanoramaImportSettings PanoramaSettings;
		if (Cube.SourceLayout == ETextureCubeSourceLayout::SixFaces)
		{
			for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
			{
				Sources[Index] = Cube.FacePathBuffers[Index].data();
			}
		}
		else
		{
			Sources[0] = Cube.PanoramaPathBuffer.data();
			PanoramaSettings = {
				.FaceDimension = Cube.PanoramaFaceDimension,
				.ExposureEV = IsRadianceHDRPath(Cube.PanoramaPathBuffer.data())
					? Cube.PanoramaExposureEV : 0.0f};
		}
		const std::string Path = AssetPath.ToString();
		auto* Factory = NewObject<AssetForge::Builtins::DTextureCubeFactory>(
			nullptr, "TextureCubeDialogFactory", EObjectFlags::Transient);
		if (Cube.SourceLayout == ETextureCubeSourceLayout::SixFaces)
			Factory->ConfigureFaces(Sources);
		else
			Factory->ConfigurePanorama(PanoramaSettings);
		const FAssetToolsResult Result = IAssetTools::Get().ImportAsset(
			AssetPath, DTextureCube::StaticClass(), Sources[0], Factory);
		if (!Result)
		{
			SetError(Result.Message.empty()
				? "TextureCube import failed." : Result.Message);
			return false;
		}
		if (const FAssetOperationResult Saved = IAssetTools::Get().SaveAssets({
				.AssetPaths = {AssetPath},
				.Publish = [this, &Path](const FAssetOperationNotification&) {
					Callbacks.NotifyAssetCreated(Path);
				}});
			!Saved)
		{
			SetError(Saved.Message.empty()
				? "TextureCube was created but its package could not be saved."
				: Saved.Message);
			return false;
		}
		(void)Asset::UnloadPackage(AssetPath);
		return true;
	}

	auto FTextureImportDialog::SuggestTextureCubeAssetPath(
		std::string_view SourceFile) -> void
	{
		const std::string AssetName = StringUtils::SanitizeFileName(
			std::filesystem::path(SourceFile).stem().generic_string(),
			"TextureCube");
		const FProjectInfo* Project = GetCurrentProject();
		Destination.SuggestPath(Destination.MakeSuggestedPath(AssetName,
			(Project ? Project->MountRoot : "/") + std::string("Textures/")));
	}
} // namespace Durin::Editor::Texture
