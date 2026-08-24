#include "Assets/TextureImportDialog.h"

#include "AssetForge/Builtins/TextureCubeImport.h"

#include "Assets/AssetDestinationValidation.h"
#include "Assets/MountedSourceImport.h"
#include "AssetAuthoring.h"
#include "AssetForge/ImportService.h"
#include "Dialogs/FileDialog.h"
#include "Misc/Paths.h"
#include "Misc/Project.h"
#include "Misc/StringConvert.h"
#include "MonaImGui.h"
#include "PixelFormat.h"

namespace Durin::Editor::Level
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
		constexpr std::array<std::string_view, TextureCubeFaceCount> FaceSuffixes = {
			"px", "nx", "py", "ny", "pz", "nz"};

		auto FaceIndex(ETextureCubeFace Face) -> size_t
		{
			return static_cast<size_t>(Face);
		}

		auto IsRadianceHDRPath(std::string_view Path) -> bool
		{
			std::string Extension =
				std::filesystem::path(Path).extension().generic_string();
			std::ranges::transform(Extension, Extension.begin(),
				[](unsigned char Character) {
					return static_cast<char>(std::tolower(Character));
				});
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
	} // namespace

	auto FTextureImportDialog::DrawTextureCubeSource() -> void
	{
		FTextureCubeImportFormState& Cube = State.GetTextureCube();
		if (TextureCubePreview) (void)RevalidateTextureCubeSources();
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
			ImGui::TextDisabled("AssetForge verified the TextureCube source graph and output policy.");
		else
		{
			ImGui::PushStyleColor(ImGuiCol_Text,
				MonaImGui::GetThemeColor(
					MonaImGui::EUIThemeColor::Warning));
			ImGui::TextWrapped("%s", Cube.SourceValidationMessage.c_str());
			ImGui::PopStyleColor();
		}
	}

	auto FTextureImportDialog::DrawTextureCubeSourceDestinations() -> void
	{
		if (State.GetSourceMode() != EMountedSourceImportMode::IngestExternal)
			return;
		FTextureCubeImportFormState& Cube = State.GetTextureCube();
		ImGui::TextUnformatted(
			Cube.SourceLayout == ETextureCubeSourceLayout::SixFaces
				? "Source virtual paths" : "Source virtual path");
		if (Cube.SourceLayout == ETextureCubeSourceLayout::SixFaces)
		{
			for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
			{
				ImGui::PushID(static_cast<int>(Index));
				ImGui::SetNextItemWidth(-FLT_MIN);
				ImGui::InputText(FaceLabels[Index].data(),
					Cube.FaceDestinationBuffers[Index].data(),
					Cube.FaceDestinationBuffers[Index].size());
				ImGui::PopID();
			}
		}
		else
		{
			ImGui::SetNextItemWidth(-FLT_MIN);
			ImGui::InputTextWithHint("##TextureCubePanoramaDestination",
				"/Project/Sources/Textures/Sky/Sky_panorama.hdr",
				Cube.PanoramaDestinationBuffer.data(),
				Cube.PanoramaDestinationBuffer.size());
		}
	}

	auto FTextureImportDialog::ValidateAndDrawTextureCubeDestination()
		-> std::string
	{
		FTextureCubeImportFormState& Cube = State.GetTextureCube();
		const FAssetDestinationValidation DestinationValidation =
			Destination.Inspect();
		const bool bEngineAuthoringContext = DestinationValidation.Mount &&
			DestinationValidation.Mount->Owner ==
				PathUtilities::EMountOwner::Engine;
		std::array<FMountedSourceImportDiagnostic, TextureCubeFaceCount>
			FaceDiagnostics;
		FMountedSourceImportDiagnostic PanoramaDiagnostic;
		if (DestinationValidation.bAssetPathValid)
		{
			if (Cube.SourceLayout == ETextureCubeSourceLayout::SixFaces)
			{
				for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
					FaceDiagnostics[Index] = InspectMountedSourceImport(
						Cube.FacePathBuffers[Index].data(),
						DestinationValidation.AssetPath.GetView(),
						Cube.FaceDestinationBuffers[Index].data(),
						State.GetSourceMode(), bEngineAuthoringContext);
			}
			else
			{
				PanoramaDiagnostic = InspectMountedSourceImport(
					Cube.PanoramaPathBuffer.data(),
					DestinationValidation.AssetPath.GetView(),
					Cube.PanoramaDestinationBuffer.data(),
					State.GetSourceMode(), bEngineAuthoringContext);
			}
		}
		const auto FirstInvalidFace = std::ranges::find_if(
			FaceDiagnostics,
			[](const FMountedSourceImportDiagnostic& Diagnostic) {
				return !Diagnostic.bValid;
			});
		const bool bMountedSourcesValid =
			Cube.SourceLayout == ETextureCubeSourceLayout::SixFaces
				? FirstInvalidFace == FaceDiagnostics.end()
				: PanoramaDiagnostic.bValid;

		std::string ValidationMessage;
		if (!DestinationValidation)
			ValidationMessage = DestinationValidation.Message;
		else if (!bMountedSourcesValid)
			ValidationMessage =
				Cube.SourceLayout == ETextureCubeSourceLayout::SixFaces
					? FirstInvalidFace->Message
					: PanoramaDiagnostic.Message;

		if (DestinationValidation.bMountedDestination && bMountedSourcesValid)
		{
			ImGui::BeginChild("TextureCubeOutputPreview",
				ImVec2(0.0f, MonaImGui::ScaleUI(
					Cube.SourceLayout == ETextureCubeSourceLayout::SixFaces
						? 204.0f : 112.0f)),
				ImGuiChildFlags_Borders);
			ImGui::TextDisabled("Asset identity");
			ImGui::TextUnformatted(
				DestinationValidation.AssetPath.ToString().c_str());
			ImGui::TextDisabled("Package file");
			ImGui::TextUnformatted(std::format("{}.dasset",
				DestinationValidation.AssetPath.ToString()).c_str());
			ImGui::TextDisabled("Source virtual path%s",
				Cube.SourceLayout == ETextureCubeSourceLayout::SixFaces
					? "s" : "");
			if (Cube.SourceLayout == ETextureCubeSourceLayout::SixFaces)
			{
				for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
					ImGui::TextUnformatted(std::format("{}  {}",
						FaceLabels[Index],
						FaceDiagnostics[Index].VirtualPath).c_str());
			}
			else
				ImGui::TextUnformatted(PanoramaDiagnostic.VirtualPath.c_str());
			ImGui::EndChild();
			const FMountedSourceImportDiagnostic& Summary =
				Cube.SourceLayout == ETextureCubeSourceLayout::SixFaces
					? FaceDiagnostics.front() : PanoramaDiagnostic;
			ImGui::TextDisabled("Mount: %s (%s)  |  %s  |  dependency allowed",
				Summary.Mount->VirtualRoot.c_str(),
				DescribeMountOwner(Summary.Mount->Owner),
				Summary.Mount->bAuthoringWritable ? "writable" : "read-only");
			if (bEngineAuthoringContext)
				ImGui::TextDisabled(
					"Engine authoring: this import writes shared Engine content.");
		}
		if (ValidationMessage.empty() && !Cube.bSourcesValid)
			ValidationMessage = Cube.SourceValidationMessage;
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
		if (State.GetSourceMode() ==
			EMountedSourceImportMode::ReferenceExisting)
		{
			const PathUtilities::FMountLookupResult Lookup =
				PathUtilities::FindMountForVirtualPath(Destination.GetPath());
			if (Lookup)
				Request.InitialDirectory =
					Lookup.Mount->GetContentDir().generic_string();
		}
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
		SuggestTextureCubeSourceDestinations();
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
		if (State.GetSourceMode() ==
			EMountedSourceImportMode::ReferenceExisting)
		{
			const PathUtilities::FMountLookupResult Lookup =
				PathUtilities::FindMountForVirtualPath(Destination.GetPath());
			if (Lookup)
				Request.InitialDirectory =
					Lookup.Mount->GetContentDir().generic_string();
		}
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
		SuggestTextureCubeSourceDestinations();
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
		const std::string CurrentKey = std::format("{}:{}:{}:{}",
			static_cast<uint32>(Cube.SourceLayout), Cube.PanoramaFaceDimension,
			Cube.PanoramaExposureEV,
			std::accumulate(Files.begin(), Files.begin() + SourceCount, std::string{},
				[](std::string Result, const std::string& File) {
					Result.append("|").append(File); return Result;
				}));
		if (TextureCubePreview)
		{
			if (PendingTextureCubePreviewKey != CurrentKey)
			{
				TextureCubePreview.reset();
				PendingTextureCubePreviewKey.clear();
			}
			else
			{
			AssetForge::FImportResult Preview;
			if (!TextureCubePreview->TryGetResult(Preview)) return Cube.bSourcesValid;
			TextureCubePreview.reset();
			PendingTextureCubePreviewKey.clear();
			Cube.bSourcesValid = Preview.Outcome.State == AssetForge::EImportOperationState::Succeeded
				&& Preview.Inspection.bCompatible;
			ValidatedTextureCubePreviewKey = Cube.bSourcesValid ? CurrentKey : std::string{};
			Cube.SourceValidationMessage = Cube.bSourcesValid
				? "TextureCube source graph is compatible."
				: Preview.Outcome.Diagnostic.empty()
					? "TextureCube AssetForge preview failed."
					: Preview.Outcome.Diagnostic;
			return Cube.bSourcesValid;
			}
		}
		if (ValidatedTextureCubePreviewKey == CurrentKey) return Cube.bSourcesValid;
		std::array<FSourcePath, TextureCubeFaceCount> Sources;
		if (Cube.SourceLayout == ETextureCubeSourceLayout::SixFaces)
		{
			for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
			{
				Sources[Index].Path = Files[Index];
			}
		}
		else
			Sources[0].Path = Files[0];
		if (std::ranges::any_of(std::span(Files).first(SourceCount),
			[](const std::string& File) { return File.empty(); }))
		{
			Cube.bSourcesValid = false;
			Cube.SourceValidationMessage = "Select every required TextureCube source.";
			return false;
		}
		FAssetPath PreviewDestination;
		if (!FAssetPath::TryCreate(Destination.GetPath(), PreviewDestination))
			(void)FAssetPath::TryCreate("/Engine/Transient/TextureCubePreview", PreviewDestination);
		AssetForge::FImportRequest Request;
		std::string Error;
		if (!AssetForge::Builtins::MakeTextureCubeImportRequest(
			std::span(Sources).first(SourceCount), Cube.SourceLayout, PreviewDestination, {},
			{.FaceDimension = Cube.PanoramaFaceDimension,
				.ExposureEV = IsRadianceHDRPath(Cube.PanoramaPathBuffer.data())
					? Cube.PanoramaExposureEV : 0.0f},
			AssetForge::EImportMode::Preview,
			{.OwnerId = "TextureImportDialog.TextureCubePreview"}, {}, Request, Error))
		{
			Cube.bSourcesValid = false;
			Cube.SourceValidationMessage = std::move(Error);
			return false;
		}
		Request.Lifetime = AssetForge::EImportOperationLifetime::EphemeralPreview;
		TextureCubePreview = AssetForge::GetImportService().SubmitImport(
			std::move(Request), "Preview TextureCube");
		PendingTextureCubePreviewKey = TextureCubePreview && *TextureCubePreview
			? CurrentKey : std::string{};
		Cube.bSourcesValid = false;
		Cube.SourceValidationMessage = TextureCubePreview && *TextureCubePreview
			? "Preparing TextureCube preview..." : "TextureCube preview could not be submitted.";
		return false;
	}

	auto FTextureImportDialog::ImportTextureCube() -> bool
	{
		FTextureCubeImportFormState& Cube = State.GetTextureCube();
		if (!RevalidateTextureCubeSources()) return false;
		FAssetPath AssetPath;
		std::string Error;
		if (!FAssetPath::TryCreate(Destination.GetPath(), AssetPath, &Error))
		{
			SetError(std::move(Error));
			return false;
		}
		std::array<std::string, TextureCubeFaceCount> Sources;
		std::array<std::string, TextureCubeFaceCount> Destinations;
		size_t SourceCount = 1;
		AssetForge::Builtins::FTextureCubePanoramaImportSettings PanoramaSettings;
		if (Cube.SourceLayout == ETextureCubeSourceLayout::SixFaces)
		{
			SourceCount = TextureCubeFaceCount;
			for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
			{
				Sources[Index] = Cube.FacePathBuffers[Index].data();
				if (State.GetSourceMode() ==
					EMountedSourceImportMode::IngestExternal)
					Destinations[Index] =
						Cube.FaceDestinationBuffers[Index].data();
			}
		}
		else
		{
			Sources[0] = Cube.PanoramaPathBuffer.data();
			PanoramaSettings = {
				.FaceDimension = Cube.PanoramaFaceDimension,
				.ExposureEV = IsRadianceHDRPath(Cube.PanoramaPathBuffer.data())
					? Cube.PanoramaExposureEV : 0.0f};
			if (State.GetSourceMode() ==
					EMountedSourceImportMode::IngestExternal)
				Destinations[0] = Cube.PanoramaDestinationBuffer.data();
		}
		const std::string Path = AssetPath.ToString();
		const FImportDialogCallbacks CompletionCallbacks = Callbacks;
		AssetForge::FImportHandle Handle = AssetForge::Builtins::SubmitTextureCubeImport(
			std::span(Sources).first(SourceCount), std::span(Destinations).first(SourceCount),
			Cube.SourceLayout, AssetPath, {}, PanoramaSettings,
			IsEngineAuthoringDestination(Destination.GetPath()),
			[CompletionCallbacks, Path](const AssetForge::FImportResult& Result) {
				if (Result.Outcome.State == AssetForge::EImportOperationState::Succeeded)
				{
					CompletionCallbacks.NotifyImported(Path);
					FAssetPath ImportedPath;
					if (FAssetPath::TryCreate(Path, ImportedPath)) Asset::UnloadPackage(ImportedPath);
				}
				else CompletionCallbacks.Report(Result.Outcome.Diagnostic.empty()
					? "TextureCube AssetForge import failed." : Result.Outcome.Diagnostic);
			}, Error);
		if (!Handle)
		{
			SetError(std::move(Error));
			return false;
		}
		Callbacks.NotifyImportStarted(Handle.GetOperationHandle(),
			std::format("Import TextureCube {}", AssetPath.GetAssetName()));
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
		SuggestTextureCubeSourceDestinations();
	}

	auto FTextureImportDialog::SuggestTextureCubeSourceDestinations() -> void
	{
		FTextureCubeImportFormState& Cube = State.GetTextureCube();
		FAssetPath AssetPath;
		if (!FAssetPath::TryCreate(Destination.GetPath(), AssetPath)) return;
		const std::string AssetName(AssetPath.GetAssetName());
		if (Cube.PanoramaPathBuffer[0] != '\0')
		{
			const std::string Suggested = MakeDefaultImportedSourceVirtualPath(
				AssetPath.GetView(), "Textures",
				AssetName + "_panorama" + std::filesystem::path(
					Cube.PanoramaPathBuffer.data()).extension().generic_string(),
				AssetName);
			ApplySuggestedPath(Cube.PanoramaDestinationBuffer,
				Cube.LastSuggestedPanoramaDestination, Suggested);
		}
		for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
		{
			if (Cube.FacePathBuffers[Index][0] == '\0') continue;
			const std::string Suggested = MakeDefaultImportedSourceVirtualPath(
				AssetPath.GetView(), "Textures",
				std::format("{}_{}{}", AssetName, FaceSuffixes[Index],
					std::filesystem::path(Cube.FacePathBuffers[Index].data())
						.extension().generic_string()),
				AssetName);
			ApplySuggestedPath(Cube.FaceDestinationBuffers[Index],
				Cube.LastSuggestedFaceDestinations[Index], Suggested);
		}
	}
} // namespace Durin::Editor::Level
