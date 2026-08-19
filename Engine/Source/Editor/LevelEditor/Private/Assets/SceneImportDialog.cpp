#include "Assets/SceneImportDialog.h"

#include "Assets/AssetDestinationValidation.h"
#include "Assets/MountedSourceImport.h"
#include "AssetLoad.h"
#include "Dialogs/FileDialog.h"
#include "Misc/Paths.h"
#include "Misc/Project.h"
#include "Misc/StringConvert.h"
#include "MonaImGui.h"
#include "Asset/MountedSource.h"
#include "StaticMesh/StaticMesh.h"

namespace Durin::Editor::Level
{
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
		SourceForm.Reset();
		Coordinates.Reset();
		PreviewKey.clear();
		Preview.reset();
		DestinationDirectory.Reset(InDestinationDirectory);
		std::string Error;
		if (!Asset::Forge::EnsureImportedSurfaceMaterial(Error)) SetError(std::move(Error));
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
		SourceForm.DrawMode(
			"Copies an external model transactionally to the explicit mounted source path.");
		const float BrowseButtonWidth = Metrics.StandardButtonWidth;
		if (SourceForm.DrawSourceRow("##ImportSource",
			"Choose an FBX, glTF, or GLB Scene source...", BrowseButtonWidth)) BrowseSource();

		const std::filesystem::path SourcePath(SourcePathBuffer.data());
		const bool bHasSource = SourcePathBuffer[0] != '\0';
		const bool bSourceExists = bHasSource && std::filesystem::is_regular_file(SourcePath);
		if (bHasSource)
			ImGui::TextDisabled("%s", std::format("{}  |  {}", SourcePath.extension().generic_string(), SourcePath.filename().generic_string()).c_str());

		ImGui::Spacing();
		ImGui::SeparatorText("Coordinate system");
		Coordinates.Draw();

		ImGui::Spacing();
		ImGui::SeparatorText("Destination");
		if (DestinationDirectory.DrawRow("Output directory", "##SceneImportDirectory",
			"/Project/Imported/SceneName", "Choose...", BrowseButtonWidth))
			BrowseDestinationDirectory();
		if (SourceForm.DrawDestinationRow("##SceneSourceDestination",
			"/Project/Sources/Models/SceneName/SceneName.fbx", BrowseButtonWidth))
			BrowseSourceDestination();

		const FContentDirectoryValidation DestinationValidation =
			DestinationDirectory.Inspect();
		const bool bEngineAuthoringContext = DestinationValidation.Mount
			&& DestinationValidation.Mount->Owner == PathUtilities::EMountOwner::Engine;
		std::string ImportSettingsError;
		const bool bImportSettingsValid = Coordinates.GetSettings().IsValid(&ImportSettingsError);
		const FMountedSourceImportDiagnostic SourceDiagnostic =
			DestinationValidation.bDirectoryPathValid
			? SourceForm.Inspect(
				DestinationValidation.DirectoryPath.GetView(), bEngineAuthoringContext)
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
				Asset::Forge::CancelAndDrainSceneImportPlan(*PreviewRequest);
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
			const Asset::FImportPreview& ImportPreview =
				Preview->Plan.GetMultiOutputPlan().GetPreview();
			ImGui::TextDisabled("Peer outputs (role, action, destination, estimated CPU/GPU/Disk)");
			for (const Asset::FImportPreviewOutput& Asset : ImportPreview.Outputs)
			{
				const char* Action = "Create";
				switch (Asset.Action)
				{
				case Asset::EImportPreviewAction::Create: Action = "Create"; break;
				case Asset::EImportPreviewAction::Replace: Action = "Replace managed"; break;
				case Asset::EImportPreviewAction::Reference: Action = "Reference"; break;
				case Asset::EImportPreviewAction::KeepDetached: Action = "Keep detached"; break;
				case Asset::EImportPreviewAction::Missing: Action = "Missing"; break;
				case Asset::EImportPreviewAction::Collision: Action = "Collision"; break;
				case Asset::EImportPreviewAction::Orphan: Action = "Orphan"; break;
				}
				ImGui::BulletText("%s  [%s]  %s  (%.2f / %.2f / %.2f MiB)",
					Asset.Output.Role.c_str(), Action,
					Asset.Output.AssetPath.ToString().c_str(),
					static_cast<double>(Asset.Output.EstimatedCpuBytes) / (1024.0 * 1024.0),
					static_cast<double>(Asset.Output.EstimatedGpuBytes) / (1024.0 * 1024.0),
					static_cast<double>(Asset.Output.EstimatedDiskBytes) / (1024.0 * 1024.0));
			}
			ImGui::Spacing();
			ImGui::TextDisabled("Captured sources");
			for (const Asset::FImportSourcePreview& Source : ImportPreview.Sources)
				ImGui::BulletText("%s", Source.SourcePath.Path.c_str());
			ImGui::TextDisabled("Estimate: CPU %.2f MiB  GPU %.2f MiB  Disk %.2f MiB",
				static_cast<double>(ImportPreview.EstimatedCpuBytes) / (1024.0 * 1024.0),
				static_cast<double>(ImportPreview.EstimatedGpuBytes) / (1024.0 * 1024.0),
				static_cast<double>(ImportPreview.EstimatedDiskBytes) / (1024.0 * 1024.0));
			for (const Asset::FImportWarningPreview& Warning : ImportPreview.Warnings)
			{
				const char* Change = Warning.Change == Asset::EImportWarningChange::New
					? "New warning" : Warning.Change == Asset::EImportWarningChange::Resolved
						? "Resolved" : "Previously accepted";
				ImGui::BulletText("%s: %s", Change, Warning.Diagnostic.Message.c_str());
			}
			ImGui::EndChild();
			ImGui::TextDisabled("Mount: %s (%s)  |  %s  |  dependency allowed",
				SourceDiagnostic.Mount->VirtualRoot.c_str(),
				DescribeMountOwner(SourceDiagnostic.Mount->Owner),
				SourceDiagnostic.Mount->bAuthoringWritable ? "writable" : "read-only");
			if (bEngineAuthoringContext)
				ImGui::TextDisabled("Engine authoring: this import writes shared Engine content.");
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
				Request.InitialDirectory = Lookup.Mount->GetContentDir().generic_string();
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
		Coordinates.Reset();
		const std::string SceneName = StringUtils::SanitizeFileName(
			std::filesystem::path(Result.FilePath).stem().generic_string(), "Scene");
		const FProjectInfo* Project = GetCurrentProject();
		DestinationDirectory.SuggestPath(DestinationDirectory.MakeSuggestedPath(SceneName,
			(Project ? Project->MountRoot : "/")
				+ std::string("Imported/")));
		SuggestSourceDestination();
	}

	auto FSceneImportDialog::SuggestSourceDestination() -> void
	{
		if (SourcePathBuffer[0] == '\0') return;
		const std::filesystem::path SourcePath(SourcePathBuffer.data());
		const std::string SceneName = StringUtils::SanitizeFileName(
			SourcePath.stem().generic_string(), "Scene");
		const std::string SuggestedSourceDestination =
			MakeDefaultImportedSourceVirtualPath(
				DestinationDirectory.GetPath(), "Models",
				SourcePath.filename().generic_string(), SceneName);
		SourceForm.SuggestDestination(SuggestedSourceDestination);
	}

	auto FSceneImportDialog::BrowseDestinationDirectory() -> void
	{
		if (DestinationDirectory.Browse("Choose a Scene Output Directory",
			"The selected directory path is too long for the import form.",
			"Scene outputs must be saved inside a package-enabled mount.",
			Callbacks))
			SuggestSourceDestination();
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
		Request.InitialDirectory = Lookup.Mount->GetContentDir().generic_string();
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
		SourceForm.SetDestination(Classified.NormalizedVirtualPath);
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
			static_cast<int32>(Coordinates.GetSettings().ForwardAxis),
			static_cast<int32>(Coordinates.GetSettings().RightAxis),
			static_cast<int32>(Coordinates.GetSettings().UpAxis));
		if (Key != PreviewKey)
		{
			if (PreviewRequest) Asset::Forge::CancelAndDrainSceneImportPlan(*PreviewRequest);
			PreviewRequest.reset();
			Preview.reset();
			PreviewKey = Key;
			if (SourceMode == EMountedSourceImportMode::IngestExternal) return;
			const PathUtilities::FSourcePathResult Source =
				PathUtilities::ClassifySourcePath(SourcePathBuffer.data());
			if (!Source)
			{
				Preview = Asset::Forge::FSceneImportPlanResult{.Message = Source.Message};
				return;
			}
			PreviewRequest = Asset::Forge::BeginSceneImportPlan({
				.RootSource = {.Path = Source.NormalizedVirtualPath},
				.DestinationDirectory = InDestinationDirectory,
				.MeshSettings = Coordinates.GetSettings()},
				"LevelEditor.SceneImportDialog.Preview");
		}
		if (!PreviewRequest) return;
		Asset::Forge::FSceneImportPlanResult Completed;
		const Asset::EAsyncImportPlanStatus Status =
			Asset::Forge::PollSceneImportPlan(*PreviewRequest, Completed);
		if (Status != Asset::EAsyncImportPlanStatus::Pending)
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
		Asset::Forge::FPreparedSceneSourceBundle Sources;
		if (!Asset::Forge::PrepareSceneSourceBundle(
			SourcePathBuffer.data(), OutputDirectory.ToString(),
			SourceMode == EMountedSourceImportMode::IngestExternal
				? std::string_view(SourceDestinationBuffer.data()) : std::string_view{},
			Sources, Error, IsEngineAuthoringDestination(OutputDirectory.GetView())))
		{
			SetError(std::move(Error));
			return false;
		}
		// Source ingestion is an explicit authoring operation and remains even if
		// the subsequent asset publication is rejected or fails.
		Asset::Forge::CommitSceneSourceBundle(Sources);
		ImportRequest = Asset::Forge::BeginSceneImportPlan({
			.RootSource = Sources.RootSource,
			.DestinationDirectory = OutputDirectory,
			.MeshSettings = Coordinates.GetSettings()},
			"LevelEditor.SceneImportDialog.Execute");
		return false;
	}

	auto FSceneImportDialog::PollImport() -> bool
	{
		if (!ImportRequest) return false;
		Asset::Forge::FSceneImportPlanResult Planned;
		const Asset::EAsyncImportPlanStatus Status =
			Asset::Forge::PollSceneImportPlan(*ImportRequest, Planned);
		if (Status == Asset::EAsyncImportPlanStatus::Pending) return false;
		ImportRequest.reset();
		if (!Planned)
		{
			SetError(Planned.Message);
			return false;
		}
		const Asset::Forge::FSceneImportExecutionResult Result = Asset::Forge::ExecuteSceneImport(Planned.Plan);
		if (!Result)
		{
			SetError(Result.Message);
			return false;
		}

		Callbacks.NotifyImportedDirectory(DestinationDirectory.GetPath());
		for (const Asset::FImportOutputPreview& Asset
			: Planned.Plan.GetMultiOutputPlan().GetGenericPlan().GetOutputs())
			Asset::UnloadPackage(Asset.AssetPath);
		return true;
	}

	auto FSceneImportDialog::CancelRequests() -> void
	{
		if (PreviewRequest) Asset::Forge::CancelAndDrainSceneImportPlan(*PreviewRequest);
		if (ImportRequest) Asset::Forge::CancelAndDrainSceneImportPlan(*ImportRequest);
		PreviewRequest.reset();
		ImportRequest.reset();
	}

	auto FSceneImportDialog::SetError(std::string Message) const -> void
	{
		Callbacks.Report(std::move(Message));
	}
} // namespace Durin::Editor::Level
