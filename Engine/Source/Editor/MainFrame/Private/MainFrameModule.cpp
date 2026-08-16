#include "MainFrameModule.h"
#include "AssetCompatibilityWindow.h"

#include "EditorBranding.h"
#include "ProjectBrowser.h"
#include "ProfilingToolService.h"

#include "Editor/WorkspaceManager.h"
#include "Editor/WorkspaceUI.h"
#include "Editor/EditorEngine.h"
#include "Mona.h"
#include "LevelEditorModule.h"
#include "MaterialEditorModule.h"
#include "TextureEditorModule.h"
#include "AssetBuild/BuildHost.h"
#include "StaticMeshEditorModule.h"
#include "SkeletalMeshEditorModule.h"
#include "Thumbnail/RenderedAssetThumbnailService.h"
#include "MonaImGui.h"
#include "Misc/Paths.h"
#include "Misc/Project.h"
#include "Misc/Version.h"
#include "Profiling/Profiling.h"
#include "Settings/HostSettings.h"

#include "Widgets/MFunctionWidget.h"
#include "Widgets/MWindow.h"

namespace Durin::Editor::MainFrame
{
	struct FBootstrapContext
	{
		EBootstrapState State = EBootstrapState::ConstructingShell;
		EDefaultDocumentState DefaultDocumentState =
			EDefaultDocumentState::NotApplicable;
		bool bHasProject = false;
		bool bWorkspaceActivationStarted = false;
		std::string FailureMessage;
		std::shared_ptr<FHostSettings> HostSettings;
		std::shared_ptr<MWindow> RootWindow;
		std::shared_ptr<Editor::FWorkspaceManager> WorkspaceManager;
		std::shared_ptr<FProjectBrowser> ProjectBrowser;
		std::unique_ptr<FEditorBrandTexture> BrandTexture;
		std::shared_ptr<FProfilingToolService> ProfilingTools;
		std::shared_ptr<FAssetCompatibilityWindow> AssetCompatibilityWindow;
		::Durin::FLevelEditorModule* LevelEditorModule = nullptr;
	};

	namespace
	{
		constexpr float EditorTitleBarHeight = 36.0f;
		constexpr float EditorTitleBarBrandHeight = 20.0f;
		constexpr float EditorTitleBarBrandSlotWidth = 24.0f;
		constexpr float EditorCaptionButtonWidth = 46.0f;
		constexpr float EditorTitleBarGap = 8.0f;
		constexpr float EditorTitleBarMenuGap = 20.0f;

		auto TransitionBootstrap(
			FBootstrapContext& Context,
			EBootstrapState NextState) -> void
		{
			check(IsValidBootstrapTransition(Context.State, NextState));
			Context.State = NextState;
		}

		auto RegisterEditorWorkspaces(
			Editor::FWorkspaceManager& WorkspaceManager,
			::Durin::FLevelEditorModule& LevelEditorModule,
			::Durin::FMaterialEditorModule& MaterialEditorModule,
			::Durin::FTextureEditorModule& TextureEditorModule,
			::Durin::FStaticMeshEditorModule& StaticMeshEditorModule,
			::Durin::FSkeletalMeshEditorModule& SkeletalMeshEditorModule,
			Editor::FRenderedAssetThumbnailService& ThumbnailService
		) -> bool
		{
			if (!LevelEditorModule.RegisterLevelEditorWorkspace(WorkspaceManager, ThumbnailService)) return false;
			if (!MaterialEditorModule.RegisterMaterialEditor(
				WorkspaceManager, ThumbnailService))
			{
				LevelEditorModule.UnregisterLevelEditorWorkspace();
				return false;
			}
			if (!TextureEditorModule.RegisterTextureEditor(
				WorkspaceManager, ThumbnailService))
			{
				MaterialEditorModule.UnregisterMaterialEditor();
				LevelEditorModule.UnregisterLevelEditorWorkspace();
				return false;
			}
			if (!StaticMeshEditorModule.RegisterStaticMeshEditor(
				WorkspaceManager, ThumbnailService))
			{
				TextureEditorModule.UnregisterTextureEditor();
				MaterialEditorModule.UnregisterMaterialEditor();
				LevelEditorModule.UnregisterLevelEditorWorkspace();
				return false;
			}
			if (!SkeletalMeshEditorModule.RegisterSkeletalMeshEditor(
				WorkspaceManager, ThumbnailService))
			{
				StaticMeshEditorModule.UnregisterStaticMeshEditor();
				TextureEditorModule.UnregisterTextureEditor();
				MaterialEditorModule.UnregisterMaterialEditor();
				LevelEditorModule.UnregisterLevelEditorWorkspace();
				return false;
			}
			if (WorkspaceManager.OpenDefaultWorkspaces()) return true;

			// Feature modules own their handles, but the host coordinates this multi-module
			// startup so a failed default document cannot leave a partial editor behind.
			SkeletalMeshEditorModule.UnregisterSkeletalMeshEditor();
			StaticMeshEditorModule.UnregisterStaticMeshEditor();
			TextureEditorModule.UnregisterTextureEditor();
			MaterialEditorModule.UnregisterMaterialEditor();
			LevelEditorModule.UnregisterLevelEditorWorkspace();
			return false;
		}

		auto ActivateEditorWorkspaces(
			FBootstrapContext& Context) -> bool
		{
			Profiling::RecordStartupMilestone(
				Profiling::EStartupMilestone::WorkspaceRegistrationBegin);
			bool bWorkspaceReady = false;
			{
				DURIN_PROFILE_CPU_ZONE_NAMED("Startup.WorkspaceRegistration");
				FModuleManager::Get().LoadModuleChecked("AssetBuildCore");
				FModuleManager::Get().LoadModuleChecked("TextureBuild");
				FModuleManager::Get().LoadModuleChecked("GeometryBuild");
				checkf(Asset::Build::InitializeBuildHost(),
					"AssetBuildCore authoring host is unavailable.");
				FModuleManager::Get().LoadModuleChecked("StandardAssetImport");
				Editor::FRenderedAssetThumbnailService& ThumbnailService =
					Editor::GetDefaultRenderedAssetThumbnailService();
				::Durin::FLevelEditorModule& LevelEditorModule =
					FModuleManager::LoadModuleChecked<::Durin::FLevelEditorModule>("LevelEditor");
				::Durin::FMaterialEditorModule& MaterialEditorModule =
					FModuleManager::LoadModuleChecked<::Durin::FMaterialEditorModule>("MaterialEditor");
				::Durin::FTextureEditorModule& TextureEditorModule =
					FModuleManager::LoadModuleChecked<::Durin::FTextureEditorModule>("TextureEditor");
				::Durin::FStaticMeshEditorModule& StaticMeshEditorModule =
					FModuleManager::LoadModuleChecked<::Durin::FStaticMeshEditorModule>("StaticMeshEditor");
				::Durin::FSkeletalMeshEditorModule& SkeletalMeshEditorModule =
					FModuleManager::LoadModuleChecked<::Durin::FSkeletalMeshEditorModule>("SkeletalMeshEditor");
				Context.LevelEditorModule = &LevelEditorModule;
				bWorkspaceReady = RegisterEditorWorkspaces(
					*Context.WorkspaceManager,
					LevelEditorModule,
					MaterialEditorModule,
					TextureEditorModule,
					StaticMeshEditorModule,
					SkeletalMeshEditorModule,
					ThumbnailService);
			}
			Profiling::RecordStartupMilestone(
				Profiling::EStartupMilestone::WorkspaceRegistrationComplete);
			if (!bWorkspaceReady)
			{
				Context.ProjectBrowser->SetError(
					"Could not initialize the editor workspaces.");
				return false;
			}
			Context.ProjectBrowser->RecordCurrentProject();
			Profiling::RecordStartupMilestone(
				Profiling::EStartupMilestone::DefaultWorkspaceReady);
			return true;
		}

		auto MakeBootstrapProgress(const FBootstrapContext& Context)
			-> FBootstrapProgress
		{
			FBootstrapProgress Progress;
			Progress.State = Context.State;
			Progress.DefaultDocumentState = Context.DefaultDocumentState;
			Progress.Status = GetBootstrapStepStatus(Context.State);
			Progress.PhaseIndex = GetBootstrapPhaseIndex(Context.State);
			Progress.Message = Context.FailureMessage;
			return Progress;
		}

		auto DrawLoadingState(const FBootstrapContext& Context) -> void
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
			if (Context.State == EBootstrapState::WaitingForFirstPresent)
				Message = "Preparing editor...";
			else if (Context.State == EBootstrapState::LoadingDefaultDocument)
				Message = "Opening default level...";
			const FBootstrapProgress Progress = MakeBootstrapProgress(Context);
			const ImVec2 MessageSize = ImGui::CalcTextSize(Message);
			const ImVec2 Available = ImGui::GetContentRegionAvail();
			const float ContentWidth = MonaImGui::ScaleUI(360.0f);
			const float ContentHeight = MonaImGui::ScaleUI(72.0f);
			ImGui::SetCursorPos({
				FMath::Max(0.0f, (Available.x - ContentWidth) * 0.5f),
				FMath::Max(0.0f, (Available.y - ContentHeight) * 0.5f)});
			ImGui::SetCursorPosX(ImGui::GetCursorPosX()
				+ FMath::Max(0.0f, (ContentWidth - MessageSize.x) * 0.5f));
			ImGui::TextUnformatted(Message);
			ImGui::Spacing();
			ImGui::ProgressBar(
				static_cast<float>(Progress.PhaseIndex) / Progress.PhaseCount,
				{ContentWidth, MonaImGui::ScaleUI(8.0f)}, "");
			ImGui::SetCursorPosX(ImGui::GetCursorPosX()
				+ ContentWidth - MonaImGui::ScaleUI(70.0f));
			ImGui::TextDisabled("Phase %u/%u", Progress.PhaseIndex, Progress.PhaseCount);
			ImGui::End();
		}

		auto BuildDefaultEditorHostLayout(
			ImGuiID DockSpaceId,
			const ImVec2& DockSpaceSize,
			const std::vector<Editor::FWorkspaceDescriptor>& Descriptors
		) -> void
		{
			ImGui::DockBuilderRemoveNode(DockSpaceId);
			ImGui::DockBuilderAddNode(DockSpaceId, ImGuiDockNodeFlags_DockSpace | ImGuiDockNodeFlags_NoWindowMenuButton);
			ImGui::DockBuilderSetNodeSize(DockSpaceId, DockSpaceSize);
			if (ImGuiDockNode* DockSpaceNode = ImGui::DockBuilderGetNode(DockSpaceId))
				DockSpaceNode->WindowClass = Editor::WorkspaceUI::MakeRootWindowClass();
			for (const Editor::FWorkspaceDescriptor& Descriptor : Descriptors)
			{
				// Per-resource windows do not exist during the initial layout build; their root helper
				// applies the same host preference when each document first appears.
				if (Descriptor.DefaultHostDockPreference != Editor::EWorkspaceHostDockPreference::Center || !Descriptor.HasSingletonDocument()) continue;
				const std::string RootWindowName = Editor::WorkspaceUI::MakeRootWindowName(Descriptor.DisplayName, Descriptor.RootKey);
				ImGui::DockBuilderDockWindow(RootWindowName.c_str(), DockSpaceId);
			}
			ImGui::DockBuilderFinish(DockSpaceId);
		}

		auto DrawAboutDialog(bool& bOpen) -> void
		{
			if (!bOpen) return;
			ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
			ImGui::SetNextWindowSize(ImVec2(MonaImGui::ScaleUI(420.0f), MonaImGui::ScaleUI(170.0f)), ImGuiCond_Appearing);
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
			ImGui::Spacing();
			ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - MonaImGui::ScaleUI(82.0f));
			if (ImGui::Button("Close", ImVec2(MonaImGui::ScaleUI(82.0f), 0.0f))) bOpen = false;
			ImGui::End();
		}

		auto DrawPreferences(FHostSettings& Settings, MWindow& RootWindow, bool& bOpen) -> void
		{
			if (!bOpen) return;
			ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
			ImGui::SetNextWindowSize(ImVec2(MonaImGui::ScaleUI(430.0f), MonaImGui::ScaleUI(230.0f)), ImGuiCond_Appearing);
			if (ImGui::Begin("Editor Preferences###Durin.EditorHost.EditorPreferences", &bOpen, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings))
			{
				ImGui::SeparatorText("Appearance");
				ImGui::AlignTextToFramePadding();
				ImGui::TextDisabled("Color theme");
				ImGui::SameLine(MonaImGui::ScaleUI(130.0f));
				const MonaImGui::EColorTheme CurrentTheme = Settings.GetColorTheme();
				const char* ThemeLabel = CurrentTheme == MonaImGui::EColorTheme::Light ? "Light" : "Dark";
				ImGui::SetNextItemWidth(-1.0f);
				if (ImGui::BeginCombo("##ColorTheme", ThemeLabel))
				{
					for (const auto [Label, Theme] : {std::pair{"Dark", MonaImGui::EColorTheme::Dark}, std::pair{"Light", MonaImGui::EColorTheme::Light}})
					{
						if (ImGui::Selectable(Label, CurrentTheme == Theme))
						{
							Settings.SetColorTheme(Theme);
							MonaImGui::SetColorTheme(Theme);
							Settings.Save();
						}
					}
					ImGui::EndCombo();
				}

				ImGui::AlignTextToFramePadding();
				ImGui::TextDisabled("UI scale");
				ImGui::SameLine(MonaImGui::ScaleUI(130.0f));
				const float CurrentScale = Settings.GetUIScale();
				const std::string ScaleLabel = std::format("{}%", static_cast<int32>(CurrentScale * 100.0f));
				ImGui::SetNextItemWidth(-1.0f);
				if (ImGui::BeginCombo("##UIScale", ScaleLabel.c_str()))
				{
					for (const float Scale : {0.75f, 1.0f, 1.25f, 1.5f, 2.0f})
					{
						const std::string Label = std::format("{}%", static_cast<int32>(Scale * 100.0f));
						if (!ImGui::Selectable(Label.c_str(), std::abs(CurrentScale - Scale) < 0.01f)) continue;
						Settings.SetDisplaySettings(Settings.GetWindowWidth(), Settings.GetWindowHeight(), Scale);
						MonaImGui::SetGlobalUIScale(Scale);
						if (!RootWindow.IsMaximized()) RootWindow.ResizeWindow({
							static_cast<float>(Settings.GetWindowWidth()), static_cast<float>(Settings.GetWindowHeight())});
						Settings.Save();
					}
					ImGui::EndCombo();
				}
			}
			ImGui::End();
		}

		auto ObserveHostWindowState(FHostSettings& Settings, const MWindow& RootWindow) -> void
		{
			const bool bMaximized = RootWindow.IsMaximized();
			if (bMaximized == Settings.IsWindowMaximized()) return;
			Settings.SetWindowMaximized(bMaximized);
			Settings.Save();
		}

		auto DrawProfilingToolStatusDialog(bool& bOpen, const std::string& Message) -> void
		{
			if (!bOpen) return;
			ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
			ImGui::SetNextWindowSize(ImVec2(MonaImGui::ScaleUI(620.0f), MonaImGui::ScaleUI(260.0f)), ImGuiCond_Appearing);
			if (ImGui::Begin(
				"Tracy Profiling Tool Status###Durin.Profiling.ToolStatus",
				&bOpen,
				ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings
			))
			{
				ImGui::TextWrapped("%s", Message.c_str());
				ImGui::Spacing();
				ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - MonaImGui::ScaleUI(82.0f));
				if (ImGui::Button("Close", ImVec2(MonaImGui::ScaleUI(82.0f), 0.0f))) bOpen = false;
			}
			ImGui::End();
		}

		auto DrawProfilingMenu(
			const FProfilingToolService& ProfilingTools,
			std::string& StatusMessage,
			bool& bStatusOpen,
			bool& bAssetCompatibilityOpen
		) -> void
		{
			if (!ImGui::BeginMenu("Tools")) return;
			if (ImGui::BeginMenu("Asset Maintenance"))
			{
				if (ImGui::MenuItem("Compatibility Audit")) bAssetCompatibilityOpen = true;
				if (ImGui::MenuItem("Canonical Resave")) bAssetCompatibilityOpen = true;
				ImGui::EndMenu();
			}
			ImGui::Separator();
			if (ImGui::BeginMenu("Profiling"))
			{
				const FTracyToolStatus Status = ProfilingTools.QueryStatus();
				const auto ShowFailure = [&](std::string Error) {
					StatusMessage = std::move(Error);
					bStatusOpen = true;
				};

				ImGui::BeginDisabled(!Status.bAvailable);
				const bool bLaunchProfiler = ImGui::MenuItem(FProfilingToolService::LaunchProfilerLabel.data());
				ImGui::EndDisabled();
				if (!Status.bAvailable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
					ImGui::SetTooltip("%s", Status.Diagnostic.c_str());
				if (bLaunchProfiler)
				{
					std::string Error;
					if (!ProfilingTools.LaunchProfiler(&Error)) ShowFailure(std::move(Error));
				}

				ImGui::BeginDisabled(!Status.bAvailable);
				const bool bOpenCapture = ImGui::MenuItem(FProfilingToolService::OpenCaptureLabel.data());
				ImGui::EndDisabled();
				if (!Status.bAvailable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
					ImGui::SetTooltip("%s", Status.Diagnostic.c_str());
				if (bOpenCapture)
				{
					std::string Error;
					if (!ProfilingTools.SelectAndOpenCapture(ImGui::GetMainViewport()->PlatformHandleRaw, &Error))
						ShowFailure(std::move(Error));
				}

				if (ImGui::MenuItem(FProfilingToolService::OpenCaptureDirectoryLabel.data()))
				{
					std::string Error;
					if (!ProfilingTools.OpenCaptureDirectory(&Error)) ShowFailure(std::move(Error));
				}

				ImGui::Separator();
				if (ImGui::MenuItem(FProfilingToolService::ShowStatusLabel.data()))
				{
					StatusMessage = Status.Diagnostic;
					if (!Status.ExpectedVersion.empty())
						StatusMessage += std::format("\nExpected Tracy version: {}.", Status.ExpectedVersion);
					if (!Status.PackagePath.empty())
						StatusMessage += std::format("\nManaged package: \"{}\".", Status.PackagePath);
					if (!Status.RepairCommand.empty())
						StatusMessage += std::format("\nRepair with: {}", Status.RepairCommand);
					bStatusOpen = true;
				}
				ImGui::EndMenu();
			}
			ImGui::EndMenu();
		}

		auto DrawOpenEditorsMenu(Editor::FWorkspaceManager& WorkspaceManager) -> void
		{
			if (!ImGui::BeginMenu("Editors")) return;

			const std::vector<Editor::FDocumentTab>& Documents = WorkspaceManager.GetDocuments();
			const Editor::FDocumentTab* ActiveDocument = WorkspaceManager.GetActiveDocument();
			if (Documents.empty()) ImGui::MenuItem("No Open Editors", nullptr, false, false);
			for (const Editor::FDocumentTab& Document : Documents)
			{
				const std::string DisplayLabel = Document.bDirty ? std::format("{} *", Document.Label) : Document.Label;
				const std::string MenuLabel = std::format("{}###Durin.Editor.DocumentMenu.{}", DisplayLabel, Document.Id.Value);
				const bool bActive = ActiveDocument && ActiveDocument->Id == Document.Id;
				if (ImGui::MenuItem(MenuLabel.c_str(), nullptr, bActive) && !bActive)
					WorkspaceManager.ActivateDocument(Document.Id);
			}

			bool bDrewOpenCommand = false;
			for (const Editor::FWorkspaceDescriptor& Descriptor : WorkspaceManager.GetWorkspaceDescriptors())
			{
				if (!Descriptor.bShowInWindowMenu || !Descriptor.HasSingletonDocument()) continue;
				const bool bDocumentOpen = std::ranges::any_of(Documents, [&](const Editor::FDocumentTab& Document) {
					return Document.WorkspaceType == Descriptor.WorkspaceType
						&& Document.DocumentKey == Descriptor.SingletonDocumentKey;
				});
				if (bDocumentOpen) continue;
				if (!bDrewOpenCommand)
				{
					ImGui::Separator();
					bDrewOpenCommand = true;
				}
				const std::string MenuLabel = std::format(
					"Open {}###Durin.Editor.WorkspaceMenu.{}", Descriptor.DisplayName, Descriptor.RootKey
				);
				if (ImGui::MenuItem(MenuLabel.c_str()))
				{
					WorkspaceManager.OpenDocument({
						.WorkspaceType = Descriptor.WorkspaceType,
						.DocumentKey = Descriptor.SingletonDocumentKey,
						.Label = Descriptor.SingletonDocumentLabel,
						.bClosable = Descriptor.bSingletonDocumentClosable,
					});
				}
			}
			ImGui::EndMenu();
		}

		auto DrawWorkspaceMenus(
			Editor::FWorkspaceManager& WorkspaceManager,
			const std::shared_ptr<Editor::IWorkspace>& ActiveWorkspace,
			const FProfilingToolService& ProfilingTools,
			bool& bAboutDialogOpen,
			bool& bEditorPreferencesOpen,
			std::string& ProfilingStatusMessage,
			bool& bProfilingStatusOpen,
			bool& bAssetCompatibilityOpen) -> void
		{
			const std::vector<std::shared_ptr<Editor::IWorkspace>> Workspaces = WorkspaceManager.GetRegisteredWorkspaces();
			if (ImGui::BeginMenu("File"))
			{
				if (ImGui::MenuItem("Save Current", "Ctrl+S", false, ActiveWorkspace && ActiveWorkspace->CanSaveActiveDocument()))
					ActiveWorkspace->SaveActiveDocument();
				ImGui::Separator();
				for (const std::shared_ptr<Editor::IWorkspace>& Workspace : Workspaces) Workspace->DrawFileMenu();
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("Edit"))
			{
				const std::string UndoLabel = ActiveWorkspace && ActiveWorkspace->CanUndo() && !ActiveWorkspace->GetUndoDescription().empty()
					? std::format("Undo {}", ActiveWorkspace->GetUndoDescription()) : "Undo";
				const std::string RedoLabel = ActiveWorkspace && ActiveWorkspace->CanRedo() && !ActiveWorkspace->GetRedoDescription().empty()
					? std::format("Redo {}", ActiveWorkspace->GetRedoDescription()) : "Redo";
				if (ImGui::MenuItem(UndoLabel.c_str(), "Ctrl+Z", false, ActiveWorkspace && ActiveWorkspace->CanUndo())) ActiveWorkspace->Undo();
				if (ImGui::MenuItem(RedoLabel.c_str(), "Ctrl+Y", false, ActiveWorkspace && ActiveWorkspace->CanRedo())) ActiveWorkspace->Redo();
				ImGui::Separator();
				if (ImGui::MenuItem("Editor Preferences...")) bEditorPreferencesOpen = true;
				for (const std::shared_ptr<Editor::IWorkspace>& Workspace : Workspaces) Workspace->DrawEditMenu();
				ImGui::EndMenu();
			}
			DrawProfilingMenu(ProfilingTools, ProfilingStatusMessage, bProfilingStatusOpen, bAssetCompatibilityOpen);
			if (ImGui::BeginMenu("Window"))
			{
				DrawOpenEditorsMenu(WorkspaceManager);
				if (ActiveWorkspace)
				{
					ImGui::Separator();
					ActiveWorkspace->DrawWindowMenu();
					if (ImGui::MenuItem("Reset Active Editor Layout")) ActiveWorkspace->ResetLayout();
				}
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("Help"))
			{
				if (ImGui::MenuItem("About Durin...")) bAboutDialogOpen = true;
				ImGui::EndMenu();
			}
		}

		auto DrawCustomEditorTitleBar(
			Editor::FWorkspaceManager& WorkspaceManager,
			MWindow& RootWindow,
			const FRHITexture* BrandTexture,
			const FProfilingToolService& ProfilingTools,
			bool bDrawWorkspaceMenus,
			bool& bAboutDialogOpen,
			bool& bEditorPreferencesOpen,
			std::string& ProfilingStatusMessage,
			bool& bProfilingStatusOpen,
			bool& bAssetCompatibilityOpen) -> void
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
			const float CaptionStartX = BarMax.x - ButtonWidth * 3.0f;
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
			const float RequiredMenuWidth = bDrawWorkspaceMenus ? MonaImGui::ScaleUI(245.0f) : 0.0f;
			const float MenuGap = MonaImGui::ScaleUI(EditorTitleBarMenuGap);
			const float AvailableTitleWidth = CaptionStartX - ImGui::GetCursorScreenPos().x
				- RequiredMenuWidth - MonaImGui::ScaleUI(56.0f) - MenuGap;
			bool bDrewTitle = false;
			if (AvailableTitleWidth >= MonaImGui::ScaleUI(120.0f))
			{
				const ImVec2 TitleCursor = ImGui::GetCursorScreenPos();
				constexpr std::string_view BrandName = "Durin";
				const bool bHasBrandPrefix = WindowTitle.starts_with(BrandName);
				const char* Suffix = bHasBrandPrefix ? WindowTitle.c_str() + BrandName.size() : WindowTitle.c_str();
				ImFont* BrandFont = MonaImGui::GetMediumUIFont();
				ImFont* BodyFont = ImGui::GetFont();
				const float FontSize = ImGui::GetFontSize();
				const float BrandFontSize = MonaImGui::QuantizeDynamicFontSize(FontSize * 0.9f);
				const ImVec2 TitlePosition(
					TitleCursor.x,
					std::round(BarMin.y + (BarMax.y - BarMin.y - FontSize) * 0.5f));
				const float BrandBaselineOffset = BodyFont->GetFontBaked(FontSize)->Ascent
					- BrandFont->GetFontBaked(BrandFontSize)->Ascent;
				const float BrandWidth = bHasBrandPrefix
					? BrandFont->CalcTextSizeA(
						BrandFontSize, FLT_MAX, 0.0f, BrandName.data(), BrandName.data() + BrandName.size()).x
					: 0.0f;
				const float SuffixWidth = BodyFont->CalcTextSizeA(FontSize, FLT_MAX, 0.0f, Suffix).x;
				const float TitleWidth = BrandWidth + SuffixWidth;
				const float DrawWidth = std::min(TitleWidth, AvailableTitleWidth);
				const ImVec4 ClipRect(
					TitlePosition.x, BarMin.y, TitlePosition.x + DrawWidth, BarMax.y);
				const ImU32 TitleColor = ImGui::GetColorU32(ImGuiCol_Text);
				if (bHasBrandPrefix)
					DrawList->AddText(
						BrandFont, BrandFontSize, TitlePosition + ImVec2(0.0f, BrandBaselineOffset), TitleColor,
						BrandName.data(), BrandName.data() + BrandName.size(), 0.0f, &ClipRect);
				DrawList->AddText(
					BodyFont, FontSize, TitlePosition + ImVec2(BrandWidth, 0.0f), TitleColor,
					Suffix, nullptr, 0.0f, &ClipRect);
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
			std::shared_ptr<Editor::IWorkspace> ActiveWorkspace;
			if (const Editor::FDocumentTab* ActiveDocument = WorkspaceManager.GetActiveDocument())
				ActiveWorkspace = WorkspaceManager.FindWorkspace(ActiveDocument->WorkspaceType);
			if (bDrawWorkspaceMenus)
			{
				DrawWorkspaceMenus(
					WorkspaceManager, ActiveWorkspace, ProfilingTools, bAboutDialogOpen, bEditorPreferencesOpen,
					ProfilingStatusMessage, bProfilingStatusOpen, bAssetCompatibilityOpen);
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
			DrawCaptionButton(0, EWindowTitleBarHitTest::Minimize);
			DrawCaptionButton(1, EWindowTitleBarHitTest::Maximize);
			DrawCaptionButton(2, EWindowTitleBarHitTest::Close);

			static uint64 LayoutGeneration = 1;
			const auto ToClientX = [&](float ScreenX) { return static_cast<int32>(std::round(ScreenX - Viewport->Pos.x)); };
			const int32 ClientHeight = static_cast<int32>(std::round(BarMax.y - Viewport->Pos.y));
			FWindowTitleBarLayout Layout;
			Layout.Generation = ++LayoutGeneration;
			Layout.bValid = true;
			Layout.Height = ClientHeight;
			Layout.MinimumWindowWidth = static_cast<int32>(std::ceil(MonaImGui::ScaleUI(640.0f)));
			if (FirstMenuX > BarMin.x) Layout.DragRegions.push_back({0, 0, ToClientX(FirstMenuX), ClientHeight});
			if (CaptionStartX > MenuEndX) Layout.DragRegions.push_back({ToClientX(MenuEndX), 0, ToClientX(CaptionStartX), ClientHeight});
			Layout.MinimizeRegion = {ToClientX(CaptionStartX), 0, ToClientX(CaptionStartX + ButtonWidth), ClientHeight};
			Layout.MaximizeRegion = {ToClientX(CaptionStartX + ButtonWidth), 0, ToClientX(CaptionStartX + ButtonWidth * 2.0f), ClientHeight};
			Layout.CloseRegion = {ToClientX(CaptionStartX + ButtonWidth * 2.0f), 0, ToClientX(BarMax.x), ClientHeight};
			RootWindow.PublishTitleBarLayout(Layout);

			ImGui::EndMainMenuBar();
			ImGui::PopStyleVar();
		}

		auto DrawWorkspaceHost(
			Editor::FWorkspaceManager& WorkspaceManager,
			FHostSettings& HostSettings,
			MWindow& RootWindow,
			const FProfilingToolService& ProfilingTools,
			bool& bAboutDialogOpen,
			bool& bEditorPreferencesOpen,
			std::string& ProfilingStatusMessage,
			bool& bProfilingStatusOpen,
			FAssetCompatibilityWindow& AssetCompatibilityWindow,
			bool& bAssetCompatibilityOpen,
			::Durin::FLevelEditorModule& LevelEditorModule
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
			const ImGuiWindowFlags HostFlags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
				ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
				ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
				(bCustomTitleBar ? ImGuiWindowFlags_None : ImGuiWindowFlags_MenuBar);
			ImGui::Begin("###Durin.Editor.WorkspaceHost", nullptr, HostFlags);
			ImGui::PopStyleVar(3);

			WorkspaceManager.RefreshDocumentState();
			std::shared_ptr<Editor::IWorkspace> ActiveWorkspace;
			if (const Editor::FDocumentTab* ActiveDocument = WorkspaceManager.GetActiveDocument())
				ActiveWorkspace = WorkspaceManager.FindWorkspace(ActiveDocument->WorkspaceType);
			const ImGuiIO& IO = ImGui::GetIO();
			if (ActiveWorkspace && IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
				ActiveWorkspace->SaveActiveDocument();
			if (ActiveWorkspace && IO.KeyCtrl && !IO.WantTextInput)
			{
				if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) ActiveWorkspace->Undo();
				if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) ActiveWorkspace->Redo();
			}
			if (!bCustomTitleBar && ImGui::BeginMenuBar())
			{
				DrawWorkspaceMenus(
					WorkspaceManager, ActiveWorkspace, ProfilingTools, bAboutDialogOpen, bEditorPreferencesOpen,
					ProfilingStatusMessage, bProfilingStatusOpen, bAssetCompatibilityOpen);
				ImGui::EndMenuBar();
			}
			DrawAboutDialog(bAboutDialogOpen);
			DrawPreferences(HostSettings, RootWindow, bEditorPreferencesOpen);
			DrawProfilingToolStatusDialog(bProfilingStatusOpen, ProfilingStatusMessage);
			AssetCompatibilityWindow.Draw(bAssetCompatibilityOpen,
				[&LevelEditorModule](const FAssetPath& Path) {
					(void)LevelEditorModule.RevealAssetInContentBrowser(Path);
				});

			const ImVec2 DockSpaceSize = ImGui::GetContentRegionAvail();
			const ImGuiID DockSpaceId = Editor::WorkspaceUI::MakeHostDockSpaceId(Editor::WorkspaceUI::HostLayoutVersion);
			const bool bNeedsDefaultLayout = ImGui::DockBuilderGetNode(DockSpaceId) == nullptr;
			if (bNeedsDefaultLayout)
			{
				// DockBuilder must finish before DockSpace submission so the new tree retains this frame's host window.
				BuildDefaultEditorHostLayout(DockSpaceId, DockSpaceSize, WorkspaceManager.GetWorkspaceDescriptors());
			}
			Editor::WorkspaceUI::SubmitHostDockSpace(Editor::WorkspaceUI::HostLayoutVersion, DockSpaceSize, ImGuiDockNodeFlags_NoWindowMenuButton);

			for (const std::shared_ptr<Editor::IWorkspace>& Workspace : WorkspaceManager.GetRegisteredWorkspaces())
			{
				if (Workspace->DrawWorkspace(Workspace == ActiveWorkspace))
					WorkspaceManager.ActivateWorkspace(Workspace->GetWorkspaceType());
			}
			Editor::WorkspaceUI::DrawDocumentCloseConfirmation(WorkspaceManager);

			ImGui::End();
		}
	}

} // namespace Durin::Editor::MainFrame

namespace Durin
{
	using namespace Editor::MainFrame;

	IMPLEMENT_MODULE(FMainFrameModule, MainFrame)

	auto FMainFrameModule::ShutdownModule() -> void
	{
		DestroyDefaultFrame();
	}

	auto FMainFrameModule::CreateDefaultFrame() -> void
	{
		check(BootstrapContext == nullptr);
		BootstrapContext = std::make_shared<FBootstrapContext>();
		FBootstrapContext& Context = *BootstrapContext;
		Context.bHasProject = HasCurrentProject();
		Context.DefaultDocumentState = Context.bHasProject
			? EDefaultDocumentState::Pending
			: EDefaultDocumentState::NotApplicable;
		Context.HostSettings = std::make_shared<FHostSettings>();
		Context.HostSettings->Load();
		MonaImGui::SetColorTheme(Context.HostSettings->GetColorTheme());
		MonaImGui::SetGlobalUIScale(Context.HostSettings->GetUIScale());
		Context.RootWindow = std::make_shared<MWindow>();
		Context.RootWindow->SetWindowDecorationMode(EWindowDecorationMode::CustomTitleBar);
		MonaImGui::BindMainViewportToWindow(Context.RootWindow);
		Context.WorkspaceManager = std::make_shared<Editor::FWorkspaceManager>();
		Context.ProjectBrowser = std::make_shared<FProjectBrowser>();
		Context.BrandTexture = std::make_unique<FEditorBrandTexture>();
		std::string BrandTextureError;
		if (!Context.BrandTexture->Load(BrandTextureError))
			DURIN_WARN("Could not load the editor branding texture: {}", BrandTextureError);
		Context.ProfilingTools =
			std::make_shared<FProfilingToolService>(FPaths::RootDir());
		Context.AssetCompatibilityWindow =
			std::make_shared<FAssetCompatibilityWindow>();

		const FIntPoint WindowSize{
			Context.HostSettings->GetWindowWidth(),
			Context.HostSettings->GetWindowHeight()};
		Context.RootWindow->SetTitle(GetCurrentProject()
			? std::format("Durin - {}", GetCurrentProject()->Name)
			: "Durin - Project Browser");
		Context.RootWindow->ReshapeWindow(
			{100.0f, 100.0f},
			{static_cast<float>(WindowSize.x), static_cast<float>(WindowSize.y)});

		Context.ProjectBrowser->SetOpenProject([](
			std::string_view ProjectFile, std::string& OutError) {
			return RelaunchEditorForProject(ProjectFile, &OutError);
		});

		auto EditorRootWidget = std::make_shared<MFunctionWidget>();
		const std::weak_ptr<FBootstrapContext> WeakContext =
			BootstrapContext;
		EditorRootWidget->Construct([WeakContext,
			bAboutDialogOpen = false, bEditorPreferencesOpen = false, ProfilingStatusMessage = std::string{},
			bProfilingStatusOpen = false, bAssetCompatibilityOpen = false]() mutable {
			const std::shared_ptr<FBootstrapContext> Context =
				WeakContext.lock();
			if (!Context) return;
			Asset::Build::PumpBuildHostCompletions();
			ObserveHostWindowState(
				*Context->HostSettings, *Context->RootWindow);
			const bool bReadyWorkspace = Context->State == EBootstrapState::Ready
				&& Context->bHasProject && Context->LevelEditorModule;
			const FRHITexture* BrandTexture = Context->BrandTexture->UpdateAndGetTexture();
			if (Context->RootWindow->GetEffectiveWindowDecorationMode() == EWindowDecorationMode::CustomTitleBar)
			{
				DrawCustomEditorTitleBar(
					*Context->WorkspaceManager,
					*Context->RootWindow,
					BrandTexture,
					*Context->ProfilingTools,
					bReadyWorkspace,
					bAboutDialogOpen,
					bEditorPreferencesOpen,
					ProfilingStatusMessage,
					bProfilingStatusOpen,
					bAssetCompatibilityOpen);
			}
			if (bReadyWorkspace)
			{
				DrawWorkspaceHost(
					*Context->WorkspaceManager,
					*Context->HostSettings,
					*Context->RootWindow,
					*Context->ProfilingTools,
					bAboutDialogOpen,
					bEditorPreferencesOpen,
					ProfilingStatusMessage,
					bProfilingStatusOpen,
					*Context->AssetCompatibilityWindow,
					bAssetCompatibilityOpen,
					*Context->LevelEditorModule);
				return;
			}
			if (!Context->bHasProject
				|| Context->State == EBootstrapState::Failed)
			{
				Context->ProjectBrowser->Draw(BrandTexture);
				return;
			}
			DrawLoadingState(*Context);
		});
		Context.RootWindow->SetContent(EditorRootWidget);

		// Keep the native window hidden until its persisted display state has been
		// applied. Showing it before maximizing causes a visible normal-size frame
		// during editor startup.
		Mona::FMonaApplication::Get().AddWindow(Context.RootWindow, false);
		if (Context.HostSettings->IsWindowMaximized())
		{
			Context.RootWindow->MaximizeWindow();
		}
		{
			DURIN_PROFILE_CPU_ZONE_NAMED("Startup.NativeViewport");
			Mona::FMonaApplication::Get().GetRenderer()->CreateViewport(
				Context.RootWindow);
		}
		Profiling::RecordStartupMilestone(Profiling::EStartupMilestone::NativeViewportReady);
		Profiling::ArmEditorShellFirstPresent();
		Context.RootWindow->ShowWindow();
		TransitionBootstrap(
			Context, EBootstrapState::WaitingForFirstPresent);
	}

	auto FMainFrameModule::DestroyDefaultFrame() -> void
	{
		Asset::Build::ShutdownBuildHost();
		BootstrapContext.reset();
	}

	auto FMainFrameModule::AdvanceDefaultBootstrap(
		bool bFirstPresentAvailable) -> FBootstrapProgress
	{
		if (!BootstrapContext)
			return {.Status = EBootstrapStepStatus::Failed,
				.Message = "The editor main frame is unavailable."};
		FBootstrapContext& Context = *BootstrapContext;
		if (Context.State == EBootstrapState::Ready
			|| Context.State == EBootstrapState::Failed)
			return MakeBootstrapProgress(Context);
		if (Context.State == EBootstrapState::WaitingForFirstPresent)
		{
			if (!bFirstPresentAvailable)
				return MakeBootstrapProgress(Context);
			if (!Context.bHasProject)
			{
				Profiling::RecordStartupMilestone(
					Profiling::EStartupMilestone::WorkspaceRegistrationBegin);
				Profiling::RecordStartupMilestone(
					Profiling::EStartupMilestone::WorkspaceRegistrationComplete);
				Profiling::RecordStartupMilestone(
					Profiling::EStartupMilestone::DefaultWorkspaceReady);
				TransitionBootstrap(Context, EBootstrapState::Ready);
				Profiling::TryLogStartupTimingSummary();
				return MakeBootstrapProgress(Context);
			}
			TransitionBootstrap(
				Context, EBootstrapState::LoadingWorkspace);
			return MakeBootstrapProgress(Context);
		}
		if (Context.State == EBootstrapState::LoadingWorkspace)
		{
			if (Context.bWorkspaceActivationStarted)
				return MakeBootstrapProgress(Context);
			Context.bWorkspaceActivationStarted = true;
			TransitionBootstrap(
				Context,
				ActivateEditorWorkspaces(Context)
					? EBootstrapState::WorkspaceReady
					: EBootstrapState::Failed);
			if (Context.State == EBootstrapState::Failed)
				Context.FailureMessage = "Could not initialize the editor workspaces.";
			return MakeBootstrapProgress(Context);
		}
		if (Context.State == EBootstrapState::WorkspaceReady)
		{
			Context.DefaultDocumentState =
				EDefaultDocumentState::Loading;
			TransitionBootstrap(
				Context, EBootstrapState::LoadingDefaultDocument);
			return MakeBootstrapProgress(Context);
		}
		if (Context.State != EBootstrapState::LoadingDefaultDocument)
			return MakeBootstrapProgress(Context);

		Context.DefaultDocumentState = Context.LevelEditorModule
			&& Context.LevelEditorModule->OpenDefaultDocument()
			? EDefaultDocumentState::Ready
			: EDefaultDocumentState::Failed;
		if (Context.DefaultDocumentState == EDefaultDocumentState::Ready)
			TransitionBootstrap(Context, EBootstrapState::Ready);
		else
		{
			Context.FailureMessage = "Could not open the configured default Level document.";
			Context.ProjectBrowser->SetError(Context.FailureMessage);
			TransitionBootstrap(Context, EBootstrapState::Failed);
		}
		Profiling::TryLogStartupTimingSummary();
		return MakeBootstrapProgress(Context);
	}

	auto FMainFrameModule::GetDefaultBootstrapProgress() const
		-> FBootstrapProgress
	{
		return BootstrapContext
			? MakeBootstrapProgress(*BootstrapContext)
			: FBootstrapProgress{};
	}

	auto FMainFrameModule::GetDefaultBootstrapState() const
		-> EBootstrapState
	{
		return BootstrapContext
			? BootstrapContext->State
			: EBootstrapState::ConstructingShell;
	}

	auto FMainFrameModule::GetDefaultDocumentState() const
		-> EDefaultDocumentState
	{
		return BootstrapContext
			? BootstrapContext->DefaultDocumentState
			: EDefaultDocumentState::NotApplicable;
	}
} // namespace Durin
