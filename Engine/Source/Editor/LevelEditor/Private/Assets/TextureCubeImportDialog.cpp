#include "Assets/TextureCubeImportDialog.h"

#include "TextureCubeSourceTranslation.h"

#include "Assets/AssetDestinationValidation.h"
#include "Assets/MountedSourceImport.h"
#include "AssetAuthoring.h"
#include "Dialogs/FileDialog.h"
#include "Misc/Paths.h"
#include "Misc/Project.h"
#include "Misc/StringConvert.h"
#include "MonaImGui.h"
#include "PixelFormat.h"
#include "Texture/TextureCube.h"

namespace Durin::Editor::Level
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

		auto ApplySuggestedPath(std::array<char, 512>& Buffer,
			std::string& LastSuggestedPath, std::string_view SuggestedPath) -> void
		{
			const std::string_view CurrentPath = Buffer.data();
			if (CurrentPath.empty() || CurrentPath == LastSuggestedPath)
			{
				Buffer.fill(0);
				std::memcpy(Buffer.data(), SuggestedPath.data(),
					std::min(SuggestedPath.size(), Buffer.size() - 1));
			}
			LastSuggestedPath = SuggestedPath;
		}
	}

	FTextureCubeImportDialog::FTextureCubeImportDialog(
		FImportDialogCallbacks InCallbacks)
		: Callbacks(std::move(InCallbacks))
	{
	}

	auto FTextureCubeImportDialog::Open(std::string_view DestinationDirectory) -> void
	{
		for (auto& Buffer : FacePathBuffers) Buffer.fill(0);
		for (auto& Buffer : FaceDestinationBuffers) Buffer.fill(0);
		for (std::string& Path : LastSuggestedFaceDestinations) Path.clear();
		PanoramaPathBuffer.fill(0);
		PanoramaDestinationBuffer.fill(0);
		LastSuggestedPanoramaDestination.clear();
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
		Destination.Reset(DestinationDirectory);
		ModalState.RequestOpen();
	}

	auto FTextureCubeImportDialog::Draw() -> void
	{
		ModalState.OpenPopupIfRequested("Import Texture Cube");

		const MonaImGui::FUIStyleMetrics Metrics = MonaImGui::GetUIStyleMetrics();
		ImGui::SetNextWindowSize(ImVec2(Metrics.WidePopupWidth, 0.0f), ImGuiCond_Appearing);
		if (!ImGui::BeginPopupModal("Import Texture Cube", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings))
			return;

		ImGui::TextUnformatted("Create a TextureCube asset from six faces or a 2:1 panorama.");
		ImGui::TextDisabled("Reference mounted images in place, or ingest external images into a writable mount.");
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
			? "Keeps images in allowed mounts; no copies are created."
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
		if (Destination.DrawRow("Asset path (one .dasset)", "##TextureCubeAssetPath",
			"/Game/Textures/Sky", "Choose...", BrowseButtonWidth))
			BrowseDestination();
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
					"/Project/Sources/Textures/Sky/Sky_panorama.hdr",
					PanoramaDestinationBuffer.data(),
					PanoramaDestinationBuffer.size());
			}
		}

		const FAssetDestinationValidation DestinationValidation =
			Destination.Inspect();
		const bool bEngineAuthoringContext = DestinationValidation.Mount
			&& DestinationValidation.Mount->Owner == PathUtilities::EMountOwner::Engine;
		std::array<FMountedSourceImportDiagnostic, TextureCubeFaceCount> FaceDiagnostics;
		FMountedSourceImportDiagnostic PanoramaDiagnostic;
		if (DestinationValidation.bAssetPathValid)
		{
			if (SourceLayout == ETextureCubeSourceLayout::SixFaces)
			{
				for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
					FaceDiagnostics[Index] = InspectMountedSourceImport(
						FacePathBuffers[Index].data(),
						DestinationValidation.AssetPath.GetView(),
						FaceDestinationBuffers[Index].data(), SourceMode,
						bEngineAuthoringContext);
			}
			else
			{
				PanoramaDiagnostic = InspectMountedSourceImport(
					PanoramaPathBuffer.data(),
					DestinationValidation.AssetPath.GetView(),
					PanoramaDestinationBuffer.data(), SourceMode,
					bEngineAuthoringContext);
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

		std::string DestinationValidationMessage;
		if (!DestinationValidation)
			DestinationValidationMessage = DestinationValidation.Message;
		else if (!bMountedSourcesValid)
			DestinationValidationMessage =
				SourceLayout == ETextureCubeSourceLayout::SixFaces
					? FirstInvalidFace->Message
					: PanoramaDiagnostic.Message;
		if (DestinationValidation.bMountedDestination && bMountedSourcesValid)
		{
			ImGui::BeginChild("TextureCubeOutputPreview",
				ImVec2(0.0f, MonaImGui::ScaleUI(
					SourceLayout == ETextureCubeSourceLayout::SixFaces ? 204.0f : 112.0f)),
				ImGuiChildFlags_Borders);
			ImGui::TextDisabled("Asset identity");
			ImGui::TextUnformatted(
				DestinationValidation.AssetPath.ToString().c_str());
			ImGui::TextDisabled("Package file");
			ImGui::TextUnformatted(std::format("{}.dasset",
				DestinationValidation.AssetPath.ToString()).c_str());
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
				Summary.Mount->bAuthoringWritable ? "writable" : "read-only");
			if (bEngineAuthoringContext)
				ImGui::TextDisabled("Engine authoring: this import writes shared Engine content.");
		}
		DrawImportDialogWarning(DestinationValidationMessage);

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::BeginDisabled(
			!bSourcesValid || !DestinationValidationMessage.empty());
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
				PathUtilities::FindMountForVirtualPath(Destination.GetPath());
			if (Lookup)
				Request.InitialDirectory = Lookup.Mount->GetContentDir().generic_string();
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
		if (Destination.GetPath().empty())
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
				PathUtilities::FindMountForVirtualPath(Destination.GetPath());
			if (Lookup)
				Request.InitialDirectory = Lookup.Mount->GetContentDir().generic_string();
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
		if (Destination.GetPath().empty()) SuggestAssetPath(Result.FilePath);
		SuggestSourceDestinations();
		RevalidateSources();
	}

	auto FTextureCubeImportDialog::BrowseDestination() -> void
	{
		FAssetPath CurrentAssetPath;
		const std::string DefaultFileName =
			FAssetPath::TryCreate(Destination.GetPath(), CurrentAssetPath)
			? std::string(CurrentAssetPath.GetAssetName()) + ".dasset" : "TextureCube.dasset";
		if (Destination.Browse("Choose a Texture Cube Asset Path",
			DefaultFileName,
			"The selected asset path is too long for the cube import form.",
			"Texture Cube assets must be saved inside a package-enabled mount.",
			Callbacks))
			SuggestSourceDestinations();
	}

	auto FTextureCubeImportDialog::RevalidateSources() -> bool
	{
		Asset::Forge::FTextureCubeImportValidation Validation;
		if (SourceLayout == ETextureCubeSourceLayout::SixFaces)
		{
			std::array<std::string, TextureCubeFaceCount> Faces;
			for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
				Faces[Index] = FacePathBuffers[Index].data();
			Validation = Asset::Forge::ValidateTextureCubeFaces(Faces);
		}
		else
		{
			const Asset::Forge::FTextureCubePanoramaImportSettings Settings{
				.FaceDimension = PanoramaFaceDimension,
				.ExposureEV = IsRadianceHDRPath(PanoramaPathBuffer.data()) ? PanoramaExposureEV : 0.0f};
			Validation = Asset::Forge::ValidateTextureCubePanorama(
				PanoramaPathBuffer.data(), Settings);
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
		Callbacks.Clear();
		if (!RevalidateSources()) return false;
		Asset::Forge::FTextureCubeImportResult Result;
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
			Result = Asset::Forge::ImportTextureCubeFaces(
				Faces, Destination.GetPath(), {}, Destinations,
				IsEngineAuthoringDestination(Destination.GetPath()));
		}
		else
		{
			const Asset::Forge::FTextureCubePanoramaImportSettings Settings{
				.FaceDimension = PanoramaFaceDimension,
				.ExposureEV = bValidatedHDR ? PanoramaExposureEV : 0.0f};
			Result = Asset::Forge::ImportTextureCubePanorama(
				PanoramaPathBuffer.data(), Destination.GetPath(), Settings,
				SourceMode == EMountedSourceImportMode::IngestExternal
					? PanoramaDestinationBuffer.data() : std::string_view{},
				IsEngineAuthoringDestination(Destination.GetPath()));
		}
		if (!Result) { SetError(Result.Message); return false; }
		Callbacks.NotifyImported(Destination.GetPath());
		FAssetPath ImportedPath;
		if (FAssetPath::TryCreate(Destination.GetPath(), ImportedPath))
			Asset::UnloadPackage(ImportedPath);
		return true;
	}

	auto FTextureCubeImportDialog::SuggestAssetPath(std::string_view SourceFile) -> void
	{
		const std::string AssetName = StringUtils::SanitizeFileName(
			std::filesystem::path(SourceFile).stem().generic_string(), "TextureCube");
		const FProjectInfo* Project = GetCurrentProject();
		Destination.SuggestPath(Destination.MakeSuggestedPath(AssetName,
			(Project ? Project->MountRoot : "/") + std::string("Textures/")));
		SuggestSourceDestinations();
	}

	auto FTextureCubeImportDialog::SuggestSourceDestinations() -> void
	{
		FAssetPath AssetPath;
		if (!FAssetPath::TryCreate(Destination.GetPath(), AssetPath)) return;
		const std::string AssetName(AssetPath.GetAssetName());
		if (PanoramaPathBuffer[0] != '\0')
		{
			const std::string Suggested = MakeDefaultImportedSourceVirtualPath(
				AssetPath.GetView(), "Textures",
				AssetName + "_panorama"
					+ std::filesystem::path(PanoramaPathBuffer.data()).extension().generic_string(),
				AssetName);
			ApplySuggestedPath(PanoramaDestinationBuffer,
				LastSuggestedPanoramaDestination, Suggested);
		}
		for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
		{
			if (FacePathBuffers[Index][0] == '\0') continue;
			const std::string Suggested = MakeDefaultImportedSourceVirtualPath(
				AssetPath.GetView(), "Textures",
				std::format("{}_{}{}", AssetName, FaceSuffixes[Index],
					std::filesystem::path(FacePathBuffers[Index].data())
						.extension().generic_string()), AssetName);
			ApplySuggestedPath(FaceDestinationBuffers[Index],
				LastSuggestedFaceDestinations[Index], Suggested);
		}
	}

	auto FTextureCubeImportDialog::SetError(std::string Message) const -> void
	{
		Callbacks.Report(std::move(Message));
	}
}
