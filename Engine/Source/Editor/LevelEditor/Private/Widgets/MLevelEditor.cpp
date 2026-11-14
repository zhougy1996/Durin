#include "Asset/RegistryOperations.h"
#include "Misc/UnifiedError.h"
#include "Asset/LoadErrors.h"
#include "ThirdParty/ImGui/imgui_internal.h"
#include "Widgets/MLevelEditor.h"

#include "Asset/Asset.h"
#include "Asset/AssetPicker.h"
#include "Editor/EditorEngine.h"
#include "Notifications/Notification.h"
#include "Transactions/Transaction.h"
#include "Documents/EditorDocumentManager.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Framework/TabLayout.h"
#include "Settings/LevelEditorSessionSettings.h"
#include "Assets/EditorAssetMoveCoordinator.h"
#include "Engine/Engine.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Engine/ProjectGameSettings.h"
#include "Engine/World.h"
#include "Documents/LevelDocumentController.h"
#include "Documents/LevelDocumentRevisionState.h"
#include "Workspace/LevelEditorContext.h"
#include "Workspace/LevelEditorWorkspace.h"
#include "Workspace/LevelEditorPresentationPolicy.h"
#include "Project/Project.h"
#include "Math/Operations.h"
#include "MonaImGui.h"
#include "Widgets/MonaImGuiWidgets.h"
#include "Icons/FontAwesomeIcons.h"
#include "Panels/DetailsPanel.h"
#include "Panels/LevelEditorPanel.h"
#include "Panels/SceneViewportPanel.h"
#include "Panels/RenderingDiagnosticsPanel.h"
#include "Panels/WorldOutlinerPanel.h"
#include "Profiling/Profiling.h"
#include "Assets/SceneImportDialog.h"

namespace Durin::LevelEditor
{
	namespace
	{
		template<typename TDialog>
		auto MakeImportDialog(const FImportDialogCallbacks& Callbacks)
			-> std::unique_ptr<TDialog>
		{
			return std::make_unique<TDialog>(Callbacks);
		}
	} // namespace

	// MLevelEditor is the composition root for the editor-specific controllers.
	MLevelEditor::MLevelEditor(std::shared_ptr<FLevelEditorSessionSettings> InSessionSettings,
		FEditorDocumentManager& InWorkspaceManager,
		FContentBrowserCallbacks InContentBrowserCallbacks,
		std::function<void()> InOpenProjectBrowser, DEditorEngine& EditorHost)
		: EditorOwner(&EditorHost)
		, SessionSettingsOwner(std::move(InSessionSettings))
		, SessionSettings(*SessionSettingsOwner)
		, DocumentManager(InWorkspaceManager)
		, ContentBrowserCallbacks(std::move(InContentBrowserCallbacks))
		, OpenProjectBrowser(std::move(InOpenProjectBrowser))
	{
	}

	MLevelEditor::~MLevelEditor() { ReleaseSessionUI(); }

	auto MLevelEditor::ReleaseSessionUI() -> void
	{
		if (bReleased) return;
		bReleased = true;
		FBaseAssetToolkit::ReleaseSessionUI();
		ViewportSettingsPage = {};
		ProjectSettingsPage = {};
		RequestDeactivate();
		if (Context && SceneViewportPanel)
		{
			SessionSettings.CaptureViewportState(*Context, *SceneViewportPanel);
			SessionSettings.Save(SceneViewportPanel);
		}
		if (DocumentController) (void)DocumentController->CloseLevel();
		DocumentController.reset();
		AssetMoveCoordinator.reset();
		SceneImportDialog.reset();
		Panels.clear();
		SceneViewportPanel = nullptr; RenderingDiagnosticsPanel = nullptr; DetailsPanel = nullptr;
		Context.reset();
		ContentBrowserCallbacks = {};
		OpenProjectBrowser = {};
	}

	auto MLevelEditor::Construct() -> void
	{
		InitializeContext();
		InitializeSession();
		CreatePanels();
		CreateDocumentServices();
		CreateImportDialogs();
		Context->Synchronize(Context->EditorHost->GetEditorWorld());
		ViewportSettingsPage = FEditorSettingsPageRegistration({
			.Id = "editor.viewport.host." + std::to_string(FObjectKeyHash{}(FObjectKey(EditorOwner.Get()))), .Label = "Viewport and Controls",
			.Keywords = "camera movement speed grid statistics snap translation rotation scale gizmo",
			.Description = "Level viewport navigation, overlays and transform snapping. Changes take effect immediately and stay in sync with the viewport toolbar.",
			.Draw = [this](std::string& Error) { DrawViewportPreferences(Error); },
			.Reset = [this] { return ResetViewportPreferences(); },
		});
		ProjectSettingsPage = FEditorSettingsPageRegistration({
			.Id = "project.general.host." + std::to_string(FObjectKeyHash{}(FObjectKey(EditorOwner.Get()))), .Label = "General and Startup",
			.Keywords = "project name file default level startup",
			.Description = "Settings shared by the current project. Apply saves the default startup level.",
			.bProject = true,
			.Draw = [this](std::string& Error) { DrawProjectSettings(Error); },
			.OnOpen = [this] { (void)LoadProjectSettings(); PendingDefaultLevel = DefaultLevel; },
		});
	}

	auto MLevelEditor::InitializeContext() -> void
	{
		Context = std::make_unique<FLevelEditorContext>();
		Context->EditorHost = EditorOwner.Get();
		Context->ReportError = [this](std::string Message) { SetError(std::move(Message)); };
		Context->RenameLevel = [this](std::string_view NewName) {
			return DocumentController && DocumentController->RenameCurrentLevel(NewName);
		};
		Context->StartPlay = [this](EPlayStartLocation StartLocation, EPlayDestination Destination) {
			StartPlay(StartLocation, Destination);
		};
		Context->ApplyPlayChanges = [this](bool bSelectedOnly) { ApplyPlayChanges(bSelectedOnly); };
		Context->RevealAsset = [this](const FPackagePath& Path, std::string& Error) {
			if (ContentBrowserCallbacks.RevealAsset
				&& ContentBrowserCallbacks.RevealAsset(Path.ToString()))
				return true;
			Error = "The asset could not be revealed in the Content Browser.";
			return false;
		};
		Context->OpenAsset = [this](const FPackagePath& Path, std::string& Error) {
			const FPackagePathResolveResult Resolution =
				ResolvePackagePathForOperation(Path);
			if (Resolution && Resolution.FinalPackageData
				&& Resolution.FinalPackageData->TopLevelAssets.size() == 1
				&& OpenRelatedAsset(Resolution.FinalPath.ToString(), Resolution.FinalPackageData->TopLevelAssets.front().AssetClassName)) return true;
			Error = "The loaded asset could not be opened.";
			return false;
		};
	}

	auto MLevelEditor::InitializeSession() -> void
	{
		SessionSettings.PruneInvalidViewportStates();
		SessionSettings.Save(nullptr);
	}

	auto MLevelEditor::CreatePanels() -> void
	{
		auto SceneViewport = std::make_unique<FSceneViewportPanel>(Context->EditorHost);
		SceneViewportPanel = SceneViewport.get();
		Context->FocusActor = [this](AActor* Actor) {
			if (SceneViewportPanel) SceneViewportPanel->FocusActor(Actor);
		};
		SessionSettings.ApplyTo(*SceneViewportPanel);
		Panels.emplace_back(std::move(SceneViewport));
		auto RenderingDiagnostics =
			std::make_unique<FRenderingDiagnosticsPanel>(*SceneViewportPanel);
		RenderingDiagnosticsPanel = RenderingDiagnostics.get();
		SceneViewportPanel->SetOpenRenderingDiagnostics([this] {
			if (RenderingDiagnosticsPanel)
				RenderingDiagnosticsPanel->SetOpen(true);
		});
		Panels.emplace_back(std::move(RenderingDiagnostics));
		Panels.emplace_back(std::make_unique<FWorldOutlinerPanel>());
		auto Details = std::make_unique<FDetailsPanel>(SessionSettings);
		DetailsPanel = Details.get();
		Panels.emplace_back(std::move(Details));
	}

	auto MLevelEditor::CreateDocumentServices() -> void
	{
		AssetMoveCoordinator = std::make_unique<FEditorAssetMoveCoordinator>(
			*Context,
			SessionSettings,
			*SceneViewportPanel
		);

		DocumentController = std::make_unique<FLevelDocumentController>(
			*Context,
			SessionSettings,
			*SceneViewportPanel,
			*AssetMoveCoordinator,
			[this] { EditorError.clear(); },
			[this](std::string Message) { SetError(std::move(Message)); },
			[this](bool bSucceeded) {
				if (!DeferredOpenDocumentId.IsValid())
				{
					if (bSucceeded && Context && Context->Level)
						DocumentManager.OpenDocument({.ToolkitId = GetToolkitId(), .DocumentKey = "LevelEditor",
							.ResourceId = Context->Level->GetObjectPath(), .Label = "Level Editor"});
					return;
				}
				const FDocumentId CompletedId = std::exchange(DeferredOpenDocumentId, {});
				if (!DocumentManager.CompleteDeferredDocumentOpen(CompletedId, bSucceeded))
					SetError("The deferred level document request is no longer available.");
			}
		);
	}

	auto MLevelEditor::CreateImportDialogs() -> void
	{
		const FImportDialogCallbacks ImportCallbacks{
			.ClearError = [this] { EditorError.clear(); },
			.ReportError =
				[this](std::string Message) { SetError(std::move(Message)); },
			.AssetCreated = [this](std::string AssetPath) {
				if (ContentBrowserCallbacks.NotifyMountedContentChanged)
					ContentBrowserCallbacks.NotifyMountedContentChanged();
				else if (Context->EditorHost)
					Context->EditorHost->GetTransactor()->NotifyMountedContentMutation();
				if (ContentBrowserCallbacks.RevealAsset)
					ContentBrowserCallbacks.RevealAsset(AssetPath);
			},
			.ImportedDirectory = [this](std::string DirectoryPath) {
				if (ContentBrowserCallbacks.NotifyMountedContentChanged)
					ContentBrowserCallbacks.NotifyMountedContentChanged();
				else if (Context->EditorHost)
					Context->EditorHost->GetTransactor()->NotifyMountedContentMutation();
				if (ContentBrowserCallbacks.RevealDirectory)
					ContentBrowserCallbacks.RevealDirectory(DirectoryPath);
			},
		};
		SceneImportDialog =
			MakeImportDialog<FSceneImportDialog>(ImportCallbacks);
	}

	auto MLevelEditor::RequestContentBrowserImport(
		std::string_view Directory,
		EImportDialogType Type) -> void
	{
		switch (Type)
		{
		case EImportDialogType::Scene:
			if (SceneImportDialog) SceneImportDialog->Open(Directory);
			break;
		}
	}

	auto MLevelEditor::DrawContentBrowserImport(
		EImportDialogType Type, bool bAllowAssetMutation) -> void
	{
		switch (Type)
		{
		case EImportDialogType::Scene:
			if (SceneImportDialog)
				SceneImportDialog->Draw(bAllowAssetMutation);
			break;
		}
	}

	auto MLevelEditor::LoadProjectSettings() -> bool
	{
		DefaultLevel.Reset();
		const FProjectInfo* Project = GetCurrentProject();
		if (!Project) return false;
		FSoftObjectPath Path;
		std::string Error;
		if (!EditorStartup::ResolveStartupLevel(*Project, Path, Error))
		{
			SetError(std::move(Error));
			return false;
		}
		if (Path.IsValid()) DefaultLevel = std::move(Path);
		return true;
	}

	auto MLevelEditor::SaveProjectSettings() -> bool
	{
		const FProjectInfo* Project = GetCurrentProject();
		if (!Project)
		{
			SetError("No project is open.");
			return false;
		}
		const FPackagePath& DefaultLevelPath =
			DefaultLevel.ToSoftObjectPath().GetPackagePath();
		if (!DefaultLevel.IsNull()
			&& !DefaultLevelPath.GetView().starts_with(Project->MountRoot))
		{
			SetError("The default level must belong to the current project.");
			return false;
		}
		FSoftObjectPath ResolvedLevelPath;
		const auto Resolution = DefaultLevel.IsNull()
			? std::expected<void, Durin::UnifiedError::FError>{}
			: ResolveLevelPackage(DefaultLevelPath, ResolvedLevelPath);
		if (!Resolution
			|| (!DefaultLevel.IsNull()
				&& ResolvedLevelPath != DefaultLevel.ToSoftObjectPath()))
		{
			SetError(Resolution
				? "The selected Level is not the unique top-level Level in its package."
				: Resolution.error().ToString());
			return false;
		}
		const FProjectGameSettingsResult SaveResult =
			FProjectGameSettingsStore::ForProject(*Project).SaveDefaultLevel(
				DefaultLevelPath.ToString());
		if (!SaveResult)
		{
			SetError(SaveResult.Message);
			return false;
		}
		return true;
	}

	auto MLevelEditor::ApplyFixedUpDefaultLevelPath(
		const FPackagePath& Path) -> void
	{
		FSoftObjectPath LevelPath;
		if (!ResolveLevelPackage(Path, LevelPath)) return;
		const bool bPendingMatchesSaved = PendingDefaultLevel == DefaultLevel;
		DefaultLevel = LevelPath;
		if (bPendingMatchesSaved)
			PendingDefaultLevel = std::move(LevelPath);
	}

	auto MLevelEditor::GetToolkitId() const -> const FAssetEditorToolkitId&
	{
		return Workspace::Type;
	}

	auto MLevelEditor::OpenLevel(const std::string& AssetPath) -> bool
	{
		if (!DocumentController) return false;
		const auto Result = DocumentController->OpenLevel(AssetPath);
		if (Result == ELevelDocumentOpenResult::Rejected) return false;
		if (Result == ELevelDocumentOpenResult::Opened)
			return DocumentManager.OpenDocument({.ToolkitId = GetToolkitId(), .DocumentKey = "LevelEditor",
				.ResourceId = AssetPath, .Label = "Level Editor"}).IsValid();
		return true;
	}
	auto MLevelEditor::CancelPendingOpen() -> bool
	{
		if (!DocumentController || !DocumentController->IsOpening()) return true;
		DocumentController->CancelPendingOpen();
		return !DocumentController->IsOpening();
	}

	auto MLevelEditor::OpenDocument(const FEditorDocument& Document) -> EDocumentOpenResult
	{
		if (Document.ResourceId.empty()) return EDocumentOpenResult::Opened;
		if (!DocumentController) return EDocumentOpenResult::Rejected;
		if (!DocumentController->LoadLevel(Document.ResourceId)) return EDocumentOpenResult::Rejected;
		if (!DocumentController->IsOpening()) return EDocumentOpenResult::Opened;
		DeferredOpenDocumentId = Document.Id;
		return EDocumentOpenResult::Deferred;
	}

	auto MLevelEditor::ActivateDocument(const FEditorDocument& Document) -> void
	{
		(void)Document;
		RootWindow.RequestFocus();
	}

	auto MLevelEditor::RequestDeactivate() -> bool
	{
		return !Context || !DetailsPanel || DetailsPanel->RequestDeactivate(*Context);
	}

	auto MLevelEditor::RequestCloseDocument(const FEditorDocument& Document) -> EDocumentCloseResult
	{
		if (!RequestDeactivate()) return EDocumentCloseResult::Rejected;
		if (IsDocumentDirty(Document)) return EDocumentCloseResult::PendingConfirmation;
		if (DocumentController && !DocumentController->CloseLevel()) return EDocumentCloseResult::Rejected;
		return EDocumentCloseResult::Closed;
	}

	auto MLevelEditor::OnSaveDocument(const FEditorDocument& Document) -> bool
	{
		(void)Document;
		return SaveActiveDocument();
	}

	auto MLevelEditor::OnDiscardDocument(const FEditorDocument& Document) -> bool
	{
		(void)Document;
		if (!Context || !Context->Level || !Context->Level->GetPackage()) return false;
		DPackage* Package = Context->Level->GetPackage();
		FLevelDocumentRevisionState::Discard(
			Context->EditorHost ? Context->EditorHost->GetTransactor() : nullptr, *Package
		);
		return true;
	}

	auto MLevelEditor::OnIsDocumentDirty(const FEditorDocument& Document) const -> bool
	{
		(void)Document;
		return Context && Context->Level && Context->Level->GetPackage() && Context->Level->GetPackage()->IsDirty();
	}

	auto MLevelEditor::ResetLayout() -> void
	{
		bResetLayoutRequested = true;
		for (const std::unique_ptr<ILevelEditorPanel>& Panel : Panels)
		{
			const ELevelEditorPanelRole Role = Panel.get() == RenderingDiagnosticsPanel
					? ELevelEditorPanelRole::Optional
					: ELevelEditorPanelRole::Persistent;
			const bool bDefaultOpen = IsLevelEditorPanelOpenByDefault(Role);
			Panel->SetOpen(bDefaultOpen);
		}
	}

	auto MLevelEditor::OnDrawToolkit(bool bActive) -> bool
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("LevelEditor.DrawToolkit");
		if (!Context || !DocumentController) return false;
		DocumentController->DrawDialogs();
		bWasActive = bActive;
		const bool bDocumentOpen = std::ranges::any_of(DocumentManager.GetDocuments(), [](const FEditorDocument& Document) {
			return Document.ToolkitId == Workspace::Type;
		});
		if (!bDocumentOpen)
		{
			RootWindow.ResetActivationState();
			return false;
		}

		const ImGuiID DockSpaceId = TabLayout::MakeDockSpaceId(Workspace::Type, Workspace::LayoutVersion);
		const bool bLevelDirty = Context->Level && Context->Level->GetPackage() && Context->Level->GetPackage()->IsDirty();
		const FEditorTabWindowState RootWindowState = RootWindow.Begin({
			.DisplayName = "Level Editor",
			.RootKey = Workspace::RootKey,
			.bDirty = bLevelDirty,
			.bZeroPadding = true,
			.InternalDockSpace = FToolkitInternalDockSpace{
				.ToolkitId = Workspace::Type,
				.LayoutVersion = Workspace::LayoutVersion,
			},
		});
		if (!RootWindowState.bVisible)
		{
			RootWindow.End();
			if (RootWindowState.bCloseRequested)
			{
				const FEditorDocument* ActiveDocument = DocumentManager.GetActiveDocument();
				if (ActiveDocument && ActiveDocument->ToolkitId == Workspace::Type)
					DocumentManager.RequestCloseDocument(ActiveDocument->Id);
			}
			return RootWindowState.bActivated;
		}

		bool bPlaying = Context->EditorHost && Context->EditorHost->IsPlaying();
		Context->bReadOnly = DocumentController->IsOpening()
			|| (Context->EditorHost && !Context->EditorHost->GetCapabilities().bEditWorld);
		Context->Synchronize(Context->EditorHost != nullptr
			? (bPlaying ? Context->EditorHost->GetPlayWorld() : Context->EditorHost->GetEditorWorld())
			: (GEngine != nullptr ? GEngine->GetWorld() : nullptr));
		const ImGuiIO& IO = ImGui::GetIO();
		if (bActive || RootWindowState.bFocused)
		{
			if (!DocumentController->IsOpening() && !IO.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F5, false) && Context->EditorHost)
			{
				if (Context->EditorHost->IsPlaying()) Context->EditorHost->StopPlaySession();
				else
				{
					StartPlay(EPlayStartLocation::LevelStart, IO.KeyCtrl ? EPlayDestination::NewWindow : EPlayDestination::EmbeddedViewport);
				}
			}
			if (!IO.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F6, false) && Context->EditorHost && Context->EditorHost->IsPlaying()) Context->EditorHost->SetPlaySessionPaused(!Context->EditorHost->IsPlaySessionPaused());
			if (!IO.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F7, false) && Context->EditorHost) Context->EditorHost->StepPlaySession();
		}
		if (Context->EditorHost && bPlaying != Context->EditorHost->IsPlaying())
		{
			bPlaying = Context->EditorHost->IsPlaying();
			Context->bReadOnly = DocumentController->IsOpening()
				|| !Context->EditorHost->GetCapabilities().bEditWorld;
			Context->Synchronize(bPlaying ? Context->EditorHost->GetPlayWorld() : Context->EditorHost->GetEditorWorld());
		}
		ImVec2 DockSpaceSize = ImGui::GetContentRegionAvail();
		const bool bNeedsDefaultLayout = ImGui::DockBuilderGetNode(DockSpaceId) == nullptr;
		const bool bCanBuildDefaultLayout = DockSpaceSize.x >= MonaImGui::ScaleUI(900.0f)
			&& DockSpaceSize.y >= MonaImGui::ScaleUI(500.0f);
		if ((bNeedsDefaultLayout || bResetLayoutRequested) && bCanBuildDefaultLayout)
		{
			// DockBuilder must finish before DockSpace submission so the new tree retains this frame's host window.
			BuildDefaultLayout(DockSpaceId, DockSpaceSize.x, DockSpaceSize.y);
			bResetLayoutRequested = false;
		}
		const ImVec2 ItemSpacing = ImGui::GetStyle().ItemSpacing;
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ItemSpacing.x, 0.0f));
		TabLayout::SubmitDockSpace(Workspace::Type, Workspace::LayoutVersion, DockSpaceSize);
		ImGui::PopStyleVar();
		RootWindow.End();
		if (RootWindowState.bCloseRequested)
		{
			const FEditorDocument* ActiveDocument = DocumentManager.GetActiveDocument();
			if (ActiveDocument && ActiveDocument->ToolkitId == Workspace::Type)
				DocumentManager.RequestCloseDocument(ActiveDocument->Id);
		}


		MonaImGui::ErrorDialog("Editor Error", EditorError);
		for (const std::unique_ptr<ILevelEditorPanel>& Panel : Panels)
		{
			if (!Panel->IsOpen())
			{
				Panel->TickWhenHidden();
				continue;
			}
			Panel->Draw(*Context);
		}
		if (SceneViewportPanel) SceneViewportPanel->FinalizeViewportFrame(*Context);
		return RootWindowState.bFocused || RootWindowState.bActivated;
	}

	auto MLevelEditor::OnCanSaveActiveDocument() const -> bool
	{
		return DocumentController && DocumentController->CanSaveCurrentLevel();
	}

	auto MLevelEditor::OnSaveActiveDocument() -> bool
	{
		return CanSaveActiveDocument() && DocumentController->SaveCurrentLevel();
	}

	auto MLevelEditor::OnCanUndo() const -> bool
	{
		const bool bDragging = SceneViewportPanel && SceneViewportPanel->GetTransformGizmo() && SceneViewportPanel->GetTransformGizmo()->IsDragging();
		return !(DocumentController && DocumentController->IsOpening())
			&& !(Context->EditorHost && Context->EditorHost->IsPlaying()) && !bDragging && Context->EditorHost && Context->EditorHost->GetTransactor()->CanUndo();
	}

	auto MLevelEditor::OnCanRedo() const -> bool
	{
		const bool bDragging = SceneViewportPanel && SceneViewportPanel->GetTransformGizmo() && SceneViewportPanel->GetTransformGizmo()->IsDragging();
		return !(DocumentController && DocumentController->IsOpening())
			&& !(Context->EditorHost && Context->EditorHost->IsPlaying()) && !bDragging && Context->EditorHost && Context->EditorHost->GetTransactor()->CanRedo();
	}

	auto MLevelEditor::GetUndoDescription() const -> std::string
	{
		return CanUndo() ? Context->EditorHost->GetTransactor()->GetUndoContext().Description : std::string{};
	}

	auto MLevelEditor::GetRedoDescription() const -> std::string
	{
		return CanRedo() ? Context->EditorHost->GetTransactor()->GetRedoContext().Description : std::string{};
	}

	auto MLevelEditor::OnUndo() -> bool
	{
		return CanUndo() && Context->EditorHost->GetTransactor()->Undo();
	}

	auto MLevelEditor::OnRedo() -> bool
	{
		return CanRedo() && Context->EditorHost->GetTransactor()->Redo();
	}

	auto MLevelEditor::DrawFileMenu() -> void
	{
		const bool bPlaying = Context->EditorHost && Context->EditorHost->IsPlaying();
		if (bPlaying) ImGui::BeginDisabled();
		if (ImGui::MenuItem("Open Project...") && OpenProjectBrowser)
			OpenProjectBrowser();
		if (bPlaying) ImGui::EndDisabled();
	}

	auto MLevelEditor::RequestOpenProject(std::string ProjectFile) -> void
	{
		if (DocumentController)
			DocumentController->RequestOpenProject(std::move(ProjectFile));
	}

	auto MLevelEditor::DrawEditMenu() -> void
	{
		const bool bPlaying = Context->EditorHost && Context->EditorHost->IsPlaying();
		if (bPlaying) ImGui::BeginDisabled();
		if (ImGui::MenuItem("Project Settings..."))
		{
			PendingDefaultLevel = DefaultLevel;
			FEditorSettingsRegistry::Get().RequestOpen("project.general.host." + std::to_string(FObjectKeyHash{}(FObjectKey(EditorOwner.Get()))));
		}
		if (bPlaying) ImGui::EndDisabled();
	}

	auto MLevelEditor::DrawWindowMenu() -> void
	{
		if (ImGui::BeginMenu("Panels###Durin.LevelEditor.Windows"))
		{
			for (const std::unique_ptr<ILevelEditorPanel>& Panel : Panels)
			{
				bool bPanelOpen = Panel->IsOpen();
				if (ImGui::MenuItem(Panel->GetWindowName(), nullptr, &bPanelOpen))
				{
					if (!bPanelOpen && Panel.get() == DetailsPanel && !RequestDeactivate())
						continue;
					Panel->SetOpen(bPanelOpen);
				}
			}
			ImGui::EndMenu();
		}
	}

	auto MLevelEditor::DrawProjectSettings(std::string& OutError) -> void
	{
		const bool bPlaying = Context->EditorHost && Context->EditorHost->IsPlaying();
		if (bPlaying) ImGui::TextDisabled("Stop Play before changing project settings.");
		ImGui::BeginDisabled(bPlaying);
		const FProjectInfo* Project = GetCurrentProject();
		ImGui::Text("Configure the current project and editor defaults.");
		ImGui::Spacing();
		ImGui::SeparatorText("Project");
		if (Project)
		{
			if (ImGui::BeginTable("ProjectInfo", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings))
			{
				ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, MonaImGui::ScaleUI(110.0f));
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextDisabled("Name");
				ImGui::TableSetColumnIndex(1);
				ImGui::TextWrapped("%s", Project->Name.c_str());
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextDisabled("Project file");
				ImGui::TableSetColumnIndex(1);
				ImGui::TextWrapped("%s", Project->ProjectFile.c_str());
				ImGui::EndTable();
			}
			ImGui::Spacing();
			ImGui::SeparatorText("Editor");
			ImGui::AlignTextToFramePadding();
			ImGui::TextDisabled("Default level");
			ImGui::SameLine(MonaImGui::ScaleUI(130.0f));
			ImGui::SetNextItemWidth(-1.0f);
			static std::array<char, 128> LevelSearchText{};
			const FAssetPickerResult PickerResult = AssetPicker::Draw({
				.ComboId = "##DefaultLevel",
				.SearchId = "##DefaultLevelSearch",
				.SearchHint = "Search levels...",
				.RequiredClass = DLevel::StaticClass(),
				.ClassPolicy = EAssetClassPolicy::Exact,
				.AssignmentMode = EAssetAssignmentMode::AssetPath,
				.CurrentSelectionPath =
					PendingDefaultLevel.ToSoftObjectPath().ToString(),
				.SearchText = LevelSearchText,
				.bAllowNone = true,
				.NoneLabel = "None",
				.AssignPathSelection = [this](
					std::string_view SelectionPath, std::string& OutError) {
					if (SelectionPath.empty())
					{
						PendingDefaultLevel.Reset();
						return true;
					}
					FTopLevelAssetPath AssetPath;
					if (const auto PathValidation = FTopLevelAssetPath::TryCreateWithDiagnostic(
							SelectionPath, AssetPath); !PathValidation)
					{
						OutError = ToString(PathValidation.error());
						return false;
					}
					FSoftObjectPath LevelPath;
					const auto Resolution =
						ResolveLevelPackage(
							AssetPath.GetPackagePath(), LevelPath);
					if (!Resolution)
					{
						OutError = Resolution.error().ToString();
						return false;
					}
					if (LevelPath.GetAssetPath() != AssetPath)
					{
						OutError = "The selected Level is not the unique top-level Level in its package.";
						return false;
					}
					PendingDefaultLevel = std::move(LevelPath);
					return true;
				},
				.PathPrefixFilter = Project->MountRoot,
			});
			if (!PickerResult.Error.empty()) OutError = PickerResult.Error;
		}
		else
		{
			ImGui::TextDisabled("No project is currently open.");
		}
		ImGui::Separator();
		const float ButtonWidth = MonaImGui::ScaleUI(86.0f);
		const float ButtonGap = ImGui::GetStyle().ItemSpacing.x;
		ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - ButtonWidth * 2.0f - ButtonGap);
		if (ImGui::Button("Revert", ImVec2(ButtonWidth, 0.0f))) { PendingDefaultLevel = DefaultLevel; OutError.clear(); }
		ImGui::SameLine();
		const bool bCanApply = Project && PendingDefaultLevel != DefaultLevel;
		if (!bCanApply) ImGui::BeginDisabled();
		if (ImGui::Button("Apply", ImVec2(ButtonWidth, 0.0f)))
		{
			const TSoftObjectPtr<DLevel> PreviousDefaultLevel = DefaultLevel;
			DefaultLevel = PendingDefaultLevel;
			if (!SaveProjectSettings()) { DefaultLevel = PreviousDefaultLevel; OutError = EditorError; }
			else OutError.clear();
		}
		if (!bCanApply) ImGui::EndDisabled();
		ImGui::EndDisabled();
	}

	auto MLevelEditor::SetError(std::string Message) -> void
	{
		EditorError = std::move(Message);
		DURIN_ERROR("Level editor: {}", EditorError);
	}

	auto MLevelEditor::StartPlay(EPlayStartLocation StartLocation, EPlayDestination Destination) -> void
	{
		if (DocumentController && DocumentController->IsOpening()) return;
		if (!Context->EditorHost || !Context || !Context->Level) return;
		if (!RequestDeactivate()) return;
		if (SceneViewportPanel) SceneViewportPanel->SetPreferredPlayMode(StartLocation, Destination);
		FPlayRequest Request;
		Request.SourceLevel = Context->Level;
		Request.StartLocation = StartLocation;
		Request.Destination = Destination;
		Request.bSimulatePhysics = Context->bSimulatePhysics;
		if (StartLocation == EPlayStartLocation::EditorCamera && SceneViewportPanel)
		{
			FLevelViewportCameraState CameraState;
			if (!SceneViewportPanel->CaptureCameraState(Context->Level, CameraState))
			{
				SetError("The editor camera is unavailable.");
				return;
			}
			Request.CameraLocation = CameraState.Location;
			const FReal Pitch = Math::DegreesToRadians(CameraState.Pitch);
			const FReal Yaw = Math::DegreesToRadians(CameraState.Yaw);
			const FVector3 Forward(std::cos(Pitch) * std::cos(Yaw), std::cos(Pitch) * std::sin(Yaw), std::sin(Pitch));
			Request.CameraTarget = Request.CameraLocation + Forward;
		}
		std::string Error;
		if (!Context->EditorHost->StartPlaySession(Request, &Error)) SetError(std::move(Error));
	}

	auto MLevelEditor::ApplyPlayChanges(bool bSelectedOnly) -> void
	{
		if (!Context->EditorHost || !Context) return;
		std::vector<AActor*> Actors;
		if (bSelectedOnly)
		{
			for (const TObjectPtr<AActor>& Actor : Context->GetSelectedActors()) if (Actor) Actors.push_back(Actor.Get());
		}
		uint32 AppliedCount = 0;
		std::string Error;
		if (!Context->EditorHost->ApplyPlaySessionChanges(Actors, &AppliedCount, &Error))
		{
			SetError(std::move(Error));
			return;
		}
		DURIN_INFO("Applied runtime changes from {} actor(s) to the editor level.", AppliedCount);
	}

	auto MLevelEditor::BuildDefaultLayout(uint32 DockSpaceId, float DockSpaceWidth, float DockSpaceHeight) -> void
	{
		ImGui::DockBuilderRemoveNode(DockSpaceId);
		ImGui::DockBuilderAddNode(DockSpaceId, ImGuiDockNodeFlags_DockSpace);
		ImGui::DockBuilderSetNodeSize(DockSpaceId, ImVec2(DockSpaceWidth, DockSpaceHeight));
		if (ImGuiDockNode* DockSpaceNode = ImGui::DockBuilderGetNode(DockSpaceId))
			DockSpaceNode->WindowClass = TabLayout::MakeWindowClass(Workspace::Type);

		ImGuiID MainDockId = DockSpaceId;
		ImGuiID DetailsDockId = ImGui::DockBuilderSplitNode(
			MainDockId, ImGuiDir_Right, 0.24f, nullptr, &MainDockId);
		const ImGuiID WorldOutlinerDockId = ImGui::DockBuilderSplitNode(
			DetailsDockId, ImGuiDir_Up, 0.35f, nullptr, &DetailsDockId);

		const std::string WorldOutlinerName = TabLayout::MakePanelWindowName("World Outliner", Workspace::Type, "WorldOutliner");
		const std::string DetailsName = TabLayout::MakePanelWindowName("Details", Workspace::Type, "Details");
		const std::string SceneViewportName = TabLayout::MakePanelWindowName("Scene Viewport", Workspace::Type, "SceneViewport");
		ImGui::DockBuilderDockWindow(WorldOutlinerName.c_str(), WorldOutlinerDockId);
		ImGui::DockBuilderDockWindow(DetailsName.c_str(), DetailsDockId);
		ImGui::DockBuilderDockWindow(SceneViewportName.c_str(), MainDockId);
		ImGui::DockBuilderFinish(DockSpaceId);
	}
} // namespace Durin::LevelEditor
