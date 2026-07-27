#include "Assets/TextureCubeImportDialog.h"

#include "Assets/MountedSourceImport.h"
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
		constexpr std::array<std::string_view, TextureCubeFaceCount> FaceSuffixes = {
			"px", "nx", "py", "ny", "pz", "nz"};

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
		for (auto& Buffer : FaceDestinationBuffers) Buffer.fill(0);
		PanoramaPathBuffer.fill(0);
		PanoramaDestinationBuffer.fill(0);
		AssetPathBuffer.fill(0);
		SourceLayout = ETextureCubeSourceLayout::EquirectangularPanorama;
		SourceMode = EMountedSourceImportMode::IngestExternal;
		PanoramaFaceDimension = 0;
		PanoramaCustomFaceDimension = 0;
		PanoramaExposureEV = 0.0f;
		SourceValidationMessage = "Select a 2:1 panorama to continue.";
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
		ImGui::TextDisabled("Reference mounted images in place, or ingest external images into SourceAssets.");
		ImGui::Spacing();
		ImGui::SeparatorText("Source");
		if (ImGui::RadioButton("Reference Existing Source",
			SourceMode == EMountedSourceImportMode::ReferenceExisting))
			SourceMode = EMountedSourceImportMode::ReferenceExisting;
		ImGui::SameLine();
		if (ImGui::RadioButton("Ingest External Source",
			SourceMode == EMountedSourceImportMode::IngestExternal))
			SourceMode = EMountedSourceImportMode::IngestExternal;
		ImGui::TextDisabled(SourceMode == EMountedSourceImportMode::ReferenceExisting
			? "Keeps images in allowed mounted SourceAssets domains; no copies are created."
			: "Copies external images transactionally to explicit mounted source paths.");
		ImGui::TextUnformatted("Source format");
		if (ImGui::RadioButton("Panorama (2:1)",
			SourceLayout == ETextureCubeSourceLayout::EquirectangularPanorama))
		{
			SourceLayout = ETextureCubeSourceLayout::EquirectangularPanorama;
			RevalidateSources();
		}
		ImGui::SameLine();
		if (ImGui::RadioButton("Six face images",
			SourceLayout == ETextureCubeSourceLayout::SixFaces))
		{
			SourceLayout = ETextureCubeSourceLayout::SixFaces;
			RevalidateSources();
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

			bool bAutomaticFaceDimension = PanoramaFaceDimension == 0;
			if (ImGui::Checkbox("Automatic face size", &bAutomaticFaceDimension))
			{
				if (bAutomaticFaceDimension)
				{
					PanoramaFaceDimension = 0;
				}
				else
				{
					PanoramaCustomFaceDimension = PanoramaCustomFaceDimension > 0
						? PanoramaCustomFaceDimension
						: (bSourcesValid ? ValidatedDimension : 1024);
					PanoramaFaceDimension = PanoramaCustomFaceDimension;
				}
				RevalidateSources();
			}
			if (bAutomaticFaceDimension)
			{
				if (bSourcesValid)
					ImGui::TextDisabled("%u x %u px, derived from one quarter of the panorama width.",
						ValidatedDimension, ValidatedDimension);
				else
					ImGui::TextDisabled("Derives the face size from one quarter of the panorama width.");
			}
			else
			{
				int FaceDimension = static_cast<int>(PanoramaFaceDimension);
				if (ImGui::DragInt("Custom face size", &FaceDimension, 1.0f, 1, 4096,
					"%d px", ImGuiSliderFlags_AlwaysClamp))
				{
					PanoramaFaceDimension = static_cast<uint32>(FaceDimension);
					PanoramaCustomFaceDimension = PanoramaFaceDimension;
				}
				if (ImGui::IsItemDeactivatedAfterEdit()) RevalidateSources();
			}

			const bool bHDRSource = IsRadianceHDRPath(PanoramaPathBuffer.data());
			ImGui::BeginDisabled(!bHDRSource);
			ImGui::DragFloat("Exposure", &PanoramaExposureEV, 0.1f, -16.0f, 16.0f,
				"%+.1f EV", ImGuiSliderFlags_AlwaysClamp);
			if (ImGui::IsItemDeactivatedAfterEdit()) RevalidateSources();
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
		if (SourceMode == EMountedSourceImportMode::IngestExternal)
		{
			ImGui::TextUnformatted(SourceLayout == ETextureCubeSourceLayout::SixFaces
				? "Source virtual paths" : "Source virtual path");
			if (SourceLayout == ETextureCubeSourceLayout::SixFaces)
			{
				for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
				{
					ImGui::PushID(static_cast<int>(Index));
					ImGui::SetNextItemWidth(-FLT_MIN);
					ImGui::InputText(FaceLabels[Index].data(),
						FaceDestinationBuffers[Index].data(),
						FaceDestinationBuffers[Index].size());
					ImGui::PopID();
				}
			}
			else
			{
				ImGui::SetNextItemWidth(-FLT_MIN);
				ImGui::InputTextWithHint("##TextureCubePanoramaDestination",
					"/Game/Textures/Sky_panorama.hdr",
					PanoramaDestinationBuffer.data(),
					PanoramaDestinationBuffer.size());
			}
		}

		FAssetPath ParsedAssetPath;
		std::string AssetPathError;
		const bool bAssetPathValid = FAssetPath::TryCreate(AssetPathBuffer.data(), ParsedAssetPath, &AssetPathError);
		const bool bMountedDestination = bAssetPathValid;
		const bool bAssetExists = bAssetPathValid &&
			(Asset::GetAssetRegistry().FindAsset(ParsedAssetPath) || Asset::FindLoadedPackage(ParsedAssetPath));
		std::array<FMountedSourceImportDiagnostic, TextureCubeFaceCount> FaceDiagnostics;
		FMountedSourceImportDiagnostic PanoramaDiagnostic;
		if (bAssetPathValid)
		{
			if (SourceLayout == ETextureCubeSourceLayout::SixFaces)
			{
				for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
					FaceDiagnostics[Index] = InspectMountedSourceImport(
						FacePathBuffers[Index].data(), ParsedAssetPath.GetView(),
						FaceDestinationBuffers[Index].data(), SourceMode);
			}
			else
			{
				PanoramaDiagnostic = InspectMountedSourceImport(
					PanoramaPathBuffer.data(), ParsedAssetPath.GetView(),
					PanoramaDestinationBuffer.data(), SourceMode);
			}
		}
		const auto FirstInvalidFace = std::ranges::find_if(
			FaceDiagnostics, [](const FMountedSourceImportDiagnostic& Diagnostic) {
				return !Diagnostic.bValid;
			});
		const bool bMountedSourcesValid =
			SourceLayout == ETextureCubeSourceLayout::SixFaces
				? FirstInvalidFace == FaceDiagnostics.end()
				: PanoramaDiagnostic.bValid;

		std::string DestinationValidation;
		if (!bAssetPathValid) DestinationValidation = AssetPathError;
		else if (!bMountedDestination) DestinationValidation = "Choose a destination inside a mounted Content directory.";
		else if (bAssetExists) DestinationValidation = "An asset already exists at this path.";
		else if (!bMountedSourcesValid)
			DestinationValidation =
				SourceLayout == ETextureCubeSourceLayout::SixFaces
					? FirstInvalidFace->Message
					: PanoramaDiagnostic.Message;
		if (bMountedDestination && bMountedSourcesValid)
		{
			ImGui::BeginChild("TextureCubeOutputPreview",
				ImVec2(0.0f, MonaImGui::ScaleUI(
					SourceLayout == ETextureCubeSourceLayout::SixFaces ? 170.0f : 78.0f)),
				ImGuiChildFlags_Borders);
			ImGui::TextDisabled("Asset destination");
			ImGui::TextUnformatted(ParsedAssetPath.ToString().c_str());
			ImGui::TextDisabled("Source virtual path%s",
				SourceLayout == ETextureCubeSourceLayout::SixFaces ? "s" : "");
			if (SourceLayout == ETextureCubeSourceLayout::SixFaces)
			{
				for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
					ImGui::TextUnformatted(std::format(
						"{}  {}", FaceLabels[Index],
						FaceDiagnostics[Index].VirtualPath).c_str());
			}
			else
			{
				ImGui::TextUnformatted(PanoramaDiagnostic.VirtualPath.c_str());
			}
			ImGui::EndChild();
			const FMountedSourceImportDiagnostic& Summary =
				SourceLayout == ETextureCubeSourceLayout::SixFaces
					? FaceDiagnostics.front() : PanoramaDiagnostic;
			ImGui::TextDisabled("Mount: %s (%s)  |  %s  |  dependency allowed",
				Summary.Mount->VirtualRoot.c_str(),
				DescribeMountOwner(Summary.Mount->Owner),
				Summary.Mount->bSourceWritable ? "writable" : "read-only");
		}
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
		if (SourceMode == EMountedSourceImportMode::ReferenceExisting)
		{
			const PathUtilities::FMountLookupResult Lookup =
				PathUtilities::FindMountForVirtualPath(AssetPathBuffer.data());
			if (Lookup && Lookup.Mount->SourceAssetsRoot)
				Request.InitialDirectory = Lookup.Mount->SourceAssetsRoot->generic_string();
		}
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
		SuggestSourceDestinations();
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
		if (SourceMode == EMountedSourceImportMode::ReferenceExisting)
		{
			const PathUtilities::FMountLookupResult Lookup =
				PathUtilities::FindMountForVirtualPath(AssetPathBuffer.data());
			if (Lookup && Lookup.Mount->SourceAssetsRoot)
				Request.InitialDirectory = Lookup.Mount->SourceAssetsRoot->generic_string();
		}
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
		SuggestSourceDestinations();
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
			if (VirtualPathString.size() >= AssetPathBuffer.size())
			{
				SetError("The selected asset path is too long for the cube import form.");
				return;
			}
			AssetPathBuffer.fill(0);
			std::memcpy(AssetPathBuffer.data(), VirtualPathString.data(), VirtualPathString.size());
			SuggestSourceDestinations();
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
			std::array<std::string, TextureCubeFaceCount> Destinations;
			for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
			{
				Faces[Index] = FacePathBuffers[Index].data();
				if (SourceMode == EMountedSourceImportMode::IngestExternal)
					Destinations[Index] = FaceDestinationBuffers[Index].data();
			}
			Result = DTextureCube::ImportAsset(
				Faces, AssetPathBuffer.data(), {}, Destinations);
		}
		else
		{
			const FTextureCubePanoramaImportSettings Settings{
				.FaceDimension = PanoramaFaceDimension,
				.ExposureEV = bValidatedHDR ? PanoramaExposureEV : 0.0f};
			Result = DTextureCube::ImportPanoramaAsset(
				PanoramaPathBuffer.data(), AssetPathBuffer.data(), Settings,
				SourceMode == EMountedSourceImportMode::IngestExternal
					? PanoramaDestinationBuffer.data() : std::string_view{});
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
		SuggestSourceDestinations();
	}

	auto FTextureCubeImportDialog::SuggestSourceDestinations() -> void
	{
		FAssetPath AssetPath;
		if (!FAssetPath::TryCreate(AssetPathBuffer.data(), AssetPath)) return;
		const std::string AssetName(AssetPath.GetAssetName());
		if (PanoramaDestinationBuffer[0] == '\0'
			&& PanoramaPathBuffer[0] != '\0')
		{
			const std::string Suggested = MakeDefaultSourceVirtualPath(
				AssetPath.GetView(), "Textures",
				AssetName + "_panorama"
					+ std::filesystem::path(PanoramaPathBuffer.data()).extension().generic_string());
			std::memcpy(PanoramaDestinationBuffer.data(), Suggested.data(),
				std::min(Suggested.size(), PanoramaDestinationBuffer.size() - 1));
		}
		for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
		{
			if (FaceDestinationBuffers[Index][0] != '\0'
				|| FacePathBuffers[Index][0] == '\0') continue;
			const std::string Suggested = MakeDefaultSourceVirtualPath(
				AssetPath.GetView(), "Textures",
				std::format("{}_{}{}", AssetName, FaceSuffixes[Index],
					std::filesystem::path(FacePathBuffers[Index].data())
						.extension().generic_string()));
			std::memcpy(FaceDestinationBuffers[Index].data(), Suggested.data(),
				std::min(Suggested.size(), FaceDestinationBuffers[Index].size() - 1));
		}
	}

	auto FTextureCubeImportDialog::SetError(std::string Message) const -> void
	{
		if (ReportError) ReportError(std::move(Message));
	}
}
