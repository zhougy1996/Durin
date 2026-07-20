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
#include "TextureImportDialog.h"
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
		Context->FocusActor = [this](AActor* Actor) {
			if (SceneViewportPanel) SceneViewportPanel->FocusActor(Actor);
		};
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
		TextureImportDialog = std::make_unique<FTextureImportDialog>(
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
			[this](const std::string& DestinationDirectory, EContentBrowserImportType ImportType) {
				if (ImportType == EContentBrowserImportType::Texture)
				{
					if (TextureImportDialog) TextureImportDialog->Open(DestinationDirectory);
				}
				else if (StaticMeshImportDialog) StaticMeshImportDialog->Open(DestinationDirectory);
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
		RootWindow.RequestFocus();
	}

	auto MLevelEditor::RequestCloseDocument(const FEditorDocumentTab& Document) -> bool
	{
		return !IsDocumentDirty(Document);
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
		if (!Context || !DocumentController || !StaticMeshImportDialog || !TextureImportDialog) return false;
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
		StaticMeshImportDialog->Draw();
		TextureImportDialog->Draw();
		DrawEditorPreferences();
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
		if (ImGui::MenuItem("Project Settings...")) bProjectSettingsOpen = true;
		if (ImGui::MenuItem("Editor Preferences...")) bEditorPreferencesOpen = true;
		if (bPlaying) ImGui::EndDisabled();
	}

	auto MLevelEditor::DrawWindowMenu() -> void
	{
		ImGui::Separator();
		if (ImGui::BeginMenu("Panels###Durin.LevelEditor.Windows"))
		{
			for (const std::unique_ptr<ILevelEditorPanel>& Panel : Panels)
			{
				bool bPanelOpen = Panel->IsOpen();
				if (ImGui::MenuItem(Panel->GetWindowName(), nullptr, &bPanelOpen)) Panel->SetOpen(bPanelOpen);
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Reset Layout")) ResetLayout();
			ImGui::EndMenu();
		}
	}

	auto MLevelEditor::DrawEditorPreferences() -> void
	{
		if (!bEditorPreferencesOpen) return;
		ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(MonaImGui::ScaleUI(430.0f), MonaImGui::ScaleUI(230.0f)), ImGuiCond_Appearing);
		if (ImGui::Begin("Editor Preferences###Durin.LevelEditor.EditorPreferences", &bEditorPreferencesOpen, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings))
		{
			ImGui::SeparatorText("Appearance");
			ImGui::AlignTextToFramePadding();
			ImGui::TextDisabled("Color theme");
			ImGui::SameLine(MonaImGui::ScaleUI(130.0f));
			const MonaImGui::EColorTheme CurrentTheme = MonaImGui::GetColorTheme();
			const char* ThemeLabel = CurrentTheme == MonaImGui::EColorTheme::Light ? "Light" : "Dark";
			ImGui::SetNextItemWidth(-1.0f);
			if (ImGui::BeginCombo("##ColorTheme", ThemeLabel))
			{
				for (const auto [Label, Theme] : {std::pair{"Dark", MonaImGui::EColorTheme::Dark}, std::pair{"Light", MonaImGui::EColorTheme::Light}})
				{
					if (ImGui::Selectable(Label, CurrentTheme == Theme))
					{
						MonaImGui::SetColorTheme(Theme);
						SessionSettings.Save(SceneViewportPanel);
					}
				}
				ImGui::EndCombo();
			}
			ImGui::AlignTextToFramePadding();
			ImGui::TextDisabled("UI scale");
			ImGui::SameLine(MonaImGui::ScaleUI(130.0f));
			const float CurrentScale = SessionSettings.GetUIScale();
			const std::string ScaleLabel = std::format("{}%", static_cast<int32>(CurrentScale * 100.0f));
			ImGui::SetNextItemWidth(-1.0f);
			if (ImGui::BeginCombo("##UIScale", ScaleLabel.c_str()))
			{
				for (const float Scale : {0.75f, 1.0f, 1.25f, 1.5f, 2.0f})
				{
					const std::string Label = std::format("{}%", static_cast<int32>(Scale * 100.0f));
					if (ImGui::Selectable(Label.c_str(), std::abs(CurrentScale - Scale) < 0.01f))
						ApplyDisplaySettings(SessionSettings.GetWindowWidth(), SessionSettings.GetWindowHeight(), Scale);
				}
				ImGui::EndCombo();
			}
		}
		ImGui::End();
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
				if (ImGui::BeginCombo("##DefaultLevel", DefaultLevel.empty() ? "None" : DefaultLevel.c_str()))
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
			if (ImGui::Button("Save", ImVec2(ButtonWidth, 0.0f)) && SaveProjectSettings()) bProjectSettingsOpen = false;
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
