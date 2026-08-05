#include "Widgets/MLevelEditor.h"

#include "AssetSystem.h"
#include "Editor/EditorAssetPicker.h"
#include "Editor/EditorEngine.h"
#include "Editor/EditorNotification.h"
#include "Editor/EditorTransaction.h"
#include "Editor/EditorWorkspaceUI.h"
#include "Settings/LevelEditorSessionSettings.h"
#include "Assets/EditorAssetMoveCoordinator.h"
#include "Engine/Engine.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Documents/LevelDocumentController.h"
#include "Documents/LevelDocumentRevisionState.h"
#include "Workspace/LevelEditorContext.h"
#include "Workspace/LevelEditorWorkspace.h"
#include "Misc/Project.h"
#include "Math/Operations.h"
#include "MonaImGui.h"
#include "Panels/ConsolePanel.h"
#include "Panels/DetailsPanel.h"
#include "Panels/ContentBrowserPanel.h"
#include "Panels/LevelEditorPanel.h"
#include "Panels/SceneViewportPanel.h"
#include "Panels/WorldOutlinerPanel.h"
#include "Assets/SceneImportDialog.h"
#include "Assets/StaticMeshImportDialog.h"
#include "Assets/TextureImportDialog.h"
#include "Assets/TextureCubeImportDialog.h"
#include "Widgets/EditorNotificationOverlay.h"
#include "Yaml/Yaml.h"

namespace Durin
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
	MLevelEditor::MLevelEditor(FLevelEditorSessionSettings& InSessionSettings, FEditorWorkspaceManager& InWorkspaceManager)
		: SessionSettings(InSessionSettings)
		, WorkspaceManager(InWorkspaceManager)
	{
	}

	MLevelEditor::~MLevelEditor()
	{
		RequestDeactivate();
		if (Context && SceneViewportPanel)
		{
			SessionSettings.CaptureViewportState(*Context, *SceneViewportPanel);
			SessionSettings.Save(SceneViewportPanel);
		}
	}

	auto MLevelEditor::Construct() -> void
	{
		InitializeContext();
		InitializeSession();
		CreatePanels();
		CreateDocumentServices();
		CreateImportDialogs();
		CreateContentBrowser();
		CreateNotificationOverlay();
		FinalizeConstruction();
	}

	auto MLevelEditor::InitializeContext() -> void
	{
		Context = std::make_unique<FLevelEditorContext>();
		Context->ReportError = [this](std::string Message) { SetError(std::move(Message)); };
		Context->RenameLevel = [this](std::string_view NewName) {
			return DocumentController && DocumentController->RenameCurrentLevel(NewName);
		};
		Context->StartPlay = [this](EEditorPlayStartLocation StartLocation, EEditorPlayDestination Destination) {
			StartPlay(StartLocation, Destination);
		};
		Context->ApplyPlayChanges = [this](bool bSelectedOnly) { ApplyPlayChanges(bSelectedOnly); };
	}

	auto MLevelEditor::InitializeSession() -> void
	{
		Asset::GetAssetRegistry().ScanMountedContent(Asset::EAssetRegistryScanMode::Incremental);
		SessionSettings.PruneInvalidViewportStates();
		LoadProjectSettings();
		SessionSettings.Save(nullptr);
	}

	auto MLevelEditor::CreatePanels() -> void
	{
		auto SceneViewport = std::make_unique<FSceneViewportPanel>();
		SceneViewportPanel = SceneViewport.get();
		Context->FocusActor = [this](AActor* Actor) {
			if (SceneViewportPanel) SceneViewportPanel->FocusActor(Actor);
		};
		SessionSettings.ApplyTo(*SceneViewportPanel);
		Panels.emplace_back(std::move(SceneViewport));
		Panels.emplace_back(std::make_unique<FWorldOutlinerPanel>());
		auto Details = std::make_unique<FDetailsPanel>(SessionSettings);
		DetailsPanel = Details.get();
		Panels.emplace_back(std::move(Details));
		Panels.emplace_back(std::make_unique<FConsolePanel>());
	}

	auto MLevelEditor::CreateDocumentServices() -> void
	{
		AssetMoveCoordinator = std::make_unique<FEditorAssetMoveCoordinator>(
			*Context,
			SessionSettings,
			*SceneViewportPanel,
			DefaultLevel,
			[this] { return SaveProjectSettings(); }
		);

		DocumentController = std::make_unique<FLevelDocumentController>(
			*Context,
			SessionSettings,
			*SceneViewportPanel,
			*AssetMoveCoordinator,
			DefaultLevel,
			[this] { EditorError.clear(); },
			[this](std::string Message) { SetError(std::move(Message)); },
			[this](bool bSucceeded) {
				if (!DeferredOpenDocumentId.IsValid()) return;
				const FEditorDocumentId CompletedId = std::exchange(DeferredOpenDocumentId, {});
				if (!WorkspaceManager.CompleteDeferredDocumentOpen(CompletedId, bSucceeded))
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
			.Imported = [this](std::string AssetPath) {
				Asset::GetAssetRegistry().ScanMountedContent(Asset::EAssetRegistryScanMode::Incremental);
				if (ContentBrowserPanel) ContentBrowserPanel->RevealAsset(AssetPath);
			},
			.ImportedDirectory = [this](std::string DirectoryPath) {
				Asset::GetAssetRegistry().ScanMountedContent(
					Asset::EAssetRegistryScanMode::Incremental);
				if (ContentBrowserPanel)
					ContentBrowserPanel->RevealDirectory(DirectoryPath);
			},
		};
		SceneImportDialog =
			MakeImportDialog<FSceneImportDialog>(ImportCallbacks);
		StaticMeshImportDialog =
			MakeImportDialog<FStaticMeshImportDialog>(ImportCallbacks);
		TextureImportDialog =
			MakeImportDialog<FTextureImportDialog>(ImportCallbacks);
		TextureCubeImportDialog =
			MakeImportDialog<FTextureCubeImportDialog>(ImportCallbacks);
	}

	auto MLevelEditor::CreateContentBrowser() -> void
	{
		auto ContentBrowser = std::make_unique<FContentBrowserPanel>(
			SessionSettings,
			[this](const std::string& Path, const std::string& AssetClassName) {
				return WorkspaceManager.OpenAsset(Path, AssetClassName);
			},
			[this](const std::string& DestinationDirectory, EContentBrowserImportType ImportType) {
				if (ImportType == EContentBrowserImportType::Texture)
				{
					if (TextureImportDialog) TextureImportDialog->Open(DestinationDirectory);
				}
				else if (ImportType == EContentBrowserImportType::TextureCube)
				{
					if (TextureCubeImportDialog) TextureCubeImportDialog->Open(DestinationDirectory);
				}
				else if (ImportType == EContentBrowserImportType::StaticMesh)
				{
					if (StaticMeshImportDialog) StaticMeshImportDialog->Open(DestinationDirectory);
				}
				else if (SceneImportDialog) SceneImportDialog->Open(DestinationDirectory);
			},
			[this](std::span<const FEditorAssetMove> Moves) {
				return AssetMoveCoordinator->MoveAssets(Moves);
			},
			[](std::unique_ptr<IEditorTransaction> Transaction) {
				return GEditor
					&& GEditor->GetTransactionManager().Execute(
						std::move(Transaction));
			},
			[] {
				return GEditor
					? GEditor->GetTransactionManager()
						.GetContentMutationRevision()
					: uint64{0};
			}
		);
		ContentBrowserPanel = ContentBrowser.get();
		Panels.emplace_back(std::move(ContentBrowser));
	}

	auto MLevelEditor::RevealAssetInContentBrowser(const FAssetPath& AssetPath) -> bool
	{
		if (!ContentBrowserPanel || !AssetPath.IsValid()) return false;
		ContentBrowserPanel->RevealAsset(AssetPath.ToString());
		return true;
	}

	auto MLevelEditor::CreateNotificationOverlay() -> void
	{
		auto ActivityHistory = std::make_unique<FEditorNotificationOverlay>();
		NotificationOverlay = ActivityHistory.get();
		Panels.emplace_back(std::move(ActivityHistory));
	}

	auto MLevelEditor::FinalizeConstruction() -> void
	{
		Context->Synchronize(GEditor != nullptr ? GEditor->GetEditorWorld() : (GEngine != nullptr ? GEngine->GetWorld() : nullptr));
		DocumentController->OpenDefaultLevel();
		if (!EditorError.empty())
		{
			DURIN_WARN("Could not open project default level {}: {}", DefaultLevel, EditorError);
			EditorError.clear();
		}
	}

	auto MLevelEditor::LoadProjectSettings() -> bool
	{
		DefaultLevel.clear();
		const FProjectInfo* Project = GetCurrentProject();
		if (!Project) return false;
		const std::string File = Project->ProjectDir + "Configs/Project.yaml";
		if (!std::filesystem::exists(File)) return true;
		FYamlDocument Document;
		FYamlParseError Error;
		if (!Document.LoadFromFile(File, &Error))
		{
			DURIN_WARN("Failed to load project settings: {}", Error.Message);
			return false;
		}
		DefaultLevel = Document.GetRootView().GetView("Editor").GetView("DefaultLevel").GetString();
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
		if (!DefaultLevel.empty() && !DefaultLevel.starts_with(Project->MountRoot))
		{
			SetError("The default level must belong to the current project.");
			return false;
		}
		const auto Found = std::ranges::find_if(Asset::GetAssetRegistry().GetAssets(), [this](const auto& Entry) {
			return Entry.first.ToString() == DefaultLevel && Entry.second.AssetClassName == DLevel::StaticClass()->GetQualifiedName().ToString();
		});
		if (!DefaultLevel.empty() && Found == Asset::GetAssetRegistry().GetAssets().end())
		{
			SetError("The default level is not a registered Level asset.");
			return false;
		}
		std::error_code Error;
		std::filesystem::create_directories(std::filesystem::path(Project->ProjectDir) / "Configs", Error);
		if (Error)
		{
			SetError("Could not create the project Configs directory.");
			return false;
		}
		FYamlDocument Document;
		const std::string SettingsFile = Project->ProjectDir + "Configs/Project.yaml";
		if (std::filesystem::exists(SettingsFile))
		{
			FYamlParseError ParseError;
			if (!Document.LoadFromFile(SettingsFile, &ParseError))
			{
				SetError(std::format("Could not load existing project settings: {}", ParseError.Message));
				return false;
			}
			if (!Document.GetRootView().IsMap())
			{
				SetError("Project settings must contain a YAML map at the root.");
				return false;
			}
		}
		FYamlNodeRef Root = Document.GetMutableRoot();
		Root.EnsureMap();
		FYamlNodeRef Editor = Root.GetRef("Editor");
		if (!Editor.IsValid()) Editor = Root.AddMap("Editor");
		else if (!Editor.IsMap())
		{
			SetError("The Editor project setting must be a YAML map.");
			return false;
		}
		Editor.SetChildValue("DefaultLevel", DefaultLevel);
		if (!Document.SaveToFile(SettingsFile))
		{
			SetError("Could not save project settings.");
			return false;
		}
		return true;
	}

	auto MLevelEditor::GetWorkspaceType() const -> const FEditorWorkspaceTypeId&
	{
		return LevelEditorWorkspace::Type;
	}

	auto MLevelEditor::OpenDocument(const FEditorDocumentTab& Document) -> EEditorDocumentOpenResult
	{
		if (Document.ResourceId.empty()) return EEditorDocumentOpenResult::Opened;
		if (!DocumentController) return EEditorDocumentOpenResult::Rejected;
		switch (DocumentController->RequestOpenLevel(Document.ResourceId))
		{
		case ELevelDocumentOpenResult::Opened: return EEditorDocumentOpenResult::Opened;
		case ELevelDocumentOpenResult::Deferred:
			DeferredOpenDocumentId = Document.Id;
			return EEditorDocumentOpenResult::Deferred;
		case ELevelDocumentOpenResult::Rejected: return EEditorDocumentOpenResult::Rejected;
		}
		return EEditorDocumentOpenResult::Rejected;
	}

	auto MLevelEditor::ActivateDocument(const FEditorDocumentTab& Document) -> void
	{
		(void)Document;
		RootWindow.RequestFocus();
	}

	auto MLevelEditor::RequestDeactivate() -> bool
	{
		return !Context || !DetailsPanel || DetailsPanel->RequestDeactivate(*Context);
	}

	auto MLevelEditor::RequestCloseDocument(const FEditorDocumentTab& Document) -> EEditorDocumentCloseResult
	{
		if (!RequestDeactivate()) return EEditorDocumentCloseResult::Rejected;
		if (IsDocumentDirty(Document)) return EEditorDocumentCloseResult::PendingConfirmation;
		return EEditorDocumentCloseResult::Closed;
	}

	auto MLevelEditor::SaveDocument(const FEditorDocumentTab& Document) -> bool
	{
		(void)Document;
		return SaveActiveDocument();
	}

	auto MLevelEditor::DiscardDocument(const FEditorDocumentTab& Document) -> bool
	{
		(void)Document;
		if (!Context || !Context->Level || !Context->Level->GetPackage()) return false;
		DPackage* Package = Context->Level->GetPackage();
		FLevelDocumentRevisionState::Discard(
			GEditor ? &GEditor->GetTransactionManager() : nullptr, *Package
		);
		return true;
	}

	auto MLevelEditor::IsDocumentDirty(const FEditorDocumentTab& Document) const -> bool
	{
		(void)Document;
		return Context && Context->Level && Context->Level->GetPackage() && Context->Level->GetPackage()->IsDirty();
	}

	auto MLevelEditor::ResetLayout() -> void
	{
		bResetLayoutRequested = true;
		bSelectDefaultBottomPanelRequested = true;
	}

	auto MLevelEditor::DrawWorkspace(bool bActive) -> bool
	{
		if (!Context || !DocumentController || !SceneImportDialog || !StaticMeshImportDialog
			|| !TextureImportDialog) return false;
		if (bActive && !bWasActive)
		{
			// Internal panel windows are not submitted while another workspace is visible, so
			// restore the asset-oriented default before the last submitted tab wins selection.
			bSelectDefaultBottomPanelRequested = true;
		}
		bWasActive = bActive;
		const bool bDocumentOpen = std::ranges::any_of(WorkspaceManager.GetDocuments(), [](const FEditorDocumentTab& Document) {
			return Document.WorkspaceType == LevelEditorWorkspace::Type;
		});
		if (!bDocumentOpen)
		{
			RootWindow.ResetActivationState();
			return false;
		}

		const ImGuiID DockSpaceId = EditorWorkspaceUI::MakeDockSpaceId(LevelEditorWorkspace::Type, LevelEditorWorkspace::LayoutVersion);
		const bool bLevelDirty = Context->Level && Context->Level->GetPackage() && Context->Level->GetPackage()->IsDirty();
		const FEditorWorkspaceRootWindowState RootWindowState = RootWindow.Begin({
			.DisplayName = "Level Editor",
			.RootKey = LevelEditorWorkspace::RootKey,
			.bDirty = bLevelDirty,
			.bZeroPadding = true,
			.InternalDockSpace = FEditorWorkspaceInternalDockSpace{
				.WorkspaceType = LevelEditorWorkspace::Type,
				.LayoutVersion = LevelEditorWorkspace::LayoutVersion,
			},
		});
		if (!RootWindowState.bVisible)
		{
			RootWindow.End();
			if (RootWindowState.bCloseRequested)
			{
				const FEditorDocumentTab* ActiveDocument = WorkspaceManager.GetActiveDocument();
				if (ActiveDocument && ActiveDocument->WorkspaceType == LevelEditorWorkspace::Type)
					WorkspaceManager.RequestCloseDocument(ActiveDocument->Id);
			}
			return RootWindowState.bActivated;
		}

		bool bPlaying = GEditor && GEditor->IsPlaying();
		Context->bReadOnly = bPlaying;
		Context->Synchronize(GEditor != nullptr
			? (bPlaying ? GEditor->GetPlayWorld() : GEditor->GetEditorWorld())
			: (GEngine != nullptr ? GEngine->GetWorld() : nullptr));
		const ImGuiIO& IO = ImGui::GetIO();
		if (bActive || RootWindowState.bFocused)
		{
			if (!IO.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F5, false) && GEditor)
			{
				if (GEditor->IsPlaying()) GEditor->StopPlaySession();
				else
				{
					StartPlay(EEditorPlayStartLocation::LevelStart, IO.KeyCtrl ? EEditorPlayDestination::NewWindow : EEditorPlayDestination::EmbeddedViewport);
				}
			}
			if (!IO.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F6, false) && GEditor && GEditor->IsPlaying()) GEditor->SetPlaySessionPaused(!GEditor->IsPlaySessionPaused());
			if (!IO.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F7, false) && GEditor) GEditor->StepPlaySession();
		}
		if (GEditor && bPlaying != GEditor->IsPlaying())
		{
			bPlaying = GEditor->IsPlaying();
			Context->bReadOnly = bPlaying;
			Context->Synchronize(bPlaying ? GEditor->GetPlayWorld() : GEditor->GetEditorWorld());
		}

		const ImVec2 DockSpaceSize = ImGui::GetContentRegionAvail();
		const bool bNeedsDefaultLayout = ImGui::DockBuilderGetNode(DockSpaceId) == nullptr;
		if (bNeedsDefaultLayout || bResetLayoutRequested)
		{
			// DockBuilder must finish before DockSpace submission so the new tree retains this frame's host window.
			BuildDefaultLayout(DockSpaceId, DockSpaceSize.x, DockSpaceSize.y);
			bResetLayoutRequested = false;
		}
		EditorWorkspaceUI::SubmitDockSpace(LevelEditorWorkspace::Type, LevelEditorWorkspace::LayoutVersion, DockSpaceSize);
		RootWindow.End();
		if (RootWindowState.bCloseRequested)
		{
			const FEditorDocumentTab* ActiveDocument = WorkspaceManager.GetActiveDocument();
			if (ActiveDocument && ActiveDocument->WorkspaceType == LevelEditorWorkspace::Type)
				WorkspaceManager.RequestCloseDocument(ActiveDocument->Id);
		}

		DocumentController->DrawDialogs();
		SceneImportDialog->Draw();
		StaticMeshImportDialog->Draw();
		TextureImportDialog->Draw();
		TextureCubeImportDialog->Draw();
		DrawProjectSettings();

		if (!EditorError.empty()) ImGui::OpenPopup("Editor Error");
		if (ImGui::BeginPopupModal("Editor Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings))
		{
			ImGui::TextWrapped("%s", EditorError.c_str());
			if (ImGui::Button("OK"))
			{
				EditorError.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
		for (const std::unique_ptr<ILevelEditorPanel>& Panel : Panels)
		{
			if (!Panel->IsOpen())
			{
				Panel->TickWhenHidden();
				continue;
			}
			const bool bDisablePanel = bPlaying && Panel.get() == ContentBrowserPanel;
			if (bDisablePanel) ImGui::BeginDisabled();
			Panel->Draw(*Context);
			if (bDisablePanel) ImGui::EndDisabled();
		}
		if (SceneViewportPanel) SceneViewportPanel->FinalizeViewportFrame(*Context);
		if (bSelectDefaultBottomPanelRequested)
		{
			// Docking tabs do not exist until every panel has submitted its window this frame.
			const std::string ContentBrowserName = EditorWorkspaceUI::MakePanelWindowName("Content Browser", LevelEditorWorkspace::Type, "ContentBrowser");
			ImGui::SetWindowFocus(ContentBrowserName.c_str());
			bSelectDefaultBottomPanelRequested = false;
		}

		if (NotificationOverlay && GEditor)
		{
			NotificationOverlay->DrawNotifications(GEditor->GetNotificationManager(), GEditor->GetTransactionManager());
		}

		return RootWindowState.bFocused || RootWindowState.bActivated;
	}

	auto MLevelEditor::CanSaveActiveDocument() const -> bool
	{
		return !(GEditor && GEditor->IsPlaying()) && Context && Context->Level && Context->Level->GetPackage();
	}

	auto MLevelEditor::SaveActiveDocument() -> bool
	{
		return CanSaveActiveDocument() && DocumentController->SaveCurrentLevel();
	}

	auto MLevelEditor::CanUndo() const -> bool
	{
		const bool bDragging = SceneViewportPanel && SceneViewportPanel->GetTransformGizmo() && SceneViewportPanel->GetTransformGizmo()->IsDragging();
		return !(GEditor && GEditor->IsPlaying()) && !bDragging && GEditor && GEditor->GetTransactionManager().CanUndo();
	}

	auto MLevelEditor::CanRedo() const -> bool
	{
		const bool bDragging = SceneViewportPanel && SceneViewportPanel->GetTransformGizmo() && SceneViewportPanel->GetTransformGizmo()->IsDragging();
		return !(GEditor && GEditor->IsPlaying()) && !bDragging && GEditor && GEditor->GetTransactionManager().CanRedo();
	}

	auto MLevelEditor::GetUndoDescription() const -> std::string_view
	{
		return CanUndo() ? GEditor->GetTransactionManager().GetUndoDescription() : std::string_view{};
	}

	auto MLevelEditor::GetRedoDescription() const -> std::string_view
	{
		return CanRedo() ? GEditor->GetTransactionManager().GetRedoDescription() : std::string_view{};
	}

	auto MLevelEditor::Undo() -> bool
	{
		return CanUndo() && GEditor->GetTransactionManager().Undo();
	}

	auto MLevelEditor::Redo() -> bool
	{
		return CanRedo() && GEditor->GetTransactionManager().Redo();
	}

	auto MLevelEditor::DrawFileMenu() -> void
	{
		const bool bPlaying = GEditor && GEditor->IsPlaying();
		if (bPlaying) ImGui::BeginDisabled();
		if (ImGui::MenuItem("Open Project...")) DocumentController->RequestAction(ELevelDocumentAction::OpenProject);
		if (bPlaying) ImGui::EndDisabled();
	}

	auto MLevelEditor::DrawEditMenu() -> void
	{
		const bool bPlaying = GEditor && GEditor->IsPlaying();
		if (bPlaying) ImGui::BeginDisabled();
		if (ImGui::MenuItem("Project Settings..."))
		{
			PendingDefaultLevel = DefaultLevel;
			bProjectSettingsOpen = true;
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
					if (bPanelOpen || Panel.get() != DetailsPanel || RequestDeactivate()) Panel->SetOpen(bPanelOpen);
				}
			}
			ImGui::EndMenu();
		}
	}

	auto MLevelEditor::DrawProjectSettings() -> void
	{
		if (!bProjectSettingsOpen) return;
		const float DialogWidth = MonaImGui::ScaleUI(620.0f);
		const float DialogHeight = MonaImGui::ScaleUI(390.0f);
		ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(DialogWidth, DialogHeight), ImGuiCond_Appearing);
		ImGui::SetNextWindowSizeConstraints(ImVec2(MonaImGui::ScaleUI(520.0f), MonaImGui::ScaleUI(300.0f)), ImVec2(MonaImGui::ScaleUI(900.0f), MonaImGui::ScaleUI(650.0f)));
		if (ImGui::Begin("Project Settings###Durin.LevelEditor.ProjectSettings", &bProjectSettingsOpen, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings))
		{
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
				const FEditorAssetPickerResult PickerResult = EditorAssetPicker::Draw({
					.ComboId = "##DefaultLevel",
					.SearchId = "##DefaultLevelSearch",
					.SearchHint = "Search levels...",
					.RequiredClass = DLevel::StaticClass(),
					.ClassPolicy = EEditorAssetClassPolicy::Exact,
					.CurrentSelectionPath = PendingDefaultLevel,
					.SearchText = LevelSearchText,
					.bAllowNone = true,
					.NoneLabel = "None",
					.AssignSelection = [this](DObject* Selection, std::string& OutError) {
						if (Selection)
						{
							if (!Selection->GetPackage())
							{
								OutError = "The selected level is not in a package.";
								return false;
							}
							PendingDefaultLevel = Selection->GetPackage()->GetPackagePath();
						}
						else PendingDefaultLevel.clear();
						return true;
					},
					.PathPrefixFilter = Project->MountRoot,
				});
				if (!PickerResult.Error.empty()) SetError(PickerResult.Error);
			}
			else
			{
				ImGui::TextDisabled("No project is currently open.");
			}
			ImGui::Separator();
			const float ButtonWidth = MonaImGui::ScaleUI(86.0f);
			const float ButtonGap = ImGui::GetStyle().ItemSpacing.x;
			ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - ButtonWidth * 2.0f - ButtonGap);
			if (ImGui::Button("Cancel", ImVec2(ButtonWidth, 0.0f))) bProjectSettingsOpen = false;
			ImGui::SameLine();
			const bool bCanApply = Project && PendingDefaultLevel != DefaultLevel;
			if (!bCanApply) ImGui::BeginDisabled();
			if (ImGui::Button("Apply", ImVec2(ButtonWidth, 0.0f)))
			{
				const std::string PreviousDefaultLevel = DefaultLevel;
				DefaultLevel = PendingDefaultLevel;
				if (!SaveProjectSettings()) DefaultLevel = PreviousDefaultLevel;
			}
			if (!bCanApply) ImGui::EndDisabled();
		}
		ImGui::End();
	}

	auto MLevelEditor::SetError(std::string Message) -> void
	{
		EditorError = std::move(Message);
		DURIN_ERROR("Level editor: {}", EditorError);
	}

	auto MLevelEditor::StartPlay(EEditorPlayStartLocation StartLocation, EEditorPlayDestination Destination) -> void
	{
		if (!GEditor || !Context || !Context->Level) return;
		if (!RequestDeactivate()) return;
		if (SceneViewportPanel) SceneViewportPanel->SetPreferredPlayMode(StartLocation, Destination);
		FEditorPlayRequest Request;
		Request.SourceLevel = Context->Level;
		Request.StartLocation = StartLocation;
		Request.Destination = Destination;
		Request.bSimulatePhysics = Context->bSimulatePhysics;
		if (StartLocation == EEditorPlayStartLocation::EditorCamera && SceneViewportPanel)
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
		if (!GEditor->StartPlaySession(Request, &Error)) SetError(std::move(Error));
	}

	auto MLevelEditor::ApplyPlayChanges(bool bSelectedOnly) -> void
	{
		if (!GEditor || !Context) return;
		std::vector<AActor*> Actors;
		if (bSelectedOnly)
		{
			for (const TObjectPtr<AActor>& Actor : Context->GetSelectedActors()) if (Actor) Actors.push_back(Actor.Get());
		}
		uint32 AppliedCount = 0;
		std::string Error;
		if (!GEditor->ApplyPlaySessionChanges(Actors, &AppliedCount, &Error))
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
			DockSpaceNode->WindowClass = EditorWorkspaceUI::MakeWindowClass(LevelEditorWorkspace::Type);

		ImGuiID MainDockId = DockSpaceId;
		const ImGuiID LeftDockId = ImGui::DockBuilderSplitNode(MainDockId, ImGuiDir_Left, 0.20f, nullptr, &MainDockId);
		const ImGuiID RightDockId = ImGui::DockBuilderSplitNode(MainDockId, ImGuiDir_Right, 0.25f, nullptr, &MainDockId);
		const ImGuiID BottomDockId = ImGui::DockBuilderSplitNode(MainDockId, ImGuiDir_Down, 0.25f, nullptr, &MainDockId);

		const std::string WorldOutlinerName = EditorWorkspaceUI::MakePanelWindowName("World Outliner", LevelEditorWorkspace::Type, "WorldOutliner");
		const std::string DetailsName = EditorWorkspaceUI::MakePanelWindowName("Details", LevelEditorWorkspace::Type, "Details");
		const std::string ContentBrowserName = EditorWorkspaceUI::MakePanelWindowName("Content Browser", LevelEditorWorkspace::Type, "ContentBrowser");
		const std::string ConsoleName = EditorWorkspaceUI::MakePanelWindowName("Console", LevelEditorWorkspace::Type, "OutputLog");
		const std::string ActivityHistoryName = EditorWorkspaceUI::MakePanelWindowName("Activity History", LevelEditorWorkspace::Type, "ActivityHistory");
		const std::string SceneViewportName = EditorWorkspaceUI::MakePanelWindowName("Scene Viewport", LevelEditorWorkspace::Type, "SceneViewport");
		ImGui::DockBuilderDockWindow(WorldOutlinerName.c_str(), LeftDockId);
		ImGui::DockBuilderDockWindow(DetailsName.c_str(), RightDockId);
		ImGui::DockBuilderDockWindow(ActivityHistoryName.c_str(), BottomDockId);
		ImGui::DockBuilderDockWindow(ContentBrowserName.c_str(), BottomDockId);
		ImGui::DockBuilderDockWindow(ConsoleName.c_str(), BottomDockId);
		ImGui::DockBuilderDockWindow(SceneViewportName.c_str(), MainDockId);
		ImGui::DockBuilderFinish(DockSpaceId);
	}
} // namespace Durin
