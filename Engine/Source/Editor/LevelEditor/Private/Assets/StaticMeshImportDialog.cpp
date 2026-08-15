#include "Assets/StaticMeshImportDialog.h"

#include "Assets/AssetDestinationValidation.h"
#include "AssetLoad.h"
#include "Dialogs/FileDialog.h"
#include "Misc/Paths.h"
#include "Misc/Project.h"
#include "Misc/StringConvert.h"
#include "MonaImGui.h"
#include "StaticMeshSourceTranslation.h"

namespace Durin::Editor::Level
{
	namespace
	{
		constexpr const char* ImportPresetNames[] = {
			"Durin (+X Forward, +Y Right, +Z Up)",
			"Y-Up / -Z Forward (+X Right)",
			"Custom"
		};
		constexpr const char* ImportAxisNames[] = {"+X", "-X", "+Y", "-Y", "+Z", "-Z"};

		auto Lowercase(std::string Value) -> std::string
		{
			std::ranges::transform(Value, Value.begin(), [](unsigned char Character) {
				return static_cast<char>(std::tolower(Character));
			});
			return Value;
		}

		auto IsSupportedModelExtension(std::string_view Extension) -> bool
		{
			const std::string Folded = Lowercase(std::string(Extension));
			return Folded == ".obj" || Folded == ".fbx" || Folded == ".gltf"
				|| Folded == ".glb" || Folded == ".dae" || Folded == ".3ds"
				|| Folded == ".ply" || Folded == ".stl";
		}

		auto FindOwningMount(std::string_view VirtualPath)
			-> const PathUtilities::FMountPoint*
		{
			const PathUtilities::FMountLookupResult Lookup =
				PathUtilities::FindMountForVirtualPath(VirtualPath);
			return Lookup ? Lookup.Mount : nullptr;
		}

		auto DrawImportAxisCombo(const char* Label, EStaticMeshImportAxis& Axis) -> bool
		{
			int Value = static_cast<int>(Axis);
			if (!ImGui::Combo(Label, &Value, ImportAxisNames, std::size(ImportAxisNames)))
				return false;
			Axis = static_cast<EStaticMeshImportAxis>(Value);
			return true;
		}
	} // namespace

	FStaticMeshImportDialog::FStaticMeshImportDialog(
		FImportDialogCallbacks InCallbacks)
		: Callbacks(std::move(InCallbacks))
	{
	}

	auto FStaticMeshImportDialog::Open(std::string_view DestinationDirectory) -> void
	{
		SourcePathBuffer.fill(0);
		SourceDestinationBuffer.fill(0);
		LastSuggestedSourceDestination.clear();
		ImportSettings = FStaticMeshImportSettings::MakeDurin();
		ImportPreset = EImportPreset::Durin;
		SourceMode = EMountedSourceImportMode::IngestExternal;
		Destination.Reset(DestinationDirectory);
		ModalState.RequestOpen();
	}

	auto FStaticMeshImportDialog::Draw() -> void
	{
		ModalState.OpenPopupIfRequested("Import Static Mesh");

		const MonaImGui::FUIStyleMetrics Metrics = MonaImGui::GetUIStyleMetrics();
		ImGui::SetNextWindowSize(ImVec2(Metrics.WidePopupWidth, 0.0f), ImGuiCond_Appearing);
		if (!ImGui::BeginPopupModal("Import Static Mesh", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize
				| ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings))
			return;

		ImGui::TextUnformatted("Create one geometry-only StaticMesh from a model file.");
		ImGui::TextDisabled("Materials and textures are not created; use Scene Source for a complete FBX or glTF scene.");
		ImGui::Spacing();
		ImGui::SeparatorText("Source model");
		if (ImGui::RadioButton("Reference Existing Source",
			SourceMode == EMountedSourceImportMode::ReferenceExisting))
			SourceMode = EMountedSourceImportMode::ReferenceExisting;
		ImGui::SameLine();
		if (ImGui::RadioButton("Ingest External Source",
			SourceMode == EMountedSourceImportMode::IngestExternal))
			SourceMode = EMountedSourceImportMode::IngestExternal;
		ImGui::TextDisabled(SourceMode == EMountedSourceImportMode::ReferenceExisting
			? "Keeps a source already inside an allowed mount; no copy is created."
			: "Copies an external model transactionally to the explicit mounted source path.");
		const float BrowseButtonWidth = Metrics.StandardButtonWidth;
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - BrowseButtonWidth
			- ImGui::GetStyle().ItemSpacing.x);
		ImGui::InputTextWithHint("##StaticMeshImportSource",
			"Choose an OBJ or another supported model...", SourcePathBuffer.data(),
			SourcePathBuffer.size(), ImGuiInputTextFlags_ReadOnly);
		ImGui::SameLine();
		if (ImGui::Button("Browse...", ImVec2(BrowseButtonWidth, 0.0f))) BrowseSource();

		const std::filesystem::path SourcePath(SourcePathBuffer.data());
		const bool bHasSource = SourcePathBuffer[0] != '\0';
		const bool bSourceExists = bHasSource && std::filesystem::is_regular_file(SourcePath);
		const bool bSupportedSource = bHasSource
			&& IsSupportedModelExtension(SourcePath.extension().generic_string());
		if (bHasSource) ImGui::TextDisabled("%s", SourcePath.filename().generic_string().c_str());

		ImGui::Spacing();
		ImGui::SeparatorText("Coordinate system");
		int PresetIndex = static_cast<int>(ImportPreset);
		if (ImGui::Combo("Preset", &PresetIndex, ImportPresetNames, std::size(ImportPresetNames)))
		{
			ImportPreset = static_cast<EImportPreset>(PresetIndex);
			if (ImportPreset == EImportPreset::Durin)
				ImportSettings = FStaticMeshImportSettings::MakeDurin();
			else if (ImportPreset == EImportPreset::YUpNegativeZForward)
				ImportSettings = FStaticMeshImportSettings::MakeYUpNegativeZForward();
		}
		if (ImportPreset == EImportPreset::Custom)
		{
			DrawImportAxisCombo("Forward", ImportSettings.ForwardAxis);
			DrawImportAxisCombo("Right", ImportSettings.RightAxis);
			DrawImportAxisCombo("Up", ImportSettings.UpAxis);
		}
		else
		{
			ImGui::TextDisabled("Source axes are baked into Durin's +X Forward / +Y Right / +Z Up basis.");
		}

		ImGui::Spacing();
		ImGui::SeparatorText("Destination");
		if (Destination.DrawRow("Asset path (one .dasset)", "##StaticMeshImportAssetPath",
			"/Project/StaticMeshes/AssetName", "Choose...", BrowseButtonWidth))
			BrowseDestination();
		if (SourceMode == EMountedSourceImportMode::IngestExternal)
		{
			ImGui::TextUnformatted("Source virtual path");
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - BrowseButtonWidth
				- ImGui::GetStyle().ItemSpacing.x);
			ImGui::InputTextWithHint("##StaticMeshSourceDestination",
				"/Project/Sources/Models/AssetName.obj", SourceDestinationBuffer.data(),
				SourceDestinationBuffer.size());
			ImGui::SameLine();
			if (ImGui::Button("Choose source...", ImVec2(BrowseButtonWidth, 0.0f)))
				BrowseSourceDestination();
		}

		const FAssetDestinationValidation DestinationValidation = Destination.Inspect();
		const bool bEngineAuthoringContext = DestinationValidation.Mount
			&& DestinationValidation.Mount->Owner == PathUtilities::EMountOwner::Engine;
		std::string ImportSettingsError;
		const bool bImportSettingsValid = ImportSettings.IsValid(&ImportSettingsError);
		const FMountedSourceImportDiagnostic SourceDiagnostic =
			DestinationValidation.bAssetPathValid
			? InspectMountedSourceImport(SourcePathBuffer.data(),
				DestinationValidation.AssetPath.GetView(), SourceDestinationBuffer.data(),
				SourceMode, bEngineAuthoringContext)
			: FMountedSourceImportDiagnostic{};
		const std::filesystem::path SourceDestination(
			SourceDiagnostic.VirtualPath.empty()
				? SourceDestinationBuffer.data() : SourceDiagnostic.VirtualPath);
		const bool bSourceExtensionMatches = bHasSource
			&& SourceMode == EMountedSourceImportMode::IngestExternal
			&& Lowercase(SourceDestination.extension().generic_string())
				== Lowercase(SourcePath.extension().generic_string());

		if (DestinationValidation.bAssetPathValid
			&& DestinationValidation.bMountedDestination && bHasSource
			&& SourceDiagnostic.bValid)
		{
			ImGui::BeginChild("StaticMeshImportOutputPreview",
				ImVec2(0.0f, MonaImGui::ScaleUI(112.0f)), ImGuiChildFlags_Borders);
			ImGui::TextDisabled("Asset identity");
			ImGui::TextUnformatted(DestinationValidation.AssetPath.ToString().c_str());
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

		std::string ValidationMessage;
		if (!bHasSource) ValidationMessage = "Select a source model to continue.";
		else if (!bSourceExists) ValidationMessage = "The selected source file no longer exists.";
		else if (!bSupportedSource)
			ValidationMessage = "Supported model formats are OBJ, FBX, glTF, COLLADA, 3DS, PLY, and STL.";
		else if (!bImportSettingsValid) ValidationMessage = ImportSettingsError;
		else if (!DestinationValidation) ValidationMessage = DestinationValidation.Message;
		else if (!SourceDiagnostic.bValid) ValidationMessage = SourceDiagnostic.Message;
		else if (SourceMode == EMountedSourceImportMode::IngestExternal
			&& !bSourceExtensionMatches)
			ValidationMessage = "The source copy must keep the selected model's file extension.";

		DrawImportDialogWarning(ValidationMessage);
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::BeginDisabled(!ValidationMessage.empty());
		if (ImGui::Button("Import Static Mesh",
			ImVec2(MonaImGui::ScaleUI(150.0f), 0.0f)) && Import())
			ImGui::CloseCurrentPopup();
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (MonaImGui::DialogButton("Cancel", true)) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	auto FStaticMeshImportDialog::BrowseSource() -> void
	{
		FFileDialogRequest Request;
		Request.ParentWindowHandle = ImGui::GetMainViewport()->PlatformHandleRaw;
		Request.Title = "Select a Static Mesh Source File";
		Request.Filters = {
			{"All Supported Models", "*.obj;*.fbx;*.gltf;*.glb;*.dae;*.3ds;*.ply;*.stl"},
			{"Wavefront OBJ", "*.obj"}, {"Autodesk FBX", "*.fbx"},
			{"glTF", "*.gltf;*.glb"}, {"COLLADA", "*.dae"},
			{"PLY", "*.ply"}, {"STL", "*.stl"}, {"All Files", "*.*"}
		};
		if (const FProjectInfo* Project = GetCurrentProject())
			Request.InitialDirectory = Project->ProjectDir;
		if (SourceMode == EMountedSourceImportMode::ReferenceExisting)
		{
			const PathUtilities::FMountLookupResult Lookup =
				PathUtilities::FindMountForVirtualPath(Destination.GetPath());
			if (Lookup) Request.InitialDirectory = Lookup.Mount->GetContentDir().generic_string();
		}
		if (SourcePathBuffer[0] != '\0')
			Request.InitialDirectory = std::filesystem::path(SourcePathBuffer.data())
				.parent_path().generic_string();
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
		if (Lowercase(std::filesystem::path(Result.FilePath).extension().generic_string()) == ".obj")
		{
			ImportPreset = EImportPreset::YUpNegativeZForward;
			ImportSettings = FStaticMeshImportSettings::MakeYUpNegativeZForward();
		}
		else
		{
			ImportPreset = EImportPreset::Durin;
			ImportSettings = FStaticMeshImportSettings::MakeDurin();
		}
		const std::string AssetName = StringUtils::SanitizeFileName(
			std::filesystem::path(Result.FilePath).stem().generic_string(), "StaticMesh");
		const FProjectInfo* Project = GetCurrentProject();
		Destination.SuggestPath(Destination.MakeSuggestedPath(AssetName,
			(Project ? Project->MountRoot : "/") + std::string("StaticMeshes/")));
		SuggestSourceDestination();
	}

	auto FStaticMeshImportDialog::SuggestSourceDestination() -> void
	{
		if (SourcePathBuffer[0] == '\0') return;
		const std::filesystem::path SourcePath(SourcePathBuffer.data());
		const std::string AssetName = StringUtils::SanitizeFileName(
			SourcePath.stem().generic_string(), "StaticMesh");
		const std::string PreviousSourceDestination = SourceDestinationBuffer.data();
		const std::string SuggestedSourceDestination = MakeDefaultImportedSourceVirtualPath(
			Destination.GetPath(), "Models",
			AssetName + SourcePath.extension().generic_string());
		if (PreviousSourceDestination.empty()
			|| PreviousSourceDestination == LastSuggestedSourceDestination)
		{
			SourceDestinationBuffer.fill(0);
			std::memcpy(SourceDestinationBuffer.data(), SuggestedSourceDestination.data(),
				std::min(SuggestedSourceDestination.size(), SourceDestinationBuffer.size() - 1));
		}
		LastSuggestedSourceDestination = SuggestedSourceDestination;
	}

	auto FStaticMeshImportDialog::BrowseDestination() -> void
	{
		const std::string DefaultFileName = SourcePathBuffer[0] != '\0'
			? StringUtils::SanitizeFileName(
				std::filesystem::path(SourcePathBuffer.data()).stem().generic_string(),
				"StaticMesh") + ".dasset"
			: "StaticMesh.dasset";
		if (Destination.Browse("Choose a Static Mesh Asset Path", DefaultFileName,
			"The selected asset path is too long for the import form.",
			"Static mesh assets must be saved inside a package-enabled mount.", Callbacks))
			SuggestSourceDestination();
	}

	auto FStaticMeshImportDialog::BrowseSourceDestination() -> void
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

		FFileDialogRequest Request;
		Request.ParentWindowHandle = ImGui::GetMainViewport()->PlatformHandleRaw;
		Request.Title = "Choose Static Mesh Source Copy Destination";
		Request.Filters = {
			{"All Supported Models", "*.obj;*.fbx;*.gltf;*.glb;*.dae;*.3ds;*.ply;*.stl"},
			{"Wavefront OBJ", "*.obj"}, {"All Files", "*.*"}
		};
		Request.InitialDirectory = Mount->GetContentDir().generic_string();
		Request.DefaultFileName = SourceDestinationBuffer[0] != '\0'
			? std::filesystem::path(SourceDestinationBuffer.data()).filename().generic_string()
			: SourcePathBuffer[0] != '\0'
				? std::filesystem::path(SourcePathBuffer.data()).filename().generic_string()
				: "StaticMesh.obj";
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
			SetError("Static mesh source copies must stay inside the selected mount.");
			return;
		}
		if (Classified.NormalizedVirtualPath.size() >= SourceDestinationBuffer.size())
		{
			SetError("The selected source destination is too long for the import form.");
			return;
		}
		SourceDestinationBuffer.fill(0);
		std::memcpy(SourceDestinationBuffer.data(),
			Classified.NormalizedVirtualPath.data(), Classified.NormalizedVirtualPath.size());
		LastSuggestedSourceDestination.clear();
	}

	auto FStaticMeshImportDialog::Import() -> bool
	{
		Callbacks.Clear();
		const FStaticMeshImportResult Result = Asset::Import::Standard::ImportStaticMeshAsset(
			SourcePathBuffer.data(), Destination.GetPath(), ImportSettings,
			SourceMode == EMountedSourceImportMode::IngestExternal
				? std::string_view(SourceDestinationBuffer.data()) : std::string_view{},
			IsEngineAuthoringDestination(Destination.GetPath()));
		if (!Result)
		{
			SetError(Result.Message);
			return false;
		}
		Callbacks.NotifyImported(Destination.GetPath());
		FAssetPath ImportedPath;
		if (FAssetPath::TryCreate(Destination.GetPath(), ImportedPath))
			Asset::UnloadPackage(ImportedPath);
		return true;
	}

	auto FStaticMeshImportDialog::SetError(std::string Message) const -> void
	{
		Callbacks.Report(std::move(Message));
	}
} // namespace Durin::Editor::Level
