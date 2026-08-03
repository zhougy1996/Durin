#include "Assets/SceneImportDialog.h"

#include "Assets/AssetDestinationValidation.h"
#include "Assets/MountedSourceImport.h"
#include "AssetSystem.h"
#include "Dialogs/FileDialog.h"
#include "Misc/Paths.h"
#include "Misc/Project.h"
#include "Misc/StringConvert.h"
#include "MonaImGui.h"
#include "Source/SourcePath.h"
#include "StaticMesh/StaticMesh.h"

namespace Durin
{
	namespace
	{
		constexpr const char* ImportPresetNames[] = {
			"Durin (+X Forward, +Y Right, +Z Up)",
			"Y-Up / -Z Forward (+X Right)",
			"Custom"
		};
		constexpr const char* ImportAxisNames[] = {"+X", "-X", "+Y", "-Y", "+Z", "-Z"};

		auto DrawImportAxisCombo(const char* Label, EStaticMeshImportAxis& Axis) -> bool
		{
			int Value = static_cast<int>(Axis);
			if (!ImGui::Combo(Label, &Value, ImportAxisNames, std::size(ImportAxisNames))) return false;
			Axis = static_cast<EStaticMeshImportAxis>(Value);
			return true;
		}
	}

	FSceneImportDialog::FSceneImportDialog(
		FImportDialogCallbacks InCallbacks)
		: Callbacks(std::move(InCallbacks))
	{
	}

	FSceneImportDialog::~FSceneImportDialog()
	{
		CancelRequests();
	}

	auto FSceneImportDialog::Open(std::string_view InDestinationDirectory) -> void
	{
		CancelRequests();
		SourcePathBuffer.fill(0);
		SourceDestinationBuffer.fill(0);
		LastSuggestedSourceDestination.clear();
		ImportSettings = FStaticMeshImportSettings::MakeDurin();
		ImportPreset = ESceneMeshImportPreset::Durin;
		SourceMode = EMountedSourceImportMode::IngestExternal;
		PreviewKey.clear();
		Preview.reset();
		DestinationDirectory.Reset(InDestinationDirectory);
		std::string Error;
		if (!EnsureStandardImportedSurfaceMaterial(Error)) SetError(std::move(Error));
		ModalState.RequestOpen();
	}

	auto FSceneImportDialog::Draw() -> void
	{
		ModalState.OpenPopupIfRequested("Import Scene Source");

		const MonaImGui::FUIStyleMetrics Metrics = MonaImGui::GetUIStyleMetrics();
		ImGui::SetNextWindowSize(ImVec2(Metrics.WidePopupWidth, 0.0f), ImGuiCond_Appearing);
		if (!ImGui::BeginPopupModal(
			"Import Scene Source",
			nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings
		))
			return;
		if (PollImport())
		{
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return;
		}
		const bool bImportPending = ImportRequest.has_value();
		ImGui::BeginDisabled(bImportPending);

		ImGui::TextUnformatted("Import the assets described by an FBX or glTF Scene source.");
		ImGui::TextDisabled("Outputs are peer assets grouped by type inside one destination directory.");
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
			? "Keeps a source already inside an allowed mounted SourceAssets domain; no copy is created."
			: "Copies an external model transactionally to the explicit mounted source path.");
		const float BrowseButtonWidth = Metrics.StandardButtonWidth;
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - BrowseButtonWidth - ImGui::GetStyle().ItemSpacing.x);
		ImGui::InputTextWithHint("##ImportSource", "Choose an FBX, glTF, or GLB Scene source...", SourcePathBuffer.data(), SourcePathBuffer.size(), ImGuiInputTextFlags_ReadOnly);
		ImGui::SameLine();
		if (ImGui::Button("Browse...", ImVec2(BrowseButtonWidth, 0.0f))) BrowseSource();

		const std::filesystem::path SourcePath(SourcePathBuffer.data());
		const bool bHasSource = SourcePathBuffer[0] != '\0';
		const bool bSourceExists = bHasSource && std::filesystem::is_regular_file(SourcePath);
		if (bHasSource)
			ImGui::TextDisabled("%s", std::format("{}  |  {}", SourcePath.extension().generic_string(), SourcePath.filename().generic_string()).c_str());

		ImGui::Spacing();
		ImGui::SeparatorText("Coordinate system");
		int PresetIndex = static_cast<int>(ImportPreset);
		if (ImGui::Combo("Preset", &PresetIndex, ImportPresetNames, std::size(ImportPresetNames)))
		{
			ImportPreset = static_cast<ESceneMeshImportPreset>(PresetIndex);
			if (ImportPreset == ESceneMeshImportPreset::Durin)
				ImportSettings = FStaticMeshImportSettings::MakeDurin();
			else if (ImportPreset == ESceneMeshImportPreset::YUpNegativeZForward)
				ImportSettings = FStaticMeshImportSettings::MakeYUpNegativeZForward();
		}
		if (ImportPreset == ESceneMeshImportPreset::Custom)
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
		if (DestinationDirectory.DrawRow("Output directory", "##SceneImportDirectory",
			"/Project/Scenes/SceneName", "Choose...", BrowseButtonWidth))
			BrowseDestinationDirectory();
		if (SourceMode == EMountedSourceImportMode::IngestExternal)
		{
			ImGui::TextUnformatted("Source virtual path");
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - BrowseButtonWidth - ImGui::GetStyle().ItemSpacing.x);
			ImGui::InputTextWithHint("##SceneSourceDestination",
				"/Project/Models/SceneName.fbx", SourceDestinationBuffer.data(),
				SourceDestinationBuffer.size());
			ImGui::SameLine();
			if (ImGui::Button("Choose source...", ImVec2(BrowseButtonWidth, 0.0f)))
				BrowseSourceDestination();
		}

		const FContentDirectoryValidation DestinationValidation =
			DestinationDirectory.Inspect();
		std::string ImportSettingsError;
		const bool bImportSettingsValid = ImportSettings.IsValid(&ImportSettingsError);
		const FMountedSourceImportDiagnostic SourceDiagnostic =
			DestinationValidation.bDirectoryPathValid
			? InspectMountedSourceImport(
				SourcePathBuffer.data(), DestinationValidation.DirectoryPath.GetView(),
				SourceDestinationBuffer.data(), SourceMode)
			: FMountedSourceImportDiagnostic{};
		if (DestinationValidation.bDirectoryPathValid && bSourceExists
			&& bImportSettingsValid && DestinationValidation
			&& SourceDiagnostic.bValid)
		{
			RefreshPreview(DestinationValidation.DirectoryPath);
		}
		else
		{
			if (PreviewRequest)
			{
				CancelAndDrainSceneImportPlan(*PreviewRequest);
				PreviewRequest.reset();
			}
			PreviewKey.clear();
			Preview.reset();
		}

		if (DestinationValidation.bDirectoryPathValid
			&& DestinationValidation.bMountedDestination && bHasSource
			&& SourceDiagnostic.bValid && Preview && *Preview)
		{
			ImGui::BeginChild("ImportOutputPreview",
				ImVec2(0.0f, MonaImGui::ScaleUI(190.0f)), ImGuiChildFlags_Borders);
			const AssetImport::FImportPreview& ImportPreview =
				Preview->Plan.GetMultiOutputPlan().GetPreview();
			ImGui::TextDisabled("Outputs and management actions");
			for (const AssetImport::FImportPreviewOutput& Asset : ImportPreview.Outputs)
			{
				const char* Action = "Create";
				switch (Asset.Action)
				{
				case AssetImport::EImportPreviewAction::Create: Action = "Create"; break;
				case AssetImport::EImportPreviewAction::Replace: Action = "Replace managed"; break;
				case AssetImport::EImportPreviewAction::Reference: Action = "Reference"; break;
				case AssetImport::EImportPreviewAction::KeepDetached: Action = "Keep detached"; break;
				case AssetImport::EImportPreviewAction::Missing: Action = "Missing"; break;
				case AssetImport::EImportPreviewAction::Collision: Action = "Collision"; break;
				case AssetImport::EImportPreviewAction::Orphan: Action = "Orphan"; break;
				}
				ImGui::BulletText("[%s] %s", Action, Asset.Output.AssetPath.ToString().c_str());
			}
			ImGui::Spacing();
			ImGui::TextDisabled("Captured sources");
			for (const AssetImport::FImportSourcePreview& Source : ImportPreview.Sources)
				ImGui::BulletText("%s", Source.SourcePath.Path.c_str());
			ImGui::TextDisabled("Estimate: CPU %.2f MiB  GPU %.2f MiB  Disk %.2f MiB",
				static_cast<double>(ImportPreview.EstimatedCpuBytes) / (1024.0 * 1024.0),
				static_cast<double>(ImportPreview.EstimatedGpuBytes) / (1024.0 * 1024.0),
				static_cast<double>(ImportPreview.EstimatedDiskBytes) / (1024.0 * 1024.0));
			for (const AssetImport::FImportWarningPreview& Warning : ImportPreview.Warnings)
			{
				const char* Change = Warning.Change == AssetImport::EImportWarningChange::New
					? "New warning" : Warning.Change == AssetImport::EImportWarningChange::Resolved
						? "Resolved" : "Previously accepted";
				ImGui::BulletText("%s: %s", Change, Warning.Diagnostic.Message.c_str());
			}
			ImGui::EndChild();
			ImGui::TextDisabled("Mount: %s (%s)  |  %s  |  dependency allowed",
				SourceDiagnostic.Mount->VirtualRoot.c_str(),
				DescribeMountOwner(SourceDiagnostic.Mount->Owner),
				SourceDiagnostic.Mount->bAuthoringWritable ? "writable" : "read-only");
		}

		std::string ValidationMessage;
		if (!bHasSource)
			ValidationMessage = "Select a source model to continue.";
		else if (!bSourceExists)
			ValidationMessage = "The selected source file no longer exists.";
		else if (!bImportSettingsValid)
			ValidationMessage = ImportSettingsError;
		else if (!DestinationValidation)
			ValidationMessage = DestinationValidation.Message;
		else if (!SourceDiagnostic.bValid)
			ValidationMessage = SourceDiagnostic.Message;
		else if (PreviewRequest)
			ValidationMessage = "Preparing import preview...";
		else if (Preview && !*Preview)
			ValidationMessage = Preview->Message;
		else if (ImportRequest)
			ValidationMessage = "Import preparation is running...";

		DrawImportDialogWarning(ValidationMessage);

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::BeginDisabled(!ValidationMessage.empty());
		if (ImGui::Button("Import Scene", ImVec2(MonaImGui::ScaleUI(150.0f), 0.0f)) && Import()) ImGui::CloseCurrentPopup();
		ImGui::EndDisabled();
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (MonaImGui::DialogButton("Cancel", true))
		{
			CancelRequests();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	auto FSceneImportDialog::BrowseSource() -> void
	{
		FFileDialogRequest Request;
		Request.ParentWindowHandle = ImGui::GetMainViewport()->PlatformHandleRaw;
		Request.Title = "Select an FBX or glTF Scene Source";
		Request.Filters = {
			{"Supported Scene Sources", "*.fbx;*.gltf;*.glb"},
			{"Autodesk FBX", "*.fbx"},
			{"glTF", "*.gltf;*.glb"},
			{"All Files", "*.*"}
		};
		if (const FProjectInfo* Project = GetCurrentProject()) Request.InitialDirectory = Project->ProjectDir;
		if (SourceMode == EMountedSourceImportMode::ReferenceExisting)
		{
			const PathUtilities::FMountLookupResult Lookup =
				PathUtilities::FindMountForVirtualPath(DestinationDirectory.GetPath());
			if (Lookup)
				Request.InitialDirectory = Lookup.Mount->Root.generic_string();
		}
		if (SourcePathBuffer[0] != '\0') Request.InitialDirectory = std::filesystem::path(SourcePathBuffer.data()).parent_path().generic_string();

		FFileDialogResult Result = OpenFileDialog(Request);
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
		std::memcpy(SourcePathBuffer.data(), Result.FilePath.data(), std::min(Result.FilePath.size(), SourcePathBuffer.size() - 1));
		ImportPreset = ESceneMeshImportPreset::Durin;
		ImportSettings = FStaticMeshImportSettings::MakeDurin();
		const std::string SceneName = StringUtils::SanitizeFileName(
			std::filesystem::path(Result.FilePath).stem().generic_string(), "Scene");
		const FProjectInfo* Project = GetCurrentProject();
		DestinationDirectory.SuggestPath(DestinationDirectory.MakeSuggestedPath(SceneName,
			(Project ? Project->MountRoot : "/")
				+ std::string("Scenes/")));
		const std::string PreviousSourceDestination = SourceDestinationBuffer.data();
		const std::string SuggestedSourceDestination = MakeDefaultSourceVirtualPath(
			DestinationDirectory.GetPath(), "Models",
			SceneName + std::filesystem::path(Result.FilePath).extension().generic_string());
		if (PreviousSourceDestination.empty()
			|| PreviousSourceDestination == LastSuggestedSourceDestination)
		{
			SourceDestinationBuffer.fill(0);
			std::memcpy(SourceDestinationBuffer.data(), SuggestedSourceDestination.data(),
				std::min(SuggestedSourceDestination.size(),
					SourceDestinationBuffer.size() - 1));
		}
		LastSuggestedSourceDestination = SuggestedSourceDestination;
	}

	auto FSceneImportDialog::BrowseDestinationDirectory() -> void
	{
		DestinationDirectory.Browse("Choose a Scene Output Directory",
			"The selected directory path is too long for the import form.",
			"Scene outputs must be saved inside a mounted Content directory.",
			Callbacks);
	}

	auto FSceneImportDialog::BrowseSourceDestination() -> void
	{
		FAssetPath AssetPath;
		std::string Error;
		if (!FAssetPath::TryCreate(DestinationDirectory.GetPath(), AssetPath, &Error))
		{
			SetError("Choose a valid output directory before selecting the source destination.");
			return;
		}
		const PathUtilities::FMountLookupResult Lookup =
			PathUtilities::FindMountForVirtualPath(AssetPath.GetView());
		if (!Lookup)
		{
			SetError("The selected asset path does not use a registered mount.");
			return;
		}
		FFileDialogRequest Request;
		Request.ParentWindowHandle = ImGui::GetMainViewport()->PlatformHandleRaw;
		Request.Title = "Choose Scene Source Destination";
		Request.Filters = {{"All Files", "*.*"}};
		Request.InitialDirectory = Lookup.Mount->Root.generic_string();
		Request.DefaultFileName = SourcePathBuffer[0] != '\0'
			? std::filesystem::path(SourcePathBuffer.data()).filename().generic_string()
			: "Scene.fbx";
		const FFileDialogResult Result = SaveFileDialog(Request);
		if (Result.Status == EFileDialogStatus::Cancelled) return;
		if (Result.Status == EFileDialogStatus::Error) { SetError(Result.ErrorMessage); return; }
		const PathUtilities::FSourcePathResult Classified =
			PathUtilities::ClassifySourcePath(Result.FilePath);
		if (!Classified)
		{
			SetError(Classified.Message);
			return;
		}
		if (Classified.NormalizedVirtualPath.size() >= SourceDestinationBuffer.size())
		{
			SetError("The selected source destination is too long for the import form.");
			return;
		}
		SourceDestinationBuffer.fill(0);
		std::memcpy(SourceDestinationBuffer.data(), Classified.NormalizedVirtualPath.data(),
			Classified.NormalizedVirtualPath.size());
		LastSuggestedSourceDestination.clear();
	}

	auto FSceneImportDialog::RefreshPreview(
		const FAssetPath& InDestinationDirectory) -> void
	{
		const std::string Key = std::format(
			"{}|{}|{}|{}|{}|{}|{}",
			SourcePathBuffer.data(),
			InDestinationDirectory.ToString(),
			SourceDestinationBuffer.data(),
			static_cast<uint32>(SourceMode),
			static_cast<int32>(ImportSettings.ForwardAxis),
			static_cast<int32>(ImportSettings.RightAxis),
			static_cast<int32>(ImportSettings.UpAxis));
		if (Key != PreviewKey)
		{
			if (PreviewRequest) CancelAndDrainSceneImportPlan(*PreviewRequest);
			PreviewRequest.reset();
			Preview.reset();
			PreviewKey = Key;
			if (SourceMode == EMountedSourceImportMode::IngestExternal) return;
			const PathUtilities::FSourcePathResult Source =
				PathUtilities::ClassifySourcePath(SourcePathBuffer.data());
			if (!Source)
			{
				Preview = FSceneImportPlanResult{.Message = Source.Message};
				return;
			}
			PreviewRequest = BeginSceneImportPlan({
				.RootSource = {.Path = Source.NormalizedVirtualPath},
				.DestinationDirectory = InDestinationDirectory,
				.MeshSettings = ImportSettings},
				"LevelEditor.SceneImportDialog.Preview");
		}
		if (!PreviewRequest) return;
		FSceneImportPlanResult Completed;
		const AssetImport::EAsyncImportPlanStatus Status =
			PollSceneImportPlan(*PreviewRequest, Completed);
		if (Status != AssetImport::EAsyncImportPlanStatus::Pending)
		{
			Preview = std::move(Completed);
			PreviewRequest.reset();
		}
	}

	auto FSceneImportDialog::Import() -> bool
	{
		if (ImportRequest) return false;
		Callbacks.Clear();
		FAssetPath OutputDirectory;
		std::string Error;
		if (!FAssetPath::TryCreate(
			DestinationDirectory.GetPath(), OutputDirectory, &Error))
		{
			SetError(std::move(Error));
			return false;
		}
		FPreparedSceneSourceBundle Sources;
		if (!PrepareSceneSourceBundle(
			SourcePathBuffer.data(), OutputDirectory.ToString(),
			SourceMode == EMountedSourceImportMode::IngestExternal
				? std::string_view(SourceDestinationBuffer.data()) : std::string_view{},
			Sources, Error))
		{
			SetError(std::move(Error));
			return false;
		}
		// Source ingestion is an explicit authoring operation and remains even if
		// the subsequent asset publication is rejected or fails.
		CommitSceneSourceBundle(Sources);
		ImportRequest = BeginSceneImportPlan({
			.RootSource = Sources.RootSource,
			.DestinationDirectory = OutputDirectory,
			.MeshSettings = ImportSettings},
			"LevelEditor.SceneImportDialog.Execute");
		return false;
	}

	auto FSceneImportDialog::PollImport() -> bool
	{
		if (!ImportRequest) return false;
		FSceneImportPlanResult Planned;
		const AssetImport::EAsyncImportPlanStatus Status =
			PollSceneImportPlan(*ImportRequest, Planned);
		if (Status == AssetImport::EAsyncImportPlanStatus::Pending) return false;
		ImportRequest.reset();
		if (!Planned)
		{
			SetError(Planned.Message);
			return false;
		}
		const FSceneImportExecutionResult Result = ExecuteSceneImport(Planned.Plan);
		if (!Result)
		{
			SetError(Result.Message);
			return false;
		}

		Callbacks.NotifyImportedDirectory(DestinationDirectory.GetPath());
		for (const AssetImport::FImportOutputPreview& Asset
			: Planned.Plan.GetMultiOutputPlan().GetGenericPlan().GetOutputs())
			Asset::UnloadPackage(Asset.AssetPath);
		return true;
	}

	auto FSceneImportDialog::CancelRequests() -> void
	{
		if (PreviewRequest) CancelAndDrainSceneImportPlan(*PreviewRequest);
		if (ImportRequest) CancelAndDrainSceneImportPlan(*ImportRequest);
		PreviewRequest.reset();
		ImportRequest.reset();
	}

	auto FSceneImportDialog::SetError(std::string Message) const -> void
	{
		Callbacks.Report(std::move(Message));
	}
} // namespace Durin
