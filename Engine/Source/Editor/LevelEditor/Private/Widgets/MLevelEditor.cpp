#include "Widgets/MLevelEditor.h"

#include "Application/MonaApplication.h"
#include "AssetSystem.h"
#include "Editor/EditorEngine.h"
#include "Editor/EditorNotification.h"
#include "Editor/EditorTransaction.h"
#include "Editor/EditorWorkspaceUI.h"
#include "EditorSessionSettings.h"
#include "EditorAssetMoveCoordinator.h"
#include "Engine/Engine.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "IRendererModule.h"
#include "LevelDocumentController.h"
#include "LevelEditorContext.h"
#include "LevelEditorWorkspace.h"
#include "Misc/Project.h"
#include "MonaImGui.h"
#include "Panels/ConsolePanel.h"
#include "Panels/DetailsPanel.h"
#include "Panels/ContentBrowserPanel.h"
#include "Panels/LevelEditorPanel.h"
#include "Panels/SceneViewportPanel.h"
#include "Panels/WorldOutlinerPanel.h"
#include "StaticMeshImportDialog.h"
#include "Widgets/EditorNotificationOverlay.h"
#include "Widgets/MWindow.h"
#include "Yaml/Yaml.h"

namespace Durin
{
	// MLevelEditor is the composition root for the editor-specific controllers.
	MLevelEditor::MLevelEditor(FEditorSessionSettings& InSessionSettings, FEditorWorkspaceManager& InWorkspaceManager)
		: SessionSettings(InSessionSettings)
		, WorkspaceManager(InWorkspaceManager)
	{
	}

	MLevelEditor::~MLevelEditor()
	{
		if (Context && SceneViewportPanel)
		{
			SessionSettings.CaptureViewportState(*Context, *SceneViewportPanel);
			SessionSettings.Save(SceneViewportPanel);
		}
	}

	auto MLevelEditor::Construct() -> void
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

		Asset::GetAssetRegistry().ScanMountedContent();
		SessionSettings.PruneInvalidViewportStates();
		LoadProjectSettings();
		SessionSettings.Save(nullptr);

		auto SceneViewport = std::make_unique<FSceneViewportPanel>();
		SceneViewportPanel = SceneViewport.get();
		SessionSettings.ApplyTo(*SceneViewportPanel);
		Panels.emplace_back(std::move(SceneViewport));
		Panels.emplace_back(std::make_unique<FWorldOutlinerPanel>());
		Panels.emplace_back(std::make_unique<FDetailsPanel>(SessionSettings));
		Panels.emplace_back(std::make_unique<FConsolePanel>());
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
			[this](std::string Message) { SetError(std::move(Message)); }
		);
		StaticMeshImportDialog = std::make_unique<FStaticMeshImportDialog>(
			[this] { EditorError.clear(); },
			[this](std::string Message) { SetError(std::move(Message)); },
			[this](std::string AssetPath) {
				Asset::GetAssetRegistry().ScanMountedContent();
				if (ContentBrowserPanel) ContentBrowserPanel->RevealAsset(AssetPath);
			}
		);
		auto ContentBrowser = std::make_unique<FContentBrowserPanel>(
			SessionSettings,
			[this](const std::string& Path, const std::string& AssetClassName) {
				return WorkspaceManager.OpenAsset(Path, AssetClassName);
			},
			[this](const std::string& DestinationDirectory) {
				if (StaticMeshImportDialog) StaticMeshImportDialog->Open(DestinationDirectory);
			},
			[this](std::span<const FEditorAssetMove> Moves) {
				return AssetMoveCoordinator->MoveAssets(Moves);
			}
		);
		ContentBrowserPanel = ContentBrowser.get();
		Panels.emplace_back(std::move(ContentBrowser));
		auto ActivityHistory = std::make_unique<FEditorNotificationOverlay>();
		NotificationOverlay = ActivityHistory.get();
		Panels.emplace_back(std::move(ActivityHistory));

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

	auto MLevelEditor::OpenDocument(const FEditorDocumentTab& Document) -> bool
	{
		if (Document.ResourceId.empty()) return true;
		return DocumentController && DocumentController->RequestOpenLevel(Document.ResourceId);
	}

	auto MLevelEditor::ActivateDocument(const FEditorDocumentTab& Document) -> void
	{
		(void)Document;
		bFocusRequested = true;
	}

	auto MLevelEditor::RequestCloseDocument(const FEditorDocumentTab& Document) -> bool
	{
		(void)Document;
		return false;
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
		if (!Context || !DocumentController || !StaticMeshImportDialog) return false;

		const ImGuiID DockSpaceId = EditorWorkspaceUI::MakeDockSpaceId(LevelEditorWorkspace::Type, LevelEditorWorkspace::LayoutVersion);
		EditorWorkspaceUI::SetNextEditorRootWindowClass();
		if (bFocusRequested)
		{
			ImGui::SetNextWindowFocus();
			bFocusRequested = false;
		}
		const bool bLevelDirty = Context->Level && Context->Level->GetPackage() && Context->Level->GetPackage()->IsDirty();
		const std::string RootWindowName = EditorWorkspaceUI::MakeEditorRootWindowName(bLevelDirty ? "Level Editor *" : "Level Editor", LevelEditorWorkspace::RootKey);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		const bool bRootVisible = ImGui::Begin(RootWindowName.c_str(), nullptr, ImGuiWindowFlags_NoCollapse);
		ImGui::PopStyleVar();
		const bool bRootFocused = bRootVisible && ImGui::IsWindowFocused(
			ImGuiFocusedFlags_RootAndChildWindows | ImGuiFocusedFlags_DockHierarchy
		);
		if (!bRootVisible)
		{
			if (ImGui::DockBuilderGetNode(DockSpaceId) != nullptr)
				EditorWorkspaceUI::SubmitDockSpace(LevelEditorWorkspace::Type, LevelEditorWorkspace::LayoutVersion, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_KeepAliveOnly);
			ImGui::End();
			return false;
		}

		bool bPlaying = GEditor && GEditor->IsPlaying();
		Context->bReadOnly = bPlaying;
		Context->Synchronize(GEditor != nullptr
			? (bPlaying ? GEditor->GetPlayWorld() : GEditor->GetEditorWorld())
			: (GEngine != nullptr ? GEngine->GetWorld() : nullptr));
		const ImGuiIO& IO = ImGui::GetIO();
		if (bActive || bRootFocused)
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
			if (!bPlaying && IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_N, false)) DocumentController->RequestAction(ELevelDocumentAction::NewLevel);
			if (!bPlaying && IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) DocumentController->SaveCurrentLevel();
			const bool bGizmoDragging = SceneViewportPanel && SceneViewportPanel->GetTransformGizmo() && SceneViewportPanel->GetTransformGizmo()->IsDragging();
			if (!bPlaying && !bGizmoDragging && !IO.WantTextInput && IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false) && GEditor) GEditor->GetTransactionManager().Undo();
			if (!bPlaying && !bGizmoDragging && !IO.WantTextInput && IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false) && GEditor) GEditor->GetTransactionManager().Redo();
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
		ImGui::End();

		DocumentController->DrawDialogs();
		StaticMeshImportDialog->Draw();
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

		if (const std::shared_ptr<MWindow> Window = Mona::FMonaApplication::Get().GetActiveTopLevelWindow())
		{
			const bool bCurrentMaximized = Window->IsMaximized();
			if (bCurrentMaximized != SessionSettings.IsWindowMaximized())
			{
				SessionSettings.SetWindowMaximized(bCurrentMaximized);
				SessionSettings.Save(SceneViewportPanel);
			}
		}

		for (const std::unique_ptr<ILevelEditorPanel>& Panel : Panels)
		{
			if (!Panel->IsOpen()) continue;
			const bool bDisablePanel = bPlaying && Panel.get() == ContentBrowserPanel;
			if (bDisablePanel) ImGui::BeginDisabled();
			Panel->Draw(*Context);
			if (bDisablePanel) ImGui::EndDisabled();
		}
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
		return bRootFocused;
	}

	auto MLevelEditor::DrawMainMenu() -> void
	{
		const bool bPlaying = GEditor && GEditor->IsPlaying();
		if (ImGui::BeginMenu("File"))
		{
			if (bPlaying) ImGui::BeginDisabled();
			if (ImGui::MenuItem("New Level", "Ctrl+N")) DocumentController->RequestAction(ELevelDocumentAction::NewLevel);
			if (ImGui::MenuItem("Save Level", "Ctrl+S", false, Context && Context->Level && Context->Level->GetPackage())) DocumentController->SaveCurrentLevel();
			if (ImGui::MenuItem("Set Current Level as Project Default", nullptr, false, Context && Context->Level && Context->Level->GetPackage()))
			{
				DefaultLevel = Context->Level->GetPackage()->GetPackagePath();
				SaveProjectSettings();
			}
			if (ImGui::MenuItem("Open Project...")) DocumentController->RequestAction(ELevelDocumentAction::OpenProject);
			ImGui::Separator();
			if (ImGui::BeginMenu("Import"))
			{
				if (ImGui::MenuItem("Static Mesh...")) StaticMeshImportDialog->Open();
				ImGui::EndMenu();
			}
			ImGui::Separator();
			if (Context && Context->Level)
			{
				DPackage* Package = Context->Level->GetPackage();
				const std::string Label = Package ? Package->GetPackagePath() + (Package->IsDirty() ? " *" : "") : "Transient Level";
				ImGui::TextDisabled("%s", Label.c_str());
			}
			if (bPlaying) ImGui::EndDisabled();
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Edit"))
		{
			if (bPlaying) ImGui::BeginDisabled();
			const bool bDragging = SceneViewportPanel && SceneViewportPanel->GetTransformGizmo() && SceneViewportPanel->GetTransformGizmo()->IsDragging();
			FEditorTransactionManager* Transactions = GEditor && !bDragging ? &GEditor->GetTransactionManager() : nullptr;
			const std::string UndoLabel = Transactions && Transactions->CanUndo() ? std::format("Undo {}", Transactions->GetUndoDescription()) : "Undo";
			const std::string RedoLabel = Transactions && Transactions->CanRedo() ? std::format("Redo {}", Transactions->GetRedoDescription()) : "Redo";
			if (ImGui::MenuItem(UndoLabel.c_str(), "Ctrl+Z", false, Transactions && Transactions->CanUndo())) Transactions->Undo();
			if (ImGui::MenuItem(RedoLabel.c_str(), "Ctrl+Y", false, Transactions && Transactions->CanRedo())) Transactions->Redo();
			ImGui::Separator();
			if (ImGui::MenuItem("Project Settings...")) bProjectSettingsOpen = true;
			if (bPlaying) ImGui::EndDisabled();
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Play"))
		{
			if (!bPlaying)
			{
				const bool bCanPlay = Context && Context->Level;
				if (ImGui::MenuItem("Play From Start", "F5", false, bCanPlay)) StartPlay(EEditorPlayStartLocation::LevelStart, EEditorPlayDestination::EmbeddedViewport);
				if (ImGui::MenuItem("Play From Camera", nullptr, false, bCanPlay)) StartPlay(EEditorPlayStartLocation::EditorCamera, EEditorPlayDestination::EmbeddedViewport);
				ImGui::Separator();
				if (ImGui::MenuItem("Play From Start in New Window", "Ctrl+F5", false, bCanPlay)) StartPlay(EEditorPlayStartLocation::LevelStart, EEditorPlayDestination::NewWindow);
				if (ImGui::MenuItem("Play From Camera in New Window", nullptr, false, bCanPlay)) StartPlay(EEditorPlayStartLocation::EditorCamera, EEditorPlayDestination::NewWindow);
				ImGui::Separator();
				ImGui::MenuItem("Simulate Physics", nullptr, Context ? &Context->bSimulatePhysics : nullptr);
			}
			else
			{
				if (ImGui::MenuItem("Stop", "F5")) GEditor->StopPlaySession();
				if (ImGui::MenuItem(GEditor->IsPlaySessionPaused() ? "Resume" : "Pause", "F6")) GEditor->SetPlaySessionPaused(!GEditor->IsPlaySessionPaused());
				if (ImGui::MenuItem("Step", "F7", false, GEditor->IsPlaySessionPaused())) GEditor->StepPlaySession();
				ImGui::Separator();
				if (ImGui::MenuItem("Apply Selected Runtime Changes", nullptr, false, Context && !Context->GetSelectedActors().empty())) ApplyPlayChanges(true);
				if (ImGui::MenuItem("Apply All Runtime Changes")) ApplyPlayChanges(false);
				ImGui::Separator();
				bool bPhysicsEnabled = GEditor->GetPlayWorld() && GEditor->GetPlayWorld()->IsPhysicsSimulationEnabled();
				if (ImGui::MenuItem("Simulate Physics", nullptr, &bPhysicsEnabled) && GEditor->GetPlayWorld()) GEditor->GetPlayWorld()->SetPhysicsSimulationEnabled(bPhysicsEnabled);
			}
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("View"))
		{
			if (ImGui::BeginMenu("Display"))
			{
				if (ImGui::BeginMenu("Color Theme"))
				{
					const MonaImGui::EColorTheme CurrentTheme = MonaImGui::GetColorTheme();
					if (ImGui::MenuItem("Dark", nullptr, CurrentTheme == MonaImGui::EColorTheme::Dark))
					{
						MonaImGui::SetColorTheme(MonaImGui::EColorTheme::Dark);
						SessionSettings.Save(SceneViewportPanel);
					}
					if (ImGui::MenuItem("Light", nullptr, CurrentTheme == MonaImGui::EColorTheme::Light))
					{
						MonaImGui::SetColorTheme(MonaImGui::EColorTheme::Light);
						SessionSettings.Save(SceneViewportPanel);
					}
					ImGui::EndMenu();
				}
				ImGui::Separator();
				ImGui::SeparatorText("UI Scale");
				for (const float Scale : {0.75f, 1.0f, 1.25f, 1.5f, 2.0f})
				{
					const std::string Label = std::format("{}%", static_cast<int32>(Scale * 100.0f));
					if (ImGui::MenuItem(Label.c_str(), nullptr, std::abs(SessionSettings.GetUIScale() - Scale) < 0.01f))
						ApplyDisplaySettings(SessionSettings.GetWindowWidth(), SessionSettings.GetWindowHeight(), Scale);
				}
				ImGui::EndMenu();
			}
			if (GEngine)
			{
				if (IRendererModule* RendererModule = GEngine->GetRendererModule())
				{
					bool bEnableFXAA = RendererModule->IsFXAAEnabled();
					if (ImGui::MenuItem("FXAA", nullptr, &bEnableFXAA)) RendererModule->SetFXAAEnabled(bEnableFXAA);
				}
			}
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Window"))
		{
			if (ImGui::BeginMenu("Level Editor"))
			{
				for (const std::unique_ptr<ILevelEditorPanel>& Panel : Panels)
				{
					bool bOpen = Panel->IsOpen();
					if (ImGui::MenuItem(Panel->GetWindowName(), nullptr, &bOpen)) Panel->SetOpen(bOpen);
				}
				ImGui::Separator();
				if (ImGui::MenuItem("Reset Layout")) ResetLayout();
				ImGui::EndMenu();
			}
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Help"))
		{
			ImGui::MenuItem("Durin Level Editor - early development", nullptr, false, false);
			ImGui::EndMenu();
		}
	}

	auto MLevelEditor::DrawProjectSettings() -> void
	{
		if (!bProjectSettingsOpen) return;
		if (ImGui::Begin("Project Settings###Durin.LevelEditor.ProjectSettings", &bProjectSettingsOpen, ImGuiWindowFlags_NoDocking))
		{
			const FProjectInfo* Project = GetCurrentProject();
			if (Project)
			{
				ImGui::Text("Project: %s", Project->Name.c_str());
				ImGui::TextWrapped("Path: %s", Project->ProjectFile.c_str());
				ImGui::SeparatorText("Editor Default Level");
				if (ImGui::BeginCombo("Default Level", DefaultLevel.empty() ? "None" : DefaultLevel.c_str()))
				{
					if (ImGui::Selectable("None", DefaultLevel.empty())) DefaultLevel.clear();
					for (const auto& [Path, Data] : Asset::GetAssetRegistry().GetAssets())
					{
						const std::string Value = Path.ToString();
						if (!Value.starts_with(Project->MountRoot) || Data.AssetClassName != DLevel::StaticClass()->GetQualifiedName().ToString()) continue;
						if (ImGui::Selectable(Value.c_str(), Value == DefaultLevel)) DefaultLevel = Value;
					}
					ImGui::EndCombo();
				}
				if (ImGui::Button("Save")) SaveProjectSettings();
			}
		}
		ImGui::End();
	}

	auto MLevelEditor::ApplyDisplaySettings(int32 Width, int32 Height, float Scale) -> void
	{
		SessionSettings.SetDisplaySettings(Width, Height, Scale);
		MonaImGui::SetGlobalUIScale(Scale);
		if (const std::shared_ptr<MWindow> Window = Mona::FMonaApplication::Get().GetActiveTopLevelWindow())
		{
			if (!Window->IsMaximized()) Window->ResizeWindow({static_cast<float>(Width), static_cast<float>(Height)});
		}
		SessionSettings.Save(SceneViewportPanel);
	}

	auto MLevelEditor::SetError(std::string Message) -> void
	{
		EditorError = std::move(Message);
		DURIN_ERROR("Level editor: {}", EditorError);
	}

	auto MLevelEditor::StartPlay(EEditorPlayStartLocation StartLocation, EEditorPlayDestination Destination) -> void
	{
		if (!GEditor || !Context || !Context->Level) return;
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
			const FReal Pitch = glm::radians(CameraState.Pitch);
			const FReal Yaw = glm::radians(CameraState.Yaw);
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
