#include "Asset/RegistryOperations.h"
#include "Misc/UnifiedError.h"
#include "Asset/LoadErrors.h"
#include "ThirdParty/ImGui/imgui_internal.h"
#include "FLevelEditorToolkit.h"
#include "LevelEditorSubsystem.h"

#include "Asset/Asset.h"
#include "Asset/AssetPicker.h"
#include "Editor/EditorEngine.h"
#include "Notifications/Notification.h"
#include "Transactions/Transaction.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Framework/TabLayout.h"
#include "Settings/LevelEditorWorkspaceSettings.h"
#include "Assets/EditorAssetMoveCoordinator.h"
#include "Engine/Engine.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Engine/ProjectGameSettings.h"
#include "Engine/World.h"
#include "Documents/LevelDocumentController.h"
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

	// FLevelEditorToolkit is the composition root for the editor-specific controllers.
	FLevelEditorToolkit::FLevelEditorToolkit(std::shared_ptr<FLevelEditorWorkspaceSettings> InWorkspaceSettings,
		FContentBrowserCallbacks InContentBrowserCallbacks,
		std::function<void()> InOpenProjectBrowser, DEditorEngine& EditorHost)
		: EditorOwner(&EditorHost)
		, WorkspaceSettingsOwner(std::move(InWorkspaceSettings))
		, WorkspaceSettings(*WorkspaceSettingsOwner)
		, ContentBrowserCallbacks(std::move(InContentBrowserCallbacks))
		, OpenProjectBrowser(std::move(InOpenProjectBrowser))
	{
	}

	FLevelEditorToolkit::~FLevelEditorToolkit() { OnClose(); }

	auto FLevelEditorToolkit::OnClose() -> void
	{
		if (bReleased) return;
		bReleased = true;
		FBaseAssetToolkit::OnClose();
		ViewportSettingsPage = {};
		ProjectSettingsPage = {};
		RequestDeactivate();
		if (Context && SceneViewportPanel)
		{
			WorkspaceSettings.CaptureViewportState(*Context, *SceneViewportPanel);
			WorkspaceSettings.Save(SceneViewportPanel);
		}
		if (DocumentController) (void)DocumentController->ReleaseLevelView();
		DocumentController.reset();
		AssetMoveCoordinator.reset();
		SceneImportDialog.reset();
		Panels.clear();
		SceneViewportPanel = nullptr; RenderingDiagnosticsPanel = nullptr; DetailsPanel = nullptr;
		Context.reset();
		ContentBrowserCallbacks = {};
		OpenProjectBrowser = {};
	}

	auto FLevelEditorToolkit::Construct() -> void
	{
		InitializeContext();
		PruneAndSaveWorkspaceSettings();
		CreatePanels();
		CreateDocumentControllers();
		CreateImportDialogs();
		Context->Synchronize(Context->EditorHost->GetEditorWorld());
		SceneViewportPanel->GetEditModeManager().Synchronize(*Context);
		ViewportSettingsPage = FEditorSettingsPageRegistration({
			.Id = "editor.viewport.host." + std::to_string(FObjectKeyHash{}(FObjectKey(EditorOwner.Get()))), .Label = "Viewport and Controls",
			.Keywords = "camera movement speed occlusion culling grid statistics snap translation rotation scale gizmo",
			.Description = "Level viewport navigation, rendering, overlays and transform snapping. Changes take effect immediately and are saved to the workspace.",
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

	auto FLevelEditorToolkit::InitializeContext() -> void
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
			if (auto* Editors = GetAssetEditorSubsystem(); Editors && Resolution && Resolution.FinalPackageData
				&& Resolution.FinalPackageData->TopLevelAssets.size() == 1)
			{
				const auto& Asset = Resolution.FinalPackageData->TopLevelAssets.front();
				if (Editors->OpenAssets({.AssetEditors = Editors, .Assets = {{FSoftObjectPath(Asset.AssetPath), Asset.AssetClassName}}})) return true;
			}
			Error = "The loaded asset could not be opened.";
			return false;
		};
	}

	auto FLevelEditorToolkit::PruneAndSaveWorkspaceSettings() -> void
	{
		WorkspaceSettings.PruneInvalidViewportStates();
		WorkspaceSettings.Save(nullptr);
	}

	auto FLevelEditorToolkit::CreatePanels() -> void
	{
		auto SceneViewport = std::make_unique<FSceneViewportPanel>(*Context->EditorHost);
		SceneViewportPanel = SceneViewport.get();
		Context->FocusActor = [this](AActor* Actor) {
			if (SceneViewportPanel) SceneViewportPanel->FocusActor(Actor);
		};
		WorkspaceSettings.ApplyTo(*SceneViewportPanel);
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
		auto Details = std::make_unique<FDetailsPanel>(WorkspaceSettings);
		DetailsPanel = Details.get();
		Panels.emplace_back(std::move(Details));
	}

	auto FLevelEditorToolkit::CreateDocumentControllers() -> void
	{
		AssetMoveCoordinator = std::make_unique<FEditorAssetMoveCoordinator>(
			*Context,
			WorkspaceSettings,
			*SceneViewportPanel
		);

		DocumentController = std::make_unique<FLevelDocumentController>(
			*Context,
			WorkspaceSettings,
			*SceneViewportPanel,
			*AssetMoveCoordinator,
			[this] { NotifyEditingObjectsChanged(); },
			[this] { EditorError.clear(); },
			[this](std::string Message) { SetError(std::move(Message)); }
		);
	}

	auto FLevelEditorToolkit::CreateImportDialogs() -> void
	{
		const FImportDialogCallbacks ImportCallbacks{
			.ClearError = [this] { EditorError.clear(); },
			.ReportError =
				[this](std::string Message) { SetError(std::move(Message)); },
			.AssetCreated = [this](std::string AssetPath) {
				if (ContentBrowserCallbacks.RevealAsset)
					ContentBrowserCallbacks.RevealAsset(AssetPath);
			},
			.ImportedDirectory = [this](std::string DirectoryPath) {
				if (ContentBrowserCallbacks.RevealDirectory)
					ContentBrowserCallbacks.RevealDirectory(DirectoryPath);
			},
		};
		SceneImportDialog =
			MakeImportDialog<FSceneImportDialog>(ImportCallbacks);
	}

	auto FLevelEditorToolkit::RequestContentBrowserImport(
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

	auto FLevelEditorToolkit::DrawContentBrowserImport(
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

	auto FLevelEditorToolkit::LoadProjectSettings() -> bool
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

	auto FLevelEditorToolkit::SaveProjectSettings() -> bool
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

	auto FLevelEditorToolkit::ApplyFixedUpDefaultLevelPath(
		const FPackagePath& Path) -> void
	{
		FSoftObjectPath LevelPath;
		if (!ResolveLevelPackage(Path, LevelPath)) return;
		const bool bPendingMatchesSaved = PendingDefaultLevel == DefaultLevel;
		DefaultLevel = LevelPath;
		if (bPendingMatchesSaved)
			PendingDefaultLevel = std::move(LevelPath);
	}

	auto FLevelEditorToolkit::GetToolkitId() const -> const FAssetEditorToolkitId&
	{
		return Workspace::Type;
	}

	auto FLevelEditorToolkit::PostInitAssetEditor() -> void
	{
		FBaseAssetToolkit::PostInitAssetEditor();
		DocumentController->ActivateCurrentLevel();
	}

	auto FLevelEditorToolkit::FocusEditorView() -> void
	{
		RootWindow.RequestFocus();
	}

	auto FLevelEditorToolkit::RequestDeactivate() -> bool
	{
		return !Context || !DetailsPanel || DetailsPanel->RequestDeactivate(*Context);
	}

	auto FLevelEditorToolkit::OnRequestClose(EAssetEditorCloseReason) -> bool
	{
		return PrepareForExit();
	}

	auto FLevelEditorToolkit::PrepareForExit() -> bool
	{
		return RequestDeactivate() && (!DocumentController || !DocumentController->HasPendingTransition());
	}

	auto FLevelEditorToolkit::SaveForExit() -> bool
	{
		return PrepareForExit() && (GetUnsavedChanges().empty() || SaveActiveDocument());
	}

	auto FLevelEditorToolkit::GetEditorDisplayName() const -> std::string
	{
		const auto* Level = DocumentController ? DocumentController->GetCurrentLevel() : nullptr;
		if (!Level) return "Level Editor";
		return DocumentController->GetCurrentLevelAsset() ? Level->GetName() : "Untitled";
	}

	auto FLevelEditorToolkit::GetOwnedEditingObjects() const -> std::optional<std::vector<DObject*>>
	{
		auto* Level = DocumentController ? DocumentController->GetCurrentLevelAsset() : nullptr;
		return Level ? std::vector<DObject*>{Level} : std::vector<DObject*>{};
	}

	auto FLevelEditorToolkit::GetUnsavedChanges() const -> std::vector<FAssetEditorUnsavedChange>
	{
		if (!DocumentController || !DocumentController->IsCurrentLevelDirty()) return {};
		const auto* Asset = DocumentController->GetCurrentLevelAsset();
		return {{GetEditorDisplayName(), Asset ? Asset->GetObjectPath() : std::string{}}};
	}

	auto FLevelEditorToolkit::RequestCloseLevel() -> bool
	{
		return IsEditorOpen() && RequestDeactivate() && DocumentController && DocumentController->RequestCloseLevel();
	}

	auto FLevelEditorToolkit::SaveLevelAs(const FPackagePath& Path) -> bool
	{
		return IsEditorOpen() && DocumentController && DocumentController->SaveCurrentLevelAs(Path);
	}

	auto FLevelEditorToolkit::ResetLayout() -> void
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

	auto FLevelEditorToolkit::OnDrawToolkit(bool bActive) -> bool
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("LevelEditor.DrawToolkit");
		if (!Context || !DocumentController) return false;
		DocumentController->DrawDialogs();
		if (!IsEditorOpen() || !Context || !DocumentController) return false;
		bWasActive = bActive;
		const ImGuiID DockSpaceId = TabLayout::MakeDockSpaceId(Workspace::Type, Workspace::LayoutVersion);
		const bool bLevelDirty = DocumentController->IsCurrentLevelDirty();
		const FEditorTabWindowState RootWindowState = RootWindow.Begin({
			.DisplayName = GetEditorDisplayName(),
			.RootKey = Workspace::RootKey,
			.bDirty = bLevelDirty,
			.bZeroPadding = true,
			.OpenLocation = GetOpenLocation(),
			.InternalDockSpace = FToolkitInternalDockSpace{
				.ToolkitId = Workspace::Type,
				.LayoutVersion = Workspace::LayoutVersion,
			},
		});
		if (!RootWindowState.bVisible)
		{
			RootWindow.End();
			if (RootWindowState.bCloseRequested)
				CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed);
			return RootWindowState.bActivated;
		}

		bool bPlaying = Context->EditorHost && Context->EditorHost->IsPlaying();
		Context->bReadOnly = (Context->EditorHost && !Context->EditorHost->GetCapabilities().bEditWorld);
		Context->Synchronize(Context->EditorHost != nullptr
			? (bPlaying ? Context->EditorHost->GetPlayWorld() : Context->EditorHost->GetEditorWorld())
			: (GEngine != nullptr ? GEngine->GetWorld() : nullptr));
		const ImGuiIO& IO = ImGui::GetIO();
		if (bActive || RootWindowState.bFocused)
		{
			if (!IO.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F5, false) && Context->EditorHost && Context->EditorHost->GetCapabilities().bRunTools)
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
			Context->bReadOnly = !Context->EditorHost->GetCapabilities().bEditWorld;
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
			CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed);
			if (!IsEditorOpen()) return false;
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

	auto FLevelEditorToolkit::OnCanSaveActiveDocument() const -> bool
	{
		return DocumentController && DocumentController->CanSaveCurrentLevel();
	}

	auto FLevelEditorToolkit::OnSaveActiveDocument() -> bool
	{
		return CanSaveActiveDocument() && DocumentController->SaveCurrentLevel();
	}

	auto FLevelEditorToolkit::OnCanUndo() const -> bool
	{
		const bool bDragging = SceneViewportPanel && SceneViewportPanel->GetTransformGizmo() && SceneViewportPanel->GetTransformGizmo()->IsDragging();
		return Context->EditorHost && Context->EditorHost->GetCapabilities().bEditWorld && !bDragging && Context->EditorHost->GetTransactor()->CanUndo();
	}

	auto FLevelEditorToolkit::OnCanRedo() const -> bool
	{
		const bool bDragging = SceneViewportPanel && SceneViewportPanel->GetTransformGizmo() && SceneViewportPanel->GetTransformGizmo()->IsDragging();
		return Context->EditorHost && Context->EditorHost->GetCapabilities().bEditWorld && !bDragging && Context->EditorHost->GetTransactor()->CanRedo();
	}

	auto FLevelEditorToolkit::GetUndoDescription() const -> std::string
	{
		return CanUndo() ? Context->EditorHost->GetTransactor()->GetUndoContext().Description : std::string{};
	}

	auto FLevelEditorToolkit::GetRedoDescription() const -> std::string
	{
		return CanRedo() ? Context->EditorHost->GetTransactor()->GetRedoContext().Description : std::string{};
	}

	auto FLevelEditorToolkit::OnUndo() -> bool
	{
		return CanUndo() && Context->EditorHost->GetTransactor()->Undo();
	}

	auto FLevelEditorToolkit::OnRedo() -> bool
	{
		return CanRedo() && Context->EditorHost->GetTransactor()->Redo();
	}

	auto FLevelEditorToolkit::DrawFileMenu() -> void
	{
		const bool bPlaying = Context->EditorHost && Context->EditorHost->IsPlaying();
		if (bPlaying) ImGui::BeginDisabled();
		if (ImGui::MenuItem("Open Project...") && OpenProjectBrowser)
			OpenProjectBrowser();
		if (DocumentController)
		{
			if (ImGui::MenuItem("Save Level As...", nullptr, false, DocumentController->CanEditCurrentLevel()))
				DocumentController->RequestSaveAs();
			if (ImGui::MenuItem("Close Level", nullptr, false, DocumentController->GetCurrentLevel() != nullptr))
				RequestCloseLevel();
		}
		if (bPlaying) ImGui::EndDisabled();
	}

	auto FLevelEditorToolkit::RequestOpenProject(std::string ProjectFile) -> void
	{
		if (DocumentController)
			DocumentController->RequestOpenProject(std::move(ProjectFile));
	}

	auto FLevelEditorToolkit::DrawEditMenu() -> void
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

	auto FLevelEditorToolkit::DrawWindowMenu() -> void
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

	auto FLevelEditorToolkit::DrawProjectSettings(std::string& OutError) -> void
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

	auto FLevelEditorToolkit::SetError(std::string Message) -> void
	{
		EditorError = std::move(Message);
		DURIN_ERROR("Level editor: {}", EditorError);
	}

	auto FLevelEditorToolkit::StartPlay(EPlayStartLocation StartLocation, EPlayDestination Destination) -> void
	{
		if (!Context || !Context->EditorHost || !Context->Level || !Context->EditorHost->GetCapabilities().bEditWorld) return;
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

	auto FLevelEditorToolkit::ApplyPlayChanges(bool bSelectedOnly) -> void
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

	auto FLevelEditorToolkit::BuildDefaultLayout(uint32 DockSpaceId, float DockSpaceWidth, float DockSpaceHeight) -> void
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
