#include "MainFrameModule.h"
#include "Panels/PythonScriptPanel.h"
#include "ThirdParty/ImGui/imgui_internal.h"
#include "AssetCompatibilityWindow.h"

#include "EditorBranding.h"
#include "Feedback/SlowTaskWidget.h"
#include "ProjectBrowser.h"
#include "ProfilingToolService.h"
#include "Tools/TracyToolset.h"
#include "Tools/NamePoolToolset.h"
#include "Modules/ModularFeature.h"

#include "Documents/EditorDocumentManager.h"
#include "Documents/EditorDocumentUI.h"
#include "Framework/TabLayout.h"
#include "Editor/EditorEngine.h"
#include "Engine/HostStartup.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditor.h"
#include "Toolkits/ToolkitHost.h"
#include "Framework/TabManager.h"
#include "Viewport/ViewportClosePolicy.h"
#include "Window/GenericWindow.h"
#include "DObject/WeakObjectPtr.h"
#include "Transactions/Transaction.h"
#include "Notifications/Notification.h"
#include "ContentBrowser/ContentBrowserTool.h"
#include "Mona.h"
#include "LevelEditorModule.h"
#include "Thumbnail/ThumbnailManager.h"
#include "Thumbnail/AssetThumbnailPool.h"
#include "Icons/FontAwesomeIcons.h"
#include "MonaImGui.h"
#include "Misc/Paths.h"
#include "Misc/Project.h"
#include "Misc/Version.h"
#include "RHIGlobals.h"
#include "Profiling/Profiling.h"
#include "Settings/MainFrameSettings.h"
#include "Settings/EditorSettingsWindow.h"
#include "Settings/EditorSettings.h"
#include "Panels/ConsolePanel.h"
#include "Widgets/EditorNotificationOverlay.h"
#include "Asset/Asset.h"
#include "AssetTools/IAssetTools.h"
#include "Import/EditorReimportHandler.h"
#include "Import/ImportDialogSupport.h"
#include "Dialogs/FileDialog.h"

#include "Widgets/MFunctionWidget.h"
#include "Widgets/MWindow.h"
#include "Rendering/MonaRenderer.h"

namespace Durin::Editor::MainFrame
{
	using namespace Startup;

	struct FMainFrameContext
	{
		~FMainFrameContext()
		{
			if (RootWindow) RootWindow->SetCloseRequestHandler({});
			if (ContentBrowserTool)
				ContentBrowserTool->StopRequestAdmission();
			// Release every UI-held FAssetThumbnail reference before renderer
			// registrations and their owning modules begin retirement.
			ContentBrowserTool.reset();
			ReleaseAssetEditors();
			if (LevelEditorModule)
				if (auto* Service = AssetEditors.Get()) LevelEditorModule->ReleaseLevelEditor(*Service);
			GetDefaultThumbnailManager().ResetSharedPool();
			Activity.reset();
			Console.reset();
		}

		auto ReleaseAssetEditors() -> void
		{
			if (auto* Service = AssetEditors.Get())
			{
				if (!SessionProject.empty() && SessionProject == FPaths::ProjectFile())
					if (!Service->SaveSessionState()) DURIN_WARN("Could not save asset editor session state.");
				SessionProject.clear();
				Service->SetToolkitHostFactory({});
				Service->ShutdownAssetEditorSessions();
			}

		}

		FSlowTaskDialogPresenter SlowTaskDialogs;
		TWeakObjectPtr<DEditorEngine> Editor;
		bool bHasProject = false;
		std::string SessionProject;
		bool bProjectBrowserOpen = false;
		bool bExitRequested = false;
		bool bExitSaveFailed = false;
		std::shared_ptr<FMainFrameSettings> MainFrameSettings;
		std::shared_ptr<MWindow> RootWindow;
		TRefCountPtr<FRHIViewport> ShellViewport;
		uint64 ShellPresentationBaseline = 0;
		TWeakObjectPtr<DAssetEditorSubsystem> AssetEditors;
		std::shared_ptr<IToolkitHost> WorldHost;
		std::shared_ptr<FProjectBrowser> ProjectBrowser;
		std::unique_ptr<FEditorBrandTexture> BrandTexture;
		std::shared_ptr<FAssetCompatibilityWindow> AssetCompatibilityWindow;
		FLevelEditorModule* LevelEditorModule = nullptr;
		std::unique_ptr<ContentBrowser::IContentBrowserTool> ContentBrowserTool;
		std::shared_ptr<ContentBrowser::FMountedContentReconciliationState> ContentReconciliation =
			std::make_shared<ContentBrowser::FMountedContentReconciliationState>();
		std::unique_ptr<FConsolePanel> Console;
		std::unique_ptr<FPythonScriptPanel> PythonScripts;
		std::unique_ptr<FEditorNotificationOverlay> Activity;
	};

		namespace
		{
		class FMainFrameToolkitHost final : public FNonWindowToolkitHost,
			public std::enable_shared_from_this<FMainFrameToolkitHost>
		{
		public:
			FMainFrameToolkitHost(std::weak_ptr<FMainFrameContext> InContext, EToolkitMode Mode)
				: FNonWindowToolkitHost(Mode), Context(std::move(InContext)) {}
			auto AttachToolkit(std::shared_ptr<FAssetEditorToolkit> Toolkit) -> bool override
			{
				if (GetToolkitMode() == EToolkitMode::Standalone && !Attached.expired()) return false;
				if (!FNonWindowToolkitHost::AttachToolkit(Toolkit)) return false;
				if (GetToolkitMode() == EToolkitMode::Standalone) Attached = Toolkit;
				return true;
			}
			auto IsAvailable() const -> bool override
			{
				const auto Owner = Context.lock();
				return Owner && Owner->RootWindow && !Owner->bExitRequested && FNonWindowToolkitHost::IsAvailable()
					&& (GetToolkitMode() != EToolkitMode::WorldCentric || Owner->LevelEditorModule);
			}
			auto PrepareRootWindow(FEditorTabWindowConfig& Config) -> void override
			{
				Config.bNativeStandalone = GetToolkitMode() == EToolkitMode::Standalone;
				Config.bDockInEditorHost = !Config.bNativeStandalone;
			}
			auto IsReadyForFocus() const -> bool override
			{
				if (!IsAvailable()) return false;
				const auto Owner = Context.lock();
				const auto* Editor = Owner ? Owner->Editor.Get() : nullptr;
				if (!Editor || !Editor->GetCapabilities().bTick) return false;
				if (GetToolkitMode() == EToolkitMode::WorldCentric) return true;
				const auto Window = MonaImGui::GetWindowForViewport(ViewportId);
				return Window && Window->GetNativeWindow();
			}
			auto OnRootWindowShown(uint32 Id) -> void override
			{
				if (GetToolkitMode() != EToolkitMode::Standalone || ViewportId == Id) return;
				ClosePolicy.Reset();
				ViewportId = Id;
				ClosePolicy = MonaImGui::RegisterViewportClosePolicy(Id, [Weak = weak_from_this()] {
					const auto Self = Weak.lock();
					return !Self || Self->RequestClose();
				});
			}
			auto DetachToolkit(FAssetEditorToolkit& Toolkit) -> void override
			{
				ClosePolicy.Reset();
				ViewportId = 0;
				Attached.reset();
				FNonWindowToolkitHost::DetachToolkit(Toolkit);
			}
			auto FocusToolkit(FAssetEditorToolkit&) -> void override
			{
				if (GetToolkitMode() == EToolkitMode::WorldCentric)
					if (const auto Owner = Context.lock(); Owner && Owner->RootWindow)
						if (const auto Native = Owner->RootWindow->GetNativeWindow()) Native->Focus();
				if (const auto Window = MonaImGui::GetWindowForViewport(ViewportId))
					if (const auto Native = Window->GetNativeWindow()) Native->Focus();
			}
		private:
			std::weak_ptr<FMainFrameContext> Context;
			MonaImGui::FViewportClosePolicyRegistration ClosePolicy;
			uint32 ViewportId = 0;
			std::weak_ptr<FAssetEditorToolkit> Attached;
		};

		auto LoadReimportObject(std::string_view AssetPath, FObjectPath& OutPath,
			DObject*& OutObject, std::string& OutError) -> bool
		{
			if (!FObjectPath::TryCreate(AssetPath, OutPath))
			{
				OutError = "The selected asset path is invalid.";
				return false;
			}
			const auto Loaded = LoadObject<DObject>(OutPath);
			OutObject = Loaded.value_or(nullptr);
			if (!Loaded || !OutObject)
			{
				OutError = Loaded ? "The selected asset could not be loaded." : (Loaded ? std::string{} : Loaded.error().Message);
				return false;
			}
			return true;
		}

		auto SelectReimportFiles(DObject& Object, std::vector<std::string>& OutFiles,
			std::string& OutError) -> bool
		{
			const auto Dialogs = FReimportManager::GetSourceFileDialogs(Object, OutError);
			if (Dialogs.empty()) return false;
			for (const auto& Dialog : Dialogs)
			{
				FFileDialogRequest Request;
				Request.Title = Dialog.Title;
				Request.Filters = {{Dialog.FilterName, Dialog.Pattern}};
				const auto Selection = OpenFileDialog(Request);
				if (Selection.Status == EFileDialogStatus::Cancelled) return false;
				if (Selection.Status == EFileDialogStatus::Error)
				{
					OutError = Selection.ErrorMessage;
					return false;
				}
				OutFiles.push_back(Selection.FilePath);
			}
			return true;
		}

		auto ExecuteReimport(bool bFromFile, std::string AssetPath,
			std::function<void(std::string)> ReportError) -> void
		{
			FObjectPath Path;
			DObject* Object = nullptr;
			std::string Error;
			if (!LoadReimportObject(AssetPath, Path, Object, Error))
			{
				if (ReportError) ReportError(std::move(Error));
				return;
			}
		auto Completion = [ReportError](
				FReimportResult Result) mutable {
				if (Result.Interrupted()) return;
				if (!Result)
				{
					if (ReportError) ReportError(Result.Message.empty()
						? "Asset reimport failed." : std::move(Result.Message));
					return;
				}
				if (Result.SaveResult && !*Result.SaveResult && ReportError)
					ReportError("Reimport succeeded; save failed: " + Result.SaveResult->Message);
			};
			if (!bFromFile)
			{
				FReimportManager::Reimport(*Object, {}, std::move(Completion));
				return;
			}
			std::vector<std::string> Files;
			if (!SelectReimportFiles(*Object, Files, Error))
			{
				if (!Error.empty() && ReportError) ReportError(std::move(Error));
				return;
			}
			FReimportManager::ReimportFromFiles(
				*Object, Files, {}, std::move(Completion));
		}

		enum class EMainFrameDrawerTool : uint8
		{
			ContentBrowser,
			Console,
		};

		struct FMainFrameViewState
		{
			bool bAboutDialogOpen = false;
			FEditorSettingsWindow SettingsWindow;
			bool bAssetUpgradeOpen = false;
			bool bContentBrowserOpen = false;
			bool bConsoleOpen = false;
			bool bPythonOpen = false;
			bool bActivityHistoryRequested = false;
			bool bResetMainFrameLayoutRequested = false;
			EMainFrameDrawerTool DrawerTool = EMainFrameDrawerTool::ContentBrowser;
			MonaImGui::FBottomDrawerState Drawer;
		};
		constexpr float EditorTitleBarHeight = 44.0f;
		constexpr float EditorTitleBarBrandHeight = 22.0f;
		constexpr float EditorTitleBarBrandSlotWidth = 30.0f;
		constexpr float EditorCaptionButtonWidth = 46.0f;
		constexpr float EditorTitleBarGap = 10.0f;
		constexpr float EditorTitleBarMenuGap = 28.0f;
		constexpr float EditorTitleBarTitleGap = 12.0f;

		auto InitializeEditorWorkspaces(
			FMainFrameContext& Context, std::string& OutError) -> bool
		{
			{
				DURIN_PROFILE_CPU_ZONE_NAMED("Startup.WorkspaceRegistration");
				FLevelEditorModule& LevelEditorModule =
					*static_cast<FLevelEditorModule*>(FModuleManager::Get().GetModule("LevelEditor"));
				Context.LevelEditorModule = &LevelEditorModule;
				if (!LevelEditorModule.InitializeLevelEditor(
					*Context.AssetEditors.Get(),
					{
						.RevealAsset = [&Context](std::string_view Path) {
							return Context.ContentBrowserTool
								&& Context.ContentBrowserTool->RevealAsset(Path);
						},
						.RevealDirectory = [&Context](std::string_view Path) {
							return Context.ContentBrowserTool
								&& Context.ContentBrowserTool->RevealDirectory(Path);
						},
						.NotifyMountedContentChanged = [&Context] {
							return Context.ContentBrowserTool
								&& Context.ContentBrowserTool->NotifyMountedContentChanged();
						},
					},
					[&Context] { Context.bProjectBrowserOpen = true; }))
				{
					Profiling::RecordStartupMilestone(Profiling::EStartupMilestone::WorkspaceRegistrationComplete);
					OutError = "Could not initialize the Level Editor workspace.";
					return false;
				}
				ContentBrowser::FConstructionServices Services{
					.OpenAsset = [Service = Context.AssetEditors](const std::string& Path,
						const std::string& AssetClassName) {
						auto* AssetEditors = Service.Get();
						if (!AssetEditors || !AssetEditors->GetWorkGate()->IsOpen()) return false;
						FObjectPath ObjectPath;
						if (!FObjectPath::TryCreate(Path, ObjectPath)) return false;
						return AssetEditors->OpenAssets({.AssetEditors = AssetEditors,
							.Assets = {{ObjectPath, AssetClassName}}, .Origin = "ContentBrowser"}).IsAccepted();
					},
					.GetMountedContentMutationRevision = [] {
						return GEditor
							? GEditor->GetTransactor()
								->GetMountedContentMutationRevision()
							: uint64{0};
					},
					.NotifyMountedContentMutation = [] {
						if (GEditor)
							GEditor->GetTransactor()
								->NotifyMountedContentMutation();
					},
					.CaptureMountedContentChanges = [](uint64 From) {
						return GEditor ? GEditor->GetTransactor()->CaptureMountedContentChanges(From)
							: FContentChangeBatch{.bFullRefresh = true};
					},
					.NotifyScopedContentMutation = [](FContentChangeBatch Changes) {
						if (GEditor) GEditor->GetTransactor()->NotifyMountedContentMutation(std::move(Changes));
					},
					.CanMutateContent = [] { return GEditor && GEditor->GetCapabilities().bEditWorld; },
					.QueryReimport = [](std::string_view AssetClassName) {
						const auto Actions = FReimportManager::QueryReimportActions(AssetClassName);
						return ContentBrowser::FReimportAvailability{
							Actions.bSupportsReimport, Actions.bSupportsReimportFromFile};
					},
					.Reimport = [](bool bFromFile, std::string AssetPath,
						std::function<void(std::string)> ReportError) {
						ExecuteReimport(bFromFile, std::move(AssetPath),
							std::move(ReportError));
					},
				};
				Services.ReconciliationState = Context.ContentReconciliation;
				ContentBrowser::FPresentationSettings BrowserSettings;
				std::string BrowserSettingsWarning;
				if (!ContentBrowser::LoadPresentationSettings(
					BrowserSettings, &BrowserSettingsWarning)
					&& !BrowserSettingsWarning.empty())
					DURIN_WARN("{}", BrowserSettingsWarning);
				Services.AssetEditors = Context.AssetEditors;
				Context.ContentBrowserTool = ContentBrowser::CreateContentBrowserTool(
					std::move(Services),
					std::move(BrowserSettings),
					[](
						const ContentBrowser::FPresentationSettings& Settings) {
						return ContentBrowser::SavePresentationSettings(Settings);
					});
			}
			Profiling::RecordStartupMilestone(
				Profiling::EStartupMilestone::WorkspaceRegistrationComplete);
			if (const auto* Project = GetCurrentProject(); Project && Project->bRecordInteractiveHistory)
				Context.ProjectBrowser->RecordCurrentProject();
			if (HasCurrentProject())
			{
				Context.SessionProject = FPaths::ProjectFile();
				const auto Restored = Context.AssetEditors.Get()->LoadSessionState();
				for (const auto& Item : Restored.Items)
					if (!Item.IsAccepted()) Context.Editor.Get()->GetNotificationManager().Post({
						.Type = ENotificationType::Warning, .Message = "An asset editor could not be restored.",
						.Details = Item.Target.AssetPath.ToString() + ": " + Item.Message});
			}
			Profiling::RecordStartupMilestone(
				Profiling::EStartupMilestone::DefaultWorkspaceReady);
			return true;
		}
		auto DrawExitConfirmation(FMainFrameContext& Context) -> void
		{
			if (!Context.bExitRequested) return;
			std::vector<std::shared_ptr<FEditorDocumentManager>> Managers;
			if (auto* Service = Context.AssetEditors.Get())
				for (const auto& Session : Service->GetOwnedAssetEditors()) Managers.push_back(Session->GetDocumentManager());
			const auto Prepare = [&] {
				bool Ready = true;
				for (const auto& Documents : Managers) Ready = Documents->PrepareForExit() && Ready;
				return Ready;
			};
			const bool bPrepared = Prepare();
			const bool bDirty = std::ranges::any_of(Managers, [](const auto& Documents) {
				return std::ranges::any_of(Documents->GetDocuments(), &FEditorDocument::bDirty);
			});
			if (bPrepared && !bDirty && !Context.bExitSaveFailed)
			{
				Context.bExitRequested = false;
				Context.RootWindow->RequestDestroyWindow();
				return;
			}
			ImGui::OpenPopup("Exit Editor###Durin.Editor.ExitConfirmation");
			if (!ImGui::BeginPopupModal("Exit Editor###Durin.Editor.ExitConfirmation", nullptr,
				ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) return;
			ImGui::TextUnformatted("Save all changes before exiting?");
			for (const auto& Documents : Managers)
				for (const auto& Document : Documents->GetDocuments())
					if (Document.bDirty) ImGui::BulletText("%s", Document.Label.c_str());
			if (!bPrepared) ImGui::TextWrapped("Finish the pending document operation before exiting.");
			if (Context.bExitSaveFailed) ImGui::TextWrapped("Some changes could not be saved. Resolve the error and retry, or cancel exit.");
			bool bExit = false;
			ImGui::BeginDisabled(!bPrepared);
			if (ImGui::Button("Save All and Exit"))
			{
				bExit = true;
				for (const auto& Documents : Managers) bExit = Documents->SaveDocumentsForExit() && bExit;
				Context.bExitSaveFailed = !bExit;
			}
			ImGui::SameLine();
			// Discard authorizes teardown only; it must not roll back live documents before approval.
			if (ImGui::Button("Exit Without Saving")) bExit = Prepare();
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button("Cancel") || bExit)
			{
				Context.bExitRequested = false;
				Context.bExitSaveFailed = false;
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
			if (bExit) Context.RootWindow->RequestDestroyWindow();
		}

		auto DrawLoadingState(const DEditorEngine& Editor) -> void
		{
			ImGuiViewport* Viewport = ImGui::GetMainViewport();
			ImGui::SetNextWindowPos(Viewport->WorkPos);
			ImGui::SetNextWindowSize(Viewport->WorkSize);
			ImGui::SetNextWindowViewport(Viewport->ID);
			const ImGuiWindowFlags Flags = ImGuiWindowFlags_NoDocking
				| ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar
				| ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize
				| ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus
				| ImGuiWindowFlags_NoNavFocus;
			ImGui::Begin("###Durin.Editor.LoadingHost", nullptr, Flags);
			const char* Message = "Loading project workspace...";
			if (Editor.GetStartupPhase() == EBootstrapState::WaitingForFirstPresent)
				Message = "Preparing editor...";
			else if (Editor.GetStartupPhase() == EBootstrapState::InitializingProject)
				Message = "Initializing project...";
			else if (Editor.GetStartupPhase() == EBootstrapState::LoadingDefaultDocument)
				Message = "Opening default level...";
			FSlowTaskSnapshot Feedback{.Title = "Starting editor...", .Message = Message};
			bool bShowElapsed = false;
			if (Editor.GetStartupPhase() == EBootstrapState::LoadingDefaultDocument)
			{
				if (const auto Progress = Editor.GetMapChangeProgress(); Progress && Editor.IsPreparingMapChange())
				{
					Feedback.Message = Progress->Phase == EMapChangePhase::Ready
						? "Activating level" : std::string(GetMapChangePhaseLabel(Progress->Phase));
					Feedback.ElapsedSeconds = Progress->ElapsedSeconds;
					bShowElapsed = true;
				}
				else if (Editor.GetCapabilities().bPresentWorkspace) Feedback.Message = "Waiting for the scene's first presentation";
			}
			const ImVec2 ContentOrigin = ImGui::GetCursorPos();
			const ImVec2 Available = ImGui::GetContentRegionAvail();
			const float ContentWidth = MonaImGui::ScaleUI(360.0f);
			const float ContentHeight = MonaImGui::ScaleUI(100.0f);
			ImGui::SetCursorPos({ContentOrigin.x + std::max(0.0f, (Available.x - ContentWidth) * 0.5f),
				ContentOrigin.y + std::max(0.0f, (Available.y - ContentHeight) * 0.5f)});
			DrawSlowTaskProgress(Feedback, false, bShowElapsed);
			ImGui::End();
		}

		auto BuildDefaultMainFrameLayout(
			ImGuiID DockSpaceId,
			const ImVec2& DockSpaceSize
		) -> void
		{
			ImGui::DockBuilderRemoveNode(DockSpaceId);
			ImGui::DockBuilderAddNode(DockSpaceId, ImGuiDockNodeFlags_DockSpace | ImGuiDockNodeFlags_NoWindowMenuButton);
			ImGui::DockBuilderSetNodeSize(DockSpaceId, DockSpaceSize);
			if (ImGuiDockNode* DockSpaceNode = ImGui::DockBuilderGetNode(DockSpaceId))
				DockSpaceNode->WindowClass = TabLayout::MakeRootWindowClass();
			ImGuiID CenterDockId = DockSpaceId;
			ImGuiID ToolDockId = 0;
			ImGui::DockBuilderSplitNode(
				DockSpaceId, ImGuiDir_Down, 0.28f, &ToolDockId, &CenterDockId);
			ImGui::DockBuilderDockWindow(
				"Content Browser###Durin.EditorHost.ContentBrowser", ToolDockId);
			const auto LevelWindowName = TabLayout::MakeRootWindowName("Level Editor", "LevelEditor");
			ImGui::DockBuilderDockWindow(LevelWindowName.c_str(), CenterDockId);
			ImGui::DockBuilderFinish(DockSpaceId);
		}

		auto DrawAboutDialog(bool& bOpen) -> void
		{
			if (!bOpen) return;
			ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
			ImGui::SetNextWindowSize(ImVec2(MonaImGui::ScaleUI(420.0f), MonaImGui::ScaleUI(200.0f)), ImGuiCond_Appearing);
			const ImGuiWindowFlags Flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
				ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
			if (!ImGui::Begin("About Durin###Durin.About", &bOpen, Flags))
			{
				ImGui::End();
				return;
			}

			ImGui::Text("Durin Engine");
			ImGui::Separator();
			ImGui::TextDisabled("Version");
			ImGui::SameLine(MonaImGui::ScaleUI(90.0f));
			ImGui::Text("%s", GetEngineVersionString().data());
			ImGui::TextDisabled("RHI Backend");
			ImGui::SameLine(MonaImGui::ScaleUI(90.0f));
			const auto Backend = GetActiveRHIBackend();
			ImGui::TextUnformatted(Backend == ERHIBackend::Metal ? "Metal"
				: Backend == ERHIBackend::Vulkan ? "Vulkan" : "Unavailable");
			ImGui::Spacing();
			ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - MonaImGui::ScaleUI(82.0f));
			if (ImGui::Button("Close", ImVec2(MonaImGui::ScaleUI(82.0f), 0.0f))) bOpen = false;
			ImGui::End();
		}

		auto ObserveMainWindowState(FMainFrameSettings& Settings, const MWindow& RootWindow) -> void
		{
			const bool bMaximized = RootWindow.IsMaximized();
			const auto* Renderer = Mona::FMonaApplication::Get().GetRenderer();
			const bool bVSync = !Renderer || Renderer->GetPresentationPolicyOverride()
				.value_or(EViewportPresentationPolicy::FramePaced) != EViewportPresentationPolicy::Unsynchronized;
			if (bMaximized == Settings.IsWindowMaximized() && bVSync == Settings.IsVSyncEnabled()) return;
			Settings.SetWindowMaximized(bMaximized);
			Settings.SetVSyncEnabled(bVSync);
			Settings.Save();
		}

		auto DrawToolsMenu(bool& bAssetUpgradeOpen) -> void
		{
			if (!ImGui::BeginMenu("Tools")) return;
			if (ImGui::MenuItem("Project Asset Upgrade...")) bAssetUpgradeOpen = true;
			ImGui::EndMenu();
		}

		auto DrawOpenEditorsMenu(DAssetEditorSubsystem& AssetEditors) -> void
		{
			if (!ImGui::BeginMenu("Editors")) return;
			bool bLevelOpen = false;
			for (const auto& Session : AssetEditors.GetOwnedAssetEditors())
			{
				const auto Name = Session->GetEditorName().ToString();
				bLevelOpen |= Name == "LevelEditor";
				const auto Label = Name + " - " + (Session->GetDocumentManager()->GetDocuments().empty() ? Name : Session->GetDocumentManager()->GetDocuments().front().Label)
					+ (Session->GetOpenMethod() == EAssetOpenMethod::View ? " (View)" : "")
					+ (Session->GetInstanceKey().empty() ? "" : " [" + std::string(Session->GetInstanceKey()) + "]");
				if (ImGui::MenuItem(Label.c_str(), nullptr, AssetEditors.GetActiveAssetToolkit() == Session->GetToolkit())) Session->FocusWindow();
			}
			if (!bLevelOpen && ImGui::MenuItem("Level Editor"))
				static_cast<FLevelEditorModule*>(FModuleManager::Get().GetModule("LevelEditor"))->GetOrCreateLevelEditor(AssetEditors);
			ImGui::EndMenu();
		}

		auto DrawWorkspaceMenus(
			DAssetEditorSubsystem& AssetEditors,
			const std::shared_ptr<IAssetEditorToolkit>& ActiveWorkspace,
			FMainFrameViewState& ViewState) -> void
		{
			std::vector<std::shared_ptr<IAssetEditorToolkit>> Toolkits;
			for (const auto& Session : AssetEditors.GetOwnedAssetEditors()) if (auto Toolkit = Session->GetToolkit()) Toolkits.push_back(std::move(Toolkit));
			if (ImGui::BeginMenu("File"))
			{
				if (ImGui::MenuItem("Save Current", "Ctrl+S", false, ActiveWorkspace && ActiveWorkspace->CanSaveActiveDocument()))
					ActiveWorkspace->SaveActiveDocument();
				ImGui::Separator();
				for (const std::shared_ptr<IAssetEditorToolkit>& Workspace : Toolkits) Workspace->DrawFileMenu();
				if (ImGui::BeginMenu("Recent Assets"))
				{
					for (const auto& Target : AssetEditors.GetRecentAssets())
						if (ImGui::MenuItem(Target.AssetPath.ToString().c_str()))
						{
							const auto Opened = AssetEditors.OpenAssets({.AssetEditors = &AssetEditors, .Assets = {Target}, .Origin = "Recent"});
							for (const auto& Item : Opened.Items) if (!Item.IsAccepted())
								if (auto* Editor = AssetEditors.GetEditor()) Editor->GetNotificationManager().Post({.Type = ENotificationType::Warning,
									.Message = "The recent asset could not be opened.", .Details = Item.Target.AssetPath.ToString() + ": " + Item.Message});
						}
					ImGui::EndMenu();
				}
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("Edit"))
			{
				const bool bCanUndo = ActiveWorkspace && ActiveWorkspace->CanUndo();
				const bool bCanRedo = ActiveWorkspace && ActiveWorkspace->CanRedo();
				const auto UndoDescription = bCanUndo ? ActiveWorkspace->GetUndoDescription() : std::string{};
				const auto RedoDescription = bCanRedo ? ActiveWorkspace->GetRedoDescription() : std::string{};
				const std::string UndoLabel = UndoDescription.empty() ? "Undo" : std::format("Undo {}", UndoDescription);
				const std::string RedoLabel = RedoDescription.empty() ? "Redo" : std::format("Redo {}", RedoDescription);
				if (ImGui::MenuItem(UndoLabel.c_str(), "Ctrl+Z", false, bCanUndo)) ActiveWorkspace->Undo();
				if (ImGui::MenuItem(RedoLabel.c_str(), "Ctrl+Y", false, bCanRedo)) ActiveWorkspace->Redo();
				ImGui::Separator();
				if (ImGui::MenuItem("Editor Preferences...")) FEditorSettingsRegistry::Get().RequestOpen("editor.appearance");
				for (const std::shared_ptr<IAssetEditorToolkit>& Workspace : Toolkits) Workspace->DrawEditMenu();
				ImGui::EndMenu();
			}
			DrawToolsMenu(ViewState.bAssetUpgradeOpen);
			if (ImGui::BeginMenu("Window"))
			{
				DrawOpenEditorsMenu(AssetEditors);
				ImGui::Separator();
				ImGui::MenuItem("Content Browser", "Ctrl+Space",
					&ViewState.bContentBrowserOpen);
				ImGui::MenuItem("Console", nullptr, &ViewState.bConsoleOpen);
				ImGui::MenuItem("Python Scripts", nullptr, &ViewState.bPythonOpen);
				if (ImGui::MenuItem("Activity History"))
					ViewState.bActivityHistoryRequested = true;
				if (ActiveWorkspace)
				{
					ImGui::Separator();
					ActiveWorkspace->DrawWindowMenu();
					if (ImGui::MenuItem("Reset Active Editor Layout"))
					{
						ActiveWorkspace->ResetLayout();
						if (auto Base = std::dynamic_pointer_cast<FBaseAssetToolkit>(ActiveWorkspace)) Base->ResetPanels();
					}
				}
				ImGui::Separator();
				if (ImGui::MenuItem("Reset Editor Layout"))
					ViewState.bResetMainFrameLayoutRequested = true;
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("Help"))
			{
				if (ImGui::MenuItem("About Durin...")) ViewState.bAboutDialogOpen = true;
				ImGui::EndMenu();
			}
		}

		auto DrawCustomEditorTitleBar(
			DAssetEditorSubsystem& AssetEditors,
			MWindow& RootWindow,
			const FRHITexture* BrandTexture,
			bool bDrawWorkspaceMenus,
			FMainFrameViewState& ViewState) -> void
		{
			const float BarHeight = MonaImGui::ScaleUI(EditorTitleBarHeight);
			const float ButtonWidth = MonaImGui::ScaleUI(EditorCaptionButtonWidth);
			const float VerticalPadding = std::max(0.0f, (BarHeight - ImGui::GetFontSize()) * 0.5f);
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(MonaImGui::ScaleUI(EditorTitleBarGap), VerticalPadding));
			if (!ImGui::BeginMainMenuBar())
			{
				ImGui::PopStyleVar();
				return;
			}

			ImGuiViewport* Viewport = ImGui::GetMainViewport();
			ImDrawList* DrawList = ImGui::GetWindowDrawList();
			const ImVec2 BarMin = ImGui::GetWindowPos();
			const ImVec2 BarMax(BarMin.x + ImGui::GetWindowWidth(), BarMin.y + ImGui::GetWindowHeight());
			const FWindowTitleBarPlatformMetrics PlatformMetrics = RootWindow.GetTitleBarPlatformMetrics();
			const float NativeControlEndX = BarMin.x + static_cast<float>(PlatformMetrics.NativeControlExclusion.MaxX);
			const float CaptionStartX = PlatformMetrics.bNativeWindowControls
				? BarMax.x : BarMax.x - ButtonWidth * 3.0f;
			if (PlatformMetrics.bNativeWindowControls && ImGui::GetCursorScreenPos().x < NativeControlEndX)
			{
				ImGui::SetCursorScreenPos({NativeControlEndX, ImGui::GetCursorScreenPos().y});
			}
			const float BrandHeight = MonaImGui::ScaleUI(EditorTitleBarBrandHeight);
			const float BrandWidth = BrandHeight;
			const float BrandSlotWidth = MonaImGui::ScaleUI(EditorTitleBarBrandSlotWidth);
			const ImVec2 BrandSlotMin = ImGui::GetCursorScreenPos();
			const ImVec2 BrandMin(
				std::round(BrandSlotMin.x),
				std::round(BarMin.y + (BarMax.y - BarMin.y - BrandHeight) * 0.5f));
			DrawEditorBrandMark(DrawList, BrandTexture, BrandMin, BrandHeight);
			ImGui::Dummy(ImVec2(BrandSlotWidth, 0.0f));
			ImGui::SameLine(0.0f, 0.0f);

			const std::string WindowTitle = RootWindow.GetTitle();
			const float RequiredMenuWidth = bDrawWorkspaceMenus ? MonaImGui::ScaleUI(265.0f) : 0.0f;
			const float MenuGap = MonaImGui::ScaleUI(EditorTitleBarMenuGap);
			const float AvailableTitleWidth = CaptionStartX - ImGui::GetCursorScreenPos().x
				- RequiredMenuWidth - MonaImGui::ScaleUI(56.0f) - MenuGap;
			bool bDrewTitle = false;
			if (AvailableTitleWidth >= MonaImGui::ScaleUI(148.0f))
			{
				const ImVec2 TitleCursor = ImGui::GetCursorScreenPos();
				constexpr std::string_view BrandName = "Durin";
				const bool bHasBrandPrefix = WindowTitle.starts_with(BrandName);
				std::string_view ProjectName = bHasBrandPrefix
					? std::string_view(WindowTitle).substr(BrandName.size()) : std::string_view(WindowTitle);
				if (ProjectName.starts_with(" - ")) ProjectName.remove_prefix(3);
				else
				{
					while (!ProjectName.empty() && (ProjectName.front() == ' ' || ProjectName.front() == '-'))
						ProjectName.remove_prefix(1);
				}
				ImFont* BrandFont = MonaImGui::GetMediumUIFont();
				ImFont* BodyFont = ImGui::GetFont();
				const float FontSize = ImGui::GetFontSize();
				const float BrandFontSize = MonaImGui::QuantizeDynamicFontSize(FontSize * 0.95f);
				const float ProjectFontSize = MonaImGui::QuantizeDynamicFontSize(FontSize * 0.9f);
				const ImVec2 TitlePosition(
					TitleCursor.x,
					std::round(BarMin.y + (BarMax.y - BarMin.y - FontSize) * 0.5f));
				const float BrandBaselineOffset = BodyFont->GetFontBaked(FontSize)->Ascent
					- BrandFont->GetFontBaked(BrandFontSize)->Ascent;
				const float ProjectBaselineOffset = BodyFont->GetFontBaked(FontSize)->Ascent
					- BodyFont->GetFontBaked(ProjectFontSize)->Ascent;
				const float BrandWidth = BrandFont->CalcTextSizeA(
					BrandFontSize, FLT_MAX, 0.0f, BrandName.data(), BrandName.data() + BrandName.size()).x;
				const float TitleGap = MonaImGui::ScaleUI(EditorTitleBarTitleGap);
				const float ProjectOffset = BrandWidth + TitleGap * 2.0f + 1.0f;
				const float ProjectWidth = ProjectName.empty() ? 0.0f : BodyFont->CalcTextSizeA(
					ProjectFontSize, FLT_MAX, 0.0f, ProjectName.data(), ProjectName.data() + ProjectName.size()).x;
				const float TitleWidth = ProjectName.empty() ? BrandWidth : ProjectOffset + ProjectWidth;
				const float DrawWidth = std::min(TitleWidth, AvailableTitleWidth);
				const ImVec4 ClipRect(
					TitlePosition.x, BarMin.y, TitlePosition.x + DrawWidth, BarMax.y);
				const ImU32 TitleColor = ImGui::GetColorU32(ImGuiCol_Text);
				DrawList->AddText(
					BrandFont, BrandFontSize, TitlePosition + ImVec2(0.0f, BrandBaselineOffset), TitleColor,
					BrandName.data(), BrandName.data() + BrandName.size(), 0.0f, &ClipRect);
				if (!ProjectName.empty())
				{
					const float SeparatorX = TitlePosition.x + BrandWidth + TitleGap;
					if (SeparatorX < ClipRect.z)
					{
						const float SeparatorHalfHeight = MonaImGui::ScaleUI(8.0f);
						const float SeparatorCenterY = std::round((BarMin.y + BarMax.y) * 0.5f);
						DrawList->AddLine(
							{SeparatorX, SeparatorCenterY - SeparatorHalfHeight},
							{SeparatorX, SeparatorCenterY + SeparatorHalfHeight},
							ImGui::GetColorU32(ImGuiCol_Separator));
					}
					DrawList->AddText(
						BodyFont, ProjectFontSize,
						TitlePosition + ImVec2(ProjectOffset, ProjectBaselineOffset),
						ImGui::GetColorU32(ImGuiCol_TextDisabled), ProjectName.data(),
						ProjectName.data() + ProjectName.size(), 0.0f, &ClipRect);
				}
				ImGui::Dummy(ImVec2(DrawWidth, 0.0f));
				ImGui::SameLine(0.0f, 0.0f);
				bDrewTitle = true;
			}
			if (bDrewTitle)
			{
				ImGui::Dummy(ImVec2(MenuGap, 0.0f));
				ImGui::SameLine(0.0f, 0.0f);
			}

			const float FirstMenuX = ImGui::GetCursorScreenPos().x;
			std::shared_ptr<IAssetEditorToolkit> ActiveWorkspace;
			ActiveWorkspace = AssetEditors.GetActiveAssetToolkit();
			if (bDrawWorkspaceMenus)
			{
				DrawWorkspaceMenus(
					AssetEditors, ActiveWorkspace, ViewState);
			}
			const float MenuEndX = ImGui::GetCursorScreenPos().x;

			const FWindowTitleBarInteractionState Interaction = RootWindow.GetTitleBarInteractionState();
			const ImU32 TextColor = ImGui::GetColorU32(
				Interaction.bFocused ? ImGuiCol_Text : ImGuiCol_TextDisabled);
			const auto DrawCaptionButton = [&](int32 Index, EWindowTitleBarHitTest Part) {
				const ImVec2 Min(CaptionStartX + ButtonWidth * static_cast<float>(Index), BarMin.y);
				const ImVec2 Max(Min.x + ButtonWidth, BarMax.y);
				const bool bHovered = Interaction.HoveredPart == Part;
				const bool bPressed = Interaction.PressedPart == Part;
				if (bHovered || bPressed)
				{
					const ImU32 Background = Part == EWindowTitleBarHitTest::Close
						? MonaImGui::GetThemeColorU32(MonaImGui::EUIThemeColor::Error)
						: ImGui::GetColorU32(bPressed ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered);
					DrawList->AddRectFilled(Min, Max, Background);
				}
				const ImVec2 Center((Min.x + Max.x) * 0.5f, (Min.y + Max.y) * 0.5f);
				const float Radius = MonaImGui::ScaleUI(5.0f);
				if (Part == EWindowTitleBarHitTest::Minimize)
					DrawList->AddLine({Center.x - Radius, Center.y + Radius * 0.45f}, {Center.x + Radius, Center.y + Radius * 0.45f}, TextColor, 1.0f);
				else if (Part == EWindowTitleBarHitTest::Maximize)
				{
					if (Interaction.bMaximized)
					{
						DrawList->AddRect({Center.x - Radius + 2.0f, Center.y - Radius}, {Center.x + Radius, Center.y + Radius - 2.0f}, TextColor);
						DrawList->AddRect({Center.x - Radius, Center.y - Radius + 2.0f}, {Center.x + Radius - 2.0f, Center.y + Radius}, TextColor);
					}
					else DrawList->AddRect({Center.x - Radius, Center.y - Radius}, {Center.x + Radius, Center.y + Radius}, TextColor);
				}
				else
				{
					DrawList->AddLine({Center.x - Radius, Center.y - Radius}, {Center.x + Radius, Center.y + Radius}, TextColor, 1.0f);
					DrawList->AddLine({Center.x + Radius, Center.y - Radius}, {Center.x - Radius, Center.y + Radius}, TextColor, 1.0f);
				}
			};
			if (!PlatformMetrics.bNativeWindowControls)
			{
				DrawCaptionButton(0, EWindowTitleBarHitTest::Minimize);
				DrawCaptionButton(1, EWindowTitleBarHitTest::Maximize);
				DrawCaptionButton(2, EWindowTitleBarHitTest::Close);
			}
			DrawList->AddLine(
				{BarMin.x, BarMax.y - 1.0f}, {BarMax.x, BarMax.y - 1.0f},
				ImGui::GetColorU32(ImGuiCol_Separator));

			static uint64 LayoutGeneration = 1;
			const auto ToClientX = [&](float ScreenX) { return static_cast<int32>(std::round(ScreenX - Viewport->Pos.x)); };
			const int32 ClientHeight = static_cast<int32>(std::round(BarMax.y - Viewport->Pos.y));
			FWindowTitleBarLayout Layout;
			Layout.Generation = ++LayoutGeneration;
			Layout.bValid = true;
			Layout.Height = ClientHeight;
			Layout.MinimumWindowWidth = static_cast<int32>(std::ceil(MonaImGui::ScaleUI(640.0f)));
			const int32 LeadingDragX = PlatformMetrics.bNativeWindowControls
				? PlatformMetrics.NativeControlExclusion.MaxX : 0;
			if (ToClientX(FirstMenuX) > LeadingDragX)
				Layout.DragRegions.push_back({LeadingDragX, 0, ToClientX(FirstMenuX), ClientHeight});
			if (CaptionStartX > MenuEndX) Layout.DragRegions.push_back({ToClientX(MenuEndX), 0, ToClientX(CaptionStartX), ClientHeight});
			if (!PlatformMetrics.bNativeWindowControls)
			{
				Layout.MinimizeRegion = {ToClientX(CaptionStartX), 0, ToClientX(CaptionStartX + ButtonWidth), ClientHeight};
				Layout.MaximizeRegion = {ToClientX(CaptionStartX + ButtonWidth), 0, ToClientX(CaptionStartX + ButtonWidth * 2.0f), ClientHeight};
				Layout.CloseRegion = {ToClientX(CaptionStartX + ButtonWidth * 2.0f), 0, ToClientX(BarMax.x), ClientHeight};
			}
			RootWindow.PublishTitleBarLayout(Layout);

			ImGui::EndMainMenuBar();
			ImGui::PopStyleVar();
		}

		auto DrawMainFrame(
			DAssetEditorSubsystem& AssetEditors,
			FMainFrameSettings& MainFrameSettings,
			MWindow& RootWindow,
			FMainFrameViewState& ViewState,
			FAssetCompatibilityWindow& AssetCompatibilityWindow,
			ContentBrowser::IContentBrowserTool& ContentBrowserTool,
			FConsolePanel& Console,
			FPythonScriptPanel& PythonScripts,
			FEditorNotificationOverlay& Activity
		) -> void
		{
			ImGuiViewport* Viewport = ImGui::GetMainViewport();
			ImGui::SetNextWindowPos(Viewport->WorkPos);
			ImGui::SetNextWindowSize(Viewport->WorkSize);
			ImGui::SetNextWindowViewport(Viewport->ID);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
			const bool bCustomTitleBar = RootWindow.GetEffectiveWindowDecorationMode() == EWindowDecorationMode::CustomTitleBar;
			const ImGuiWindowFlags MainFrameFlags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
				ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
				ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
				(bCustomTitleBar ? ImGuiWindowFlags_None : ImGuiWindowFlags_MenuBar);
			ImGui::Begin("###Durin.Editor.WorkspaceHost", nullptr, MainFrameFlags);
			ImGui::PopStyleVar(3);

			std::shared_ptr<IAssetEditorToolkit> ActiveWorkspace;
			ActiveWorkspace = AssetEditors.GetActiveAssetToolkit();
			const ImGuiIO& IO = ImGui::GetIO();
			if (ActiveWorkspace && IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
				ActiveWorkspace->SaveActiveDocument();
			if (IO.KeyCtrl && !IO.WantTextInput
				&& ImGui::IsKeyPressed(ImGuiKey_Space, false))
			{
				if (ViewState.bContentBrowserOpen)
				{
					ImGui::SetWindowFocus(
						"Content Browser###Durin.EditorHost.ContentBrowser");
					ContentBrowserTool.RequestFocus();
				}
				else if (ViewState.Drawer.IsOpen()
					&& ViewState.DrawerTool == EMainFrameDrawerTool::ContentBrowser)
					ViewState.Drawer.Close();
				else
				{
					ViewState.DrawerTool = EMainFrameDrawerTool::ContentBrowser;
					ViewState.Drawer.Open();
					ContentBrowserTool.RequestFocus();
				}
			}
			if (ActiveWorkspace && IO.KeyCtrl && !IO.WantTextInput)
			{
				if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) ActiveWorkspace->Undo();
				if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) ActiveWorkspace->Redo();
			}
			if (!bCustomTitleBar && ImGui::BeginMenuBar())
			{
				DrawWorkspaceMenus(
					AssetEditors, ActiveWorkspace, ViewState);
				ImGui::EndMenuBar();
			}
			DrawAboutDialog(ViewState.bAboutDialogOpen);
			ViewState.SettingsWindow.Draw(MainFrameSettings, RootWindow);
			AssetCompatibilityWindow.Draw(ViewState.bAssetUpgradeOpen,
				[&ContentBrowserTool, &ViewState](const FPackagePath& Path) {
					ViewState.bContentBrowserOpen = true;
					(void)ContentBrowserTool.RevealAsset(Path.ToString());
					(void)ContentBrowserTool.RequestFocus();
				});
			if (ViewState.bActivityHistoryRequested)
			{
				Activity.OpenHistory();
				ViewState.bActivityHistoryRequested = false;
			}

			ImVec2 DockSpaceSize = ImGui::GetContentRegionAvail();
			if (GEditor)
				DockSpaceSize.y = std::max(
					0.0f, DockSpaceSize.y - Activity.GetStatusBarHeight());
			const ImVec2 DrawerAnchorMin = ImGui::GetCursorScreenPos();
			const ImVec2 DrawerAnchorMax = DrawerAnchorMin + DockSpaceSize;
			const ImGuiID DockSpaceId = TabLayout::MakeMainFrameDockSpaceId(TabLayout::MainFrameLayoutVersion);
			const bool bNeedsDefaultLayout = ImGui::DockBuilderGetNode(DockSpaceId) == nullptr;
			if (bNeedsDefaultLayout || ViewState.bResetMainFrameLayoutRequested)
			{
				// DockBuilder must finish before DockSpace submission so the new tree retains this frame's host window.
				BuildDefaultMainFrameLayout(DockSpaceId, DockSpaceSize);
				ViewState.bContentBrowserOpen = false;
				ViewState.bConsoleOpen = false;
				ViewState.Drawer.Reset();
				ViewState.DrawerTool = EMainFrameDrawerTool::ContentBrowser;
				ViewState.bResetMainFrameLayoutRequested = false;
			}
			const ImVec2 MainFrameItemSpacing(
				ImGui::GetStyle().ItemSpacing.x, 0.0f);
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, MainFrameItemSpacing);
			TabLayout::SubmitMainFrameDockSpace(TabLayout::MainFrameLayoutVersion,
				DockSpaceSize, ImGuiDockNodeFlags_NoWindowMenuButton);
			ImGui::PopStyleVar();

			bool bBrowserSubmitted = false;
			bool bConsoleSubmitted = false;
			const bool bAllowAssetMutation = !GEditor || (GEditor->GetCapabilities().bEditWorld && GEditor->CanTransact());
			if (ViewState.bContentBrowserOpen)
			{
				const bool bVisible = ImGui::Begin(
					"Content Browser###Durin.EditorHost.ContentBrowser",
					&ViewState.bContentBrowserOpen);
				if (bVisible)
				{
					ContentBrowserTool.DrawContents(bAllowAssetMutation);
					bBrowserSubmitted = true;
				}
				ImGui::End();
			}

			if (ViewState.bConsoleOpen)
			{
				const bool bVisible = ImGui::Begin(
					"Console###Durin.EditorHost.Console", &ViewState.bConsoleOpen);
				if (bVisible)
				{
					Console.DrawContents();
					bConsoleSubmitted = true;
				}
				ImGui::End();
			}

			if (GEditor)
			{
				EEditorStatusBarAction Selected = EEditorStatusBarAction::None;
				if (ViewState.Drawer.IsOpen())
					Selected = ViewState.DrawerTool == EMainFrameDrawerTool::Console
						? EEditorStatusBarAction::Console
						: EEditorStatusBarAction::ContentBrowser;
				const EEditorStatusBarAction Action = Activity.DrawStatusBar(
					GEditor->GetNotificationManager(), Selected,
					Console.GetUnreadImportantRecordCount());
				const auto ToggleDrawer = [&](EMainFrameDrawerTool Tool) {
					const bool bDocked = Tool == EMainFrameDrawerTool::Console
						? ViewState.bConsoleOpen : ViewState.bContentBrowserOpen;
					if (bDocked)
					{
						ImGui::SetWindowFocus(Tool == EMainFrameDrawerTool::Console
							? "Console###Durin.EditorHost.Console"
							: "Content Browser###Durin.EditorHost.ContentBrowser");
						if (Tool == EMainFrameDrawerTool::Console) Console.RequestInputFocus();
						else ContentBrowserTool.RequestFocus();
						return;
					}
					if (ViewState.Drawer.IsOpen() && ViewState.DrawerTool == Tool)
						ViewState.Drawer.Close();
					else
					{
						ViewState.DrawerTool = Tool;
						ViewState.Drawer.Open();
						if (Tool == EMainFrameDrawerTool::Console)
						{
							Console.RequestInputFocus();
							Console.RequestScrollToLatest();
						}
						else ContentBrowserTool.RequestFocus();
					}
				};
				switch (Action)
				{
				case EEditorStatusBarAction::ContentBrowser:
					ToggleDrawer(EMainFrameDrawerTool::ContentBrowser); break;
				case EEditorStatusBarAction::Console:
					ToggleDrawer(EMainFrameDrawerTool::Console); break;
				case EEditorStatusBarAction::ActivityHistory:
					Activity.OpenHistory(); break;
				case EEditorStatusBarAction::None: break;
				}
			}

			if (ViewState.Drawer.IsOpen() || ViewState.Drawer.IsVisible())
			{
				const MonaImGui::FBottomDrawerConfig Config{
					.Id = "Editor Bottom Drawer###Durin.EditorHost.BottomDrawer",
					.AnchorMin = DrawerAnchorMin,
					.AnchorMax = DrawerAnchorMax,
					.bDismissOnFocusLoss = true,
					.bDismissWhenDragLeavesBounds = true,
				};
				const ImVec2 DrawerPadding(
					ImGui::GetStyle().WindowPadding.x, 0.0f);
				ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, DrawerPadding);
				const bool bDrawerVisible = MonaImGui::BeginBottomDrawer(Config, ViewState.Drawer);
				ImGui::PopStyleVar();
				if (bDrawerVisible)
				{
					const bool bConsole = ViewState.DrawerTool == EMainFrameDrawerTool::Console;
					ImGui::AlignTextToFramePadding();
					ImGui::TextUnformatted(bConsole ? "Console" : "Content Browser");
					const char* DockLabel = "Dock in Layout";
					const float DockButtonWidth = ImGui::CalcTextSize(DockLabel).x
						+ ImGui::GetStyle().FramePadding.x * 2.0f;
					const float CloseButtonWidth = ImGui::GetFrameHeight();
					ImGui::SameLine(std::max(ImGui::GetCursorPosX(),
						ImGui::GetWindowContentRegionMax().x - DockButtonWidth
							- ImGui::GetStyle().ItemSpacing.x - CloseButtonWidth));
					const bool bDock = ImGui::Button(DockLabel);
					ImGui::SameLine();
					const bool bClose = ImGui::Button(
						Icons::Close, ImVec2(CloseButtonWidth, 0.0f));
					if (ImGui::IsItemHovered()) ImGui::SetTooltip("Close drawer");
					ImGui::Separator();
					const ImVec2 DrawerWindowPos = ImGui::GetWindowPos();
					const ImVec2 DrawerWindowSize = ImGui::GetWindowSize();
					const ImVec2 ContentPos = ImGui::GetCursorScreenPos();
					const float ContentHeight = std::max(
						0.0f, DrawerWindowPos.y + DrawerWindowSize.y - ContentPos.y);
					const bool bDrawContent = ImGui::BeginChild(
						"##EditorBottomDrawerContent", ImVec2(0.0f, ContentHeight),
						ImGuiChildFlags_None,
						ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
					if (bDrawContent && bConsole && !bConsoleSubmitted)
					{
						Console.DrawContents();
						bConsoleSubmitted = true;
					}
					else if (bDrawContent && !bConsole && !bBrowserSubmitted)
					{
						ContentBrowserTool.DrawContents(bAllowAssetMutation);
						bBrowserSubmitted = true;
					}
					ImGui::EndChild();
					MonaImGui::EndBottomDrawer(ViewState.Drawer);
					if (bClose) ViewState.Drawer.Close();
					if (bDock)
					{
						if (bConsole) ViewState.bConsoleOpen = true;
						else ViewState.bContentBrowserOpen = true;
						ViewState.Drawer.Reset();
					}
				}
			}
			if (!bBrowserSubmitted) ContentBrowserTool.TickWhenHidden();
			ContentBrowserTool.DrawHostPresenters(bAllowAssetMutation);

			{
				const auto Sessions = AssetEditors.GetOwnedAssetEditors();
				std::vector<FEditorDocumentManager*> CloseControllers;
				std::vector<std::shared_ptr<FEditorDocumentManager>> ControllerLeases;
				for (const auto& Session : Sessions)
				{
					const auto Toolkit = Session->GetToolkit();
					const auto Documents = Session->GetDocumentManager();
					if (!Toolkit || !Documents) continue;
					if (Session->DrawToolkitFrame(Toolkit == ActiveWorkspace)) AssetEditors.ActivateAssetEditor(*Session);
					CloseControllers.push_back(Documents.get());
					ControllerLeases.push_back(Documents);
				}
				if (auto* Pending = DocumentUI::FindPendingClose(CloseControllers)) DocumentUI::DrawCloseConfirmation(*Pending);
				AssetEditors.TickAssetEditors();
			}
			Activity.DrawHistoryWindow();
			PythonScripts.Draw(ViewState.bPythonOpen);
			if (GEditor)
				Activity.DrawToasts(GEditor->GetNotificationManager());

			ImGui::End();
		}
	}

} // namespace Durin::Editor::MainFrame

namespace Durin
{
	using namespace Editor::MainFrame;

	IMPLEMENT_MODULE(FMainFrameModule, MainFrame)

	struct Editor::MainFrame::FMainFrameToolsetState
	{
		std::unique_ptr<FNativeToolset> Tracy;
		FModularFeatureRegistration TracyRegistration;
		std::unique_ptr<FNativeToolset> NamePool;
		FModularFeatureRegistration NamePoolRegistration;
	};

	auto FMainFrameModule::StartupModule() -> void
	{
		Toolsets = std::make_shared<FMainFrameToolsetState>();
		Toolsets->Tracy = CreateTracyToolset(std::make_shared<FProfilingToolService>(FPaths::RootDir()));
		Toolsets->TracyRegistration = FModularFeatureRegistry::Get().RegisterFeature(*Toolsets->Tracy);
		Toolsets->NamePool = CreateNamePoolToolset();
		Toolsets->NamePoolRegistration = FModularFeatureRegistry::Get().RegisterFeature(*Toolsets->NamePool);
	}

	auto FMainFrameModule::ShutdownModule() -> void
	{
		DestroyMainFrame();
		Toolsets->NamePoolRegistration.Reset();
		Toolsets->TracyRegistration.Reset();
		Toolsets.reset();
		if (GEditor)
			checkf(GEditor->GetTransactor()
				->DiscardCustomChangesByModule("ContentBrowser"),
				"ContentBrowser cannot retire while one of its custom changes is active");
	}

	auto FMainFrameModule::CreateDefaultMainFrame(
		DEditorEngine& Editor, std::shared_ptr<MWindow> StartupWindow) -> void
	{
		check(MainFrameContext == nullptr);
		check(StartupWindow != nullptr);
		check(StartupWindow->GetNativeWindow() != nullptr);
		// Reserve a small entry-storage floor before workspace bootstrap creates names.
		ReserveNamePoolBlocks(4);
		MainFrameContext = std::make_shared<FMainFrameContext>();
		FMainFrameContext& Context = *MainFrameContext;
		Context.Editor = &Editor;
		Context.bHasProject = HasCurrentProject();
		Context.MainFrameSettings = std::make_shared<FMainFrameSettings>();
		Context.MainFrameSettings->Load();
		ApplyDisplaySettings(*Context.MainFrameSettings, &Editor);
		MonaImGui::SetColorTheme(Context.MainFrameSettings->GetColorTheme());
		MonaImGui::SetGlobalUIScale(Context.MainFrameSettings->GetUIScale());
		Context.RootWindow = std::move(StartupWindow);
		MonaImGui::BindMainViewportToWindow(Context.RootWindow);
		Context.AssetEditors = Editor.GetEditorSubsystem<DAssetEditorSubsystem>();
		check(Context.AssetEditors.IsValid());
		Context.AssetEditors.Get()->SetToolkitHostFactory([Weak = std::weak_ptr<FMainFrameContext>(MainFrameContext)](const FAssetOpenArgs& Args) {
			const auto Owner = Weak.lock();
			if (!Owner) return std::shared_ptr<IToolkitHost>{};
			if (Args.ToolkitMode == EToolkitMode::WorldCentric)
			{
				if (!Owner->WorldHost) Owner->WorldHost = std::make_shared<FMainFrameToolkitHost>(Weak, Args.ToolkitMode);
				return Owner->WorldHost;
			}
			return std::shared_ptr<IToolkitHost>(std::make_shared<FMainFrameToolkitHost>(Weak, Args.ToolkitMode));
		});
		Context.ProjectBrowser = std::make_shared<FProjectBrowser>();
		Context.BrandTexture = std::make_unique<FEditorBrandTexture>();
		std::string BrandTextureError;
		if (!Context.BrandTexture->Load(BrandTextureError))
			DURIN_WARN("Could not load the editor branding texture: {}", BrandTextureError);
		Context.AssetCompatibilityWindow =
			std::make_shared<FAssetCompatibilityWindow>();
		Context.Console = std::make_unique<FConsolePanel>(Editor);
		Context.PythonScripts = std::make_unique<FPythonScriptPanel>(Editor);
		Context.Activity = std::make_unique<FEditorNotificationOverlay>();

		const FIntPoint WindowSize{
			Context.MainFrameSettings->GetWindowWidth(),
			Context.MainFrameSettings->GetWindowHeight()};
		Context.RootWindow->SetTitle(GetCurrentProject()
			? std::format("Durin - {}", GetCurrentProject()->Name)
			: "Durin - Project Browser");
		Context.RootWindow->ReshapeWindow(
			{100.0f, 100.0f},
			{static_cast<float>(WindowSize.x), static_cast<float>(WindowSize.y)});

		const std::weak_ptr<FMainFrameContext> WeakContext =
			MainFrameContext;
		Context.RootWindow->SetCloseRequestHandler([WeakContext] {
			if (const auto Context = WeakContext.lock())
			{
				// An existing document confirmation retains ownership of its response.
				const auto* Service = Context->AssetEditors.Get();
				const auto Sessions = Service ? Service->GetOwnedAssetEditors() : std::vector<TStrongObjectPtr<DAssetEditor>>{};
				if (std::ranges::none_of(Sessions, [](const auto& Session) {
					const auto Documents = Session->GetDocumentManager();
					return Documents && Documents->GetPendingCloseDocument();
				}))
					Context->bExitRequested = true;
			}
		});
		Context.ProjectBrowser->SetOpenProject([WeakContext](
			std::string_view ProjectFile, std::string& OutError) {
			const std::shared_ptr<FMainFrameContext> Context =
				WeakContext.lock();
			if (!Context)
			{
				OutError = "The editor main frame is unavailable.";
				return false;
			}
			const FProjectInfo* CurrentProject = GetCurrentProject();
			if (CurrentProject
				&& CurrentProject->ProjectFile == NormalizeProjectFile(ProjectFile))
			{
				Context->bProjectBrowserOpen = false;
				return true;
			}
			if (Context->LevelEditorModule)
			{
				if (!Context->LevelEditorModule->RequestOpenProject(
					*Context->AssetEditors.Get(), 					std::string(ProjectFile)))
				{
					OutError = "The Level Editor workspace is unavailable.";
					return false;
				}
				Context->bProjectBrowserOpen = false;
				return true;
			}
			const auto RelaunchEditorForProjectResult = RelaunchEditorForProject(ProjectFile);
			OutError = RelaunchEditorForProjectResult ? std::string{} : RelaunchEditorForProjectResult.error().ToString();
			return RelaunchEditorForProjectResult.has_value();
		});
		Context.ProjectBrowser->SetClose([WeakContext] {
			if (const std::shared_ptr<FMainFrameContext> Context =
					WeakContext.lock())
				Context->bProjectBrowserOpen = false;
		});

		auto EditorRootWidget = std::make_shared<MFunctionWidget>();
		EditorRootWidget->Construct([WeakContext,
			ViewState = FMainFrameViewState{}]() mutable {
			auto* Editor = GEditor;
			if (!Editor) return;
			Editor->DispatchSubsystemCallback([&] {
				const std::shared_ptr<FMainFrameContext> Context =
					WeakContext.lock();
				if (!Context) return;
				const bool bReadyWorkspace = Editor->GetCapabilities().bPresentWorkspace
					&& Context->bHasProject && Context->LevelEditorModule
					&& Context->ContentBrowserTool;
				const FRHITexture* BrandTexture = Context->BrandTexture->UpdateAndGetTexture();
				if (Context->RootWindow->GetEffectiveWindowDecorationMode() == EWindowDecorationMode::CustomTitleBar)
				{
					DrawCustomEditorTitleBar(
						*Context->AssetEditors.Get(),
						*Context->RootWindow,
						BrandTexture,
						bReadyWorkspace && !Context->bProjectBrowserOpen,
						ViewState);
				}
				if (bReadyWorkspace && !Context->bProjectBrowserOpen)
				{
					auto& ThumbnailPool = Editor::GetDefaultThumbnailManager().GetSharedPool();
					ThumbnailPool.BeginFrame();
					DrawMainFrame(
						*Context->AssetEditors.Get(),
						*Context->MainFrameSettings,
						*Context->RootWindow,
						ViewState,
						*Context->AssetCompatibilityWindow,
						*Context->ContentBrowserTool,
						*Context->Console,
						*Context->PythonScripts,
						*Context->Activity);
					ThumbnailPool.EndFrame(Editor->GetCapabilities().bRunTools);
					Context->SlowTaskDialogs.Draw(FFeedbackContext::Get());
					DrawExitConfirmation(*Context);
					return;
				}
				if (Context->bProjectBrowserOpen || !Context->bHasProject
					|| Editor->GetStartupPhase() == EBootstrapState::Failed)
				{
					Context->ProjectBrowser->Draw(
						BrandTexture, bReadyWorkspace && Context->bProjectBrowserOpen, Editor->GetStartupError());
					Context->SlowTaskDialogs.Draw(FFeedbackContext::Get());
					DrawExitConfirmation(*Context);
					return;
				}
				DrawLoadingState(*Editor);
				Context->SlowTaskDialogs.Draw(FFeedbackContext::Get());
				DrawExitConfirmation(*Context);
			});
		});
		Context.RootWindow->SetContent(EditorRootWidget);

		// Keep the native window hidden until its persisted display state has been
		// applied. Showing it before maximizing causes a visible normal-size frame
		// during editor startup.
		if (Context.MainFrameSettings->IsWindowMaximized())
		{
			Context.RootWindow->MaximizeWindow();
		}
		{
			DURIN_PROFILE_CPU_ZONE_NAMED("Startup.NativeViewport");
			Mona::FMonaApplication::Get().GetRenderer()->CreateViewport(
				Context.RootWindow);
		}
		Profiling::RecordStartupMilestone(Profiling::EStartupMilestone::NativeViewportReady);
		Context.ShellViewport = Mona::FMonaApplication::Get().GetRenderer()->PrepareViewportForDraw(*Context.RootWindow);
		Context.ShellPresentationBaseline = Context.ShellViewport ? Context.ShellViewport->GetPresentationSerial() : 0;
		Profiling::ArmEditorShellFirstPresent();
		Context.RootWindow->ShowWindow();
	}

	auto FMainFrameModule::DestroyMainFrame() -> void
	{
		if (!MainFrameContext) return;
		MainFrameContext.reset();
	}

	auto FMainFrameModule::IsShellPresented() const -> bool
	{
		return MainFrameContext && MainFrameContext->ShellViewport
			&& MainFrameContext->ShellViewport->GetPresentationSerial() > MainFrameContext->ShellPresentationBaseline;
	}

	auto FMainFrameModule::LoadWorkspaceModules() -> bool
	{
		Profiling::RecordStartupMilestone(Profiling::EStartupMilestone::WorkspaceRegistrationBegin);
		DURIN_PROFILE_CPU_ZONE_NAMED("Startup.WorkspaceModules");
		for (const auto* Name : {"LevelEditor", "MaterialEditor", "TextureEditor", "StaticMeshEditor"})
			if (!FModuleManager::Get().LoadModule(Name))
			{
				DURIN_ERROR("Workspace module '{}' could not start.", Name);
				return false;
			}
		return true;
	}

	auto FMainFrameModule::InitializeWorkspaces(std::string& OutError) -> bool
	{
		OutError.clear();
		check(MainFrameContext);
		return InitializeEditorWorkspaces(*MainFrameContext, OutError);
	}

	auto FMainFrameModule::TickMainFrame(float DeltaSeconds) -> void
	{
		const auto Context = MainFrameContext;
		if (!Context) return;
		auto* Editor = Context->Editor.Get();
		if (!Editor) return;
		ObserveMainWindowState(*Context->MainFrameSettings, *Context->RootWindow);
		Context->AssetCompatibilityWindow->Tick(FPaths::ProjectFile(), Editor->GetCapabilities().bEditWorld && Editor->CanTransact());
		if (auto* Service = Context->AssetEditors.Get()) Service->TickAssetEditors();
		Context->PythonScripts->Tick();
		Context->Console->Tick();
		Context->Activity->Tick(Editor->GetNotificationManager(), *Editor->GetTransactor());
	}

} // namespace Durin
