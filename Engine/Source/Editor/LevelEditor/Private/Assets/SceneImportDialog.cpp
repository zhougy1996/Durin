#include "Assets/SceneImportDialog.h"

#include "Assets/AssetDestinationValidation.h"
#include "Assets/MountedSourceImport.h"
#include "AssetAuthoring.h"
#include "ImportService.h"
#include "Dialogs/FileDialog.h"
#include "Misc/Paths.h"
#include "Misc/Project.h"
#include "Misc/StringConvert.h"
#include "MonaImGui.h"
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
		const bool bImportFinished = PollImport();

		const MonaImGui::FUIStyleMetrics Metrics = MonaImGui::GetUIStyleMetrics();
		ImGui::SetNextWindowSize(ImVec2(Metrics.WidePopupWidth, 0.0f), ImGuiCond_Appearing);
		if (!ImGui::BeginPopupModal(
			"Import Scene Source",
			nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings
		))
			return;
		if (bImportFinished)
		{
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return;
		}
		const bool bImportPending = SourceRequest.has_value()
			|| InterchangeRequest.has_value();
		if (InterchangeRequest) ImportProgress.Refresh();
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
				Asset::GetImportService().CancelImportOperation(
					PreviewRequest->GetOperationHandle());
				PreviewRequest.reset();
			}
			PreviewKey.clear();
			Preview.reset();
		}

		if (DestinationValidation.bDirectoryPathValid
			&& DestinationValidation.bMountedDestination && bHasSource
			&& SourceDiagnostic.bValid && Preview
			&& Preview->Outcome.State == Asset::EImportOperationState::Succeeded
			&& Preview->Inspection.bCompatible)
		{
			ImGui::BeginChild("ImportOutputPreview",
				ImVec2(0.0f, MonaImGui::ScaleUI(190.0f)), ImGuiChildFlags_Borders);
			ImGui::TextDisabled("Peer outputs (role, policy, destination)");
			for (const Asset::FImportOutputPreview& Output : Preview->Inspection.Outputs)
			{
				const char* Policy = Output.Policy == Asset::EImportOutputPolicy::Create
					? "Create" : "Replace managed";
				ImGui::BulletText("%s  [%s]  %s", Output.Role.c_str(), Policy,
					Output.AssetPath.ToString().c_str());
			}
			ImGui::Spacing();
			ImGui::TextDisabled("Captured sources");
			for (const Asset::FImportSourcePreview& Source : Preview->Inspection.Sources)
				ImGui::BulletText("%s", Source.SourcePath.Path.c_str());
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
		else if (Preview
			&& Preview->Outcome.State != Asset::EImportOperationState::Succeeded)
			ValidationMessage = Preview->Outcome.Diagnostic;
		else if (SourceRequest)
			ValidationMessage = "Capturing Scene sources in the background...";
		else if (InterchangeRequest)
			ValidationMessage = "Scene Interchange import is running...";

		DrawImportDialogWarning(ValidationMessage);

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::EndDisabled();
		if (bImportPending)
		{
			if (SourceRequest)
			{
				ImGui::TextUnformatted("Capturing Scene sources");
				const float Progress = static_cast<float>(
					std::fmod(ImGui::GetTime() * 0.65, 1.0));
				ImGui::ProgressBar(
					Progress, ImVec2(-std::numeric_limits<float>::min(), 0.0f));
				if (ImGui::Button("Run in Background")) ImGui::CloseCurrentPopup();
				ImGui::SameLine();
				if (ImGui::Button("Cancel"))
				{
					Asset::Forge::CancelAndDrainSceneSourceBundlePreparation(*SourceRequest);
					SourceRequest.reset();
					PendingImportDirectory.reset();
				}
				ImGui::EndPopup();
				return;
			}
			const Asset::FImportOperationSnapshot& Snapshot = ImportProgress.GetSnapshot();
			const std::string Overlay = Snapshot.Progress
				? std::format("{}%", static_cast<int>(*Snapshot.Progress * 100.0f))
				: std::string{};
			ImGui::TextUnformatted(Asset::GetImportPhaseLabel(Snapshot.Phase).data());
			if (!Snapshot.SourceIdentity.empty() && Snapshot.SourceIdentity != "root")
				ImGui::TextDisabled("Current source: %s", Snapshot.SourceIdentity.c_str());
			const float Progress = Snapshot.Progress.value_or(
				static_cast<float>(std::fmod(ImGui::GetTime() * 0.65, 1.0)));
			ImGui::ProgressBar(Progress, ImVec2(-std::numeric_limits<float>::min(), 0.0f),
				Overlay.empty() ? nullptr : Overlay.c_str());
			if (ImGui::Button("Run in Background"))
			{
				ImportProgress.RunInBackground();
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			ImGui::BeginDisabled(!ImportProgress.CanCancel());
			if (ImGui::Button(Snapshot.State == Asset::EImportOperationState::Canceling
				? "Canceling..." : "Cancel")) ImportProgress.RequestCancel();
			ImGui::EndDisabled();
		}
		else
		{
			ImGui::BeginDisabled(!ValidationMessage.empty());
			if (ImGui::Button("Import Scene", ImVec2(MonaImGui::ScaleUI(150.0f), 0.0f))
				&& Import()) ImGui::CloseCurrentPopup();
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (MonaImGui::DialogButton("Cancel", true))
			{
				CancelRequests();
				ImGui::CloseCurrentPopup();
			}
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
			if (PreviewRequest) Asset::GetImportService().CancelImportOperation(
				PreviewRequest->GetOperationHandle());
			PreviewRequest.reset();
			Preview.reset();
			PreviewKey = Key;
			Asset::FInterchangeImportRequest Request;
			std::string Error;
			if (!Asset::Forge::MakeSceneInterchangeRequest(
				{.Path = SourcePathBuffer.data()}, InDestinationDirectory,
				Coordinates.GetSettings(), Asset::EInterchangeImportMode::Preview,
				{.OwnerId = "LevelEditor.SceneImportDialog.Preview"}, {}, Request, Error))
			{
				Preview = Asset::FInterchangeImportResult{
					.Outcome = {.State = Asset::EImportOperationState::Failed,
						.Diagnostic = std::move(Error)}};
				return;
			}
			Request.Lifetime = Asset::EImportOperationLifetime::EphemeralPreview;
			PreviewRequest = Asset::GetImportService().SubmitInterchangeImport(
				std::move(Request), "Preview Scene import");
		}
		if (!PreviewRequest) return;
		Asset::FInterchangeImportResult Completed;
		if (PreviewRequest->TryGetResult(Completed))
		{
			Preview = std::move(Completed);
			PreviewRequest.reset();
		}
	}

	auto FSceneImportDialog::Import() -> bool
	{
		if (SourceRequest || InterchangeRequest) return false;
		Callbacks.Clear();
		FAssetPath OutputDirectory;
		std::string Error;
		if (!FAssetPath::TryCreate(
			DestinationDirectory.GetPath(), OutputDirectory, &Error))
		{
			SetError(std::move(Error));
			return false;
		}
		PendingImportDirectory = OutputDirectory;
		SourceRequest = Asset::Forge::BeginSceneSourceBundlePreparation(
			SourcePathBuffer.data(), OutputDirectory.ToString(),
			SourceMode == EMountedSourceImportMode::IngestExternal
				? std::string(SourceDestinationBuffer.data()) : std::string{},
			IsEngineAuthoringDestination(OutputDirectory.GetView()));
		return false;
	}

	auto FSceneImportDialog::PollImport() -> bool
	{
		if (SourceRequest)
		{
			Asset::Forge::FPreparedSceneSourceBundle Sources;
			std::string Error;
			const Asset::EAsyncImportPlanStatus Status =
				Asset::Forge::PollSceneSourceBundlePreparation(
					*SourceRequest, Sources, Error);
			if (Status == Asset::EAsyncImportPlanStatus::Pending) return false;
			SourceRequest.reset();
			if (Status != Asset::EAsyncImportPlanStatus::Succeeded
				|| !PendingImportDirectory)
			{
				PendingImportDirectory.reset();
				SetError(Error.empty()
					? "Scene source preparation did not complete." : std::move(Error));
				return false;
			}
			// Source ingestion is an explicit authoring operation and remains even if
			// the subsequent asset publication is rejected or fails.
			Asset::Forge::CommitSceneSourceBundle(Sources);
			Asset::FInterchangeImportRequest Request;
			if (!Asset::Forge::MakeSceneInterchangeRequest(
				Sources.RootSource, *PendingImportDirectory, Coordinates.GetSettings(),
				Asset::EInterchangeImportMode::Import,
				{.OwnerId = "LevelEditor.SceneImportDialog.Execute",
					.ConflictIdentities = {PendingImportDirectory->ToString()}},
				{}, Request, Error))
			{
				PendingImportDirectory.reset();
				SetError(std::move(Error));
				return false;
			}
			InterchangeRequest = Asset::GetImportService().SubmitInterchangeImport(
				std::move(Request), "Importing Scene");
			PendingImportDirectory.reset();
			if (InterchangeRequest && *InterchangeRequest)
			{
				ImportProgress.Begin(InterchangeRequest->GetOperationHandle());
				Callbacks.NotifyImportStarted(
					InterchangeRequest->GetOperationHandle(), "Importing Scene");
			}
			else SetError("Scene Interchange import could not be submitted.");
			return false;
		}
		if (!InterchangeRequest) return false;
		Asset::FInterchangeImportResult Result;
		if (!InterchangeRequest->TryGetResult(Result)) return false;
		InterchangeRequest.reset();
		ImportProgress.Reset();
		if (Result.Outcome.State != Asset::EImportOperationState::Succeeded)
		{
			SetError(Result.Outcome.Diagnostic.empty()
				? "Scene Interchange import failed." : Result.Outcome.Diagnostic);
			return false;
		}
		Callbacks.NotifyImportedDirectory(DestinationDirectory.GetPath());
		for (const Asset::FInterchangeOutputMapping& Output : Result.Provenance.OutputMappings)
			Asset::UnloadPackage(Output.AssetPath);
		return true;
	}

	auto FSceneImportDialog::CancelRequests() -> void
	{
		if (PreviewRequest)
			Asset::GetImportService().CancelImportOperation(
				PreviewRequest->GetOperationHandle());
		if (SourceRequest)
			Asset::Forge::CancelAndDrainSceneSourceBundlePreparation(*SourceRequest);
		if (InterchangeRequest)
			Asset::GetImportService().CancelAndDrainImportOperation(
				InterchangeRequest->GetOperationHandle());
		PreviewRequest.reset();
		SourceRequest.reset();
		PendingImportDirectory.reset();
		InterchangeRequest.reset();
		ImportProgress.Reset();
	}

	auto FSceneImportDialog::SetError(std::string Message) const -> void
	{
		Callbacks.Report(std::move(Message));
	}
} // namespace Durin::Editor::Level
