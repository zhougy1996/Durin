#include "MainFrameModule.h"

#include "ProjectBrowser.h"
#include "ProfilingToolService.h"

#include "Editor/EditorWorkspace.h"
#include "Editor/EditorWorkspaceUI.h"
#include "Editor/EditorEngine.h"
#include "Mona.h"
#include "LevelEditorModule.h"
#include "MaterialEditorModule.h"
#include "TextureEditorModule.h"
#include "MonaImGui.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Misc/Project.h"
#include "Misc/Version.h"
#include "Profiling/Profiling.h"
#include "Settings/EditorHostSettings.h"

#include "Widgets/MFunctionWidget.h"
#include "Widgets/MWindow.h"

namespace Durin
{
	namespace
	{
		auto RegisterEditorWorkspaces(
			FEditorWorkspaceManager& WorkspaceManager,
			FLevelEditorModule& LevelEditorModule,
			FMaterialEditorModule& MaterialEditorModule,
			FTextureEditorModule& TextureEditorModule
		) -> bool
		{
			if (!LevelEditorModule.RegisterLevelEditorWorkspace(WorkspaceManager)) return false;
			if (!MaterialEditorModule.RegisterMaterialEditorWorkspace(WorkspaceManager))
			{
				LevelEditorModule.UnregisterLevelEditorWorkspace();
				return false;
			}
			if (!TextureEditorModule.RegisterTextureEditorWorkspace(WorkspaceManager))
			{
				MaterialEditorModule.UnregisterMaterialEditorWorkspace();
				LevelEditorModule.UnregisterLevelEditorWorkspace();
				return false;
			}
			if (WorkspaceManager.OpenDefaultWorkspaces()) return true;

			// Feature modules own their handles, but the host coordinates this multi-module
			// startup so a failed default document cannot leave a partial editor behind.
			TextureEditorModule.UnregisterTextureEditorWorkspace();
			MaterialEditorModule.UnregisterMaterialEditorWorkspace();
			LevelEditorModule.UnregisterLevelEditorWorkspace();
			return false;
		}

		auto BuildDefaultEditorHostLayout(
			ImGuiID DockSpaceId,
			const ImVec2& DockSpaceSize,
			const std::vector<FEditorWorkspaceDescriptor>& Descriptors
		) -> void
		{
			ImGui::DockBuilderRemoveNode(DockSpaceId);
			ImGui::DockBuilderAddNode(DockSpaceId, ImGuiDockNodeFlags_DockSpace | ImGuiDockNodeFlags_NoWindowMenuButton);
			ImGui::DockBuilderSetNodeSize(DockSpaceId, DockSpaceSize);
			if (ImGuiDockNode* DockSpaceNode = ImGui::DockBuilderGetNode(DockSpaceId))
				DockSpaceNode->WindowClass = EditorWorkspaceUI::MakeEditorRootWindowClass();
			for (const FEditorWorkspaceDescriptor& Descriptor : Descriptors)
			{
				// Per-resource windows do not exist during the initial layout build; their root helper
				// applies the same host preference when each document first appears.
				if (Descriptor.DefaultHostDockPreference != EEditorWorkspaceHostDockPreference::Center || !Descriptor.HasSingletonDocument()) continue;
				const std::string RootWindowName = EditorWorkspaceUI::MakeEditorRootWindowName(Descriptor.DisplayName, Descriptor.RootKey);
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

		auto DrawEditorPreferences(FEditorHostSettings& Settings, MWindow& RootWindow, bool& bOpen) -> void
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

		auto ObserveEditorHostWindowState(FEditorHostSettings& Settings, const MWindow& RootWindow) -> void
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
			bool& bStatusOpen
		) -> void
		{
			if (!ImGui::BeginMenu("Tools")) return;
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

		auto DrawOpenEditorsMenu(FEditorWorkspaceManager& WorkspaceManager) -> void
		{
			if (!ImGui::BeginMenu("Editors")) return;

			const std::vector<FEditorDocumentTab>& Documents = WorkspaceManager.GetDocuments();
			const FEditorDocumentTab* ActiveDocument = WorkspaceManager.GetActiveDocument();
			if (Documents.empty()) ImGui::MenuItem("No Open Editors", nullptr, false, false);
			for (const FEditorDocumentTab& Document : Documents)
			{
				const std::string DisplayLabel = Document.bDirty ? std::format("{} *", Document.Label) : Document.Label;
				const std::string MenuLabel = std::format("{}###Durin.Editor.DocumentMenu.{}", DisplayLabel, Document.Id.Value);
				const bool bActive = ActiveDocument && ActiveDocument->Id == Document.Id;
				if (ImGui::MenuItem(MenuLabel.c_str(), nullptr, bActive) && !bActive)
					WorkspaceManager.ActivateDocument(Document.Id);
			}

			bool bDrewOpenCommand = false;
			for (const FEditorWorkspaceDescriptor& Descriptor : WorkspaceManager.GetWorkspaceDescriptors())
			{
				if (!Descriptor.bShowInWindowMenu || !Descriptor.HasSingletonDocument()) continue;
				const bool bDocumentOpen = std::ranges::any_of(Documents, [&](const FEditorDocumentTab& Document) {
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

		auto DrawWorkspaceHost(
			FEditorWorkspaceManager& WorkspaceManager,
			FEditorHostSettings& HostSettings,
			MWindow& RootWindow,
			const FProfilingToolService& ProfilingTools,
			bool& bAboutDialogOpen,
			bool& bEditorPreferencesOpen,
			std::string& ProfilingStatusMessage,
			bool& bProfilingStatusOpen
		) -> void
		{
			ImGuiViewport* Viewport = ImGui::GetMainViewport();
			ImGui::SetNextWindowPos(Viewport->WorkPos);
			ImGui::SetNextWindowSize(Viewport->WorkSize);
			ImGui::SetNextWindowViewport(Viewport->ID);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
			const ImGuiWindowFlags HostFlags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
				ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
				ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
				ImGuiWindowFlags_MenuBar;
			ImGui::Begin("###Durin.Editor.WorkspaceHost", nullptr, HostFlags);
			ImGui::PopStyleVar(3);

			WorkspaceManager.RefreshDocumentState();
			std::shared_ptr<IEditorWorkspace> ActiveWorkspace;
			if (const FEditorDocumentTab* ActiveDocument = WorkspaceManager.GetActiveDocument())
				ActiveWorkspace = WorkspaceManager.FindWorkspace(ActiveDocument->WorkspaceType);
			const ImGuiIO& IO = ImGui::GetIO();
			if (ActiveWorkspace && IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
				ActiveWorkspace->SaveActiveDocument();
			if (ActiveWorkspace && IO.KeyCtrl && !IO.WantTextInput)
			{
				if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) ActiveWorkspace->Undo();
				if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) ActiveWorkspace->Redo();
			}
			if (ImGui::BeginMenuBar())
			{
				const std::vector<std::shared_ptr<IEditorWorkspace>> Workspaces = WorkspaceManager.GetRegisteredWorkspaces();
				if (ImGui::BeginMenu("File"))
				{
					if (ImGui::MenuItem("Save Current", "Ctrl+S", false, ActiveWorkspace && ActiveWorkspace->CanSaveActiveDocument()))
						ActiveWorkspace->SaveActiveDocument();
					ImGui::Separator();
					for (const std::shared_ptr<IEditorWorkspace>& Workspace : Workspaces) Workspace->DrawFileMenu();
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
					for (const std::shared_ptr<IEditorWorkspace>& Workspace : Workspaces) Workspace->DrawEditMenu();
					ImGui::EndMenu();
				}
				DrawProfilingMenu(ProfilingTools, ProfilingStatusMessage, bProfilingStatusOpen);
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
				ImGui::EndMenuBar();
			}
			DrawAboutDialog(bAboutDialogOpen);
			DrawEditorPreferences(HostSettings, RootWindow, bEditorPreferencesOpen);
			DrawProfilingToolStatusDialog(bProfilingStatusOpen, ProfilingStatusMessage);

			const ImVec2 DockSpaceSize = ImGui::GetContentRegionAvail();
			const ImGuiID DockSpaceId = EditorWorkspaceUI::MakeEditorHostDockSpaceId(EditorWorkspaceUI::HostLayoutVersion);
			const bool bNeedsDefaultLayout = ImGui::DockBuilderGetNode(DockSpaceId) == nullptr;
			if (bNeedsDefaultLayout)
			{
				// DockBuilder must finish before DockSpace submission so the new tree retains this frame's host window.
				BuildDefaultEditorHostLayout(DockSpaceId, DockSpaceSize, WorkspaceManager.GetWorkspaceDescriptors());
			}
			EditorWorkspaceUI::SubmitEditorHostDockSpace(EditorWorkspaceUI::HostLayoutVersion, DockSpaceSize, ImGuiDockNodeFlags_NoWindowMenuButton);

			for (const std::shared_ptr<IEditorWorkspace>& Workspace : WorkspaceManager.GetRegisteredWorkspaces())
			{
				if (Workspace->DrawWorkspace(Workspace == ActiveWorkspace))
					WorkspaceManager.ActivateWorkspace(Workspace->GetWorkspaceType());
			}
			EditorWorkspaceUI::DrawDocumentCloseConfirmation(WorkspaceManager);

			ImGui::End();
		}
	}

	IMPLEMENT_MODULE(FMainFrameModule, MainFrame)

	auto FMainFrameModule::StartupModule() -> void
	{
	}

	auto FMainFrameModule::ShutdownModule() -> void
	{
	}

	auto FMainFrameModule::CreateDefaultMainFrame() -> void
	{
		auto HostSettings = std::make_shared<FEditorHostSettings>();
		HostSettings->Load();
		MonaImGui::SetColorTheme(HostSettings->GetColorTheme());
		MonaImGui::SetGlobalUIScale(HostSettings->GetUIScale());
		FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");
		FMaterialEditorModule& MaterialEditorModule = FModuleManager::LoadModuleChecked<FMaterialEditorModule>("MaterialEditor");
		FTextureEditorModule& TextureEditorModule = FModuleManager::LoadModuleChecked<FTextureEditorModule>("TextureEditor");
		const FIntPoint WindowSize{HostSettings->GetWindowWidth(), HostSettings->GetWindowHeight()};
		FLevelEditorModule* LevelEditorModulePtr = &LevelEditorModule;
		FMaterialEditorModule* MaterialEditorModulePtr = &MaterialEditorModule;
		FTextureEditorModule* TextureEditorModulePtr = &TextureEditorModule;
		auto RootWindow = std::make_shared<MWindow>();
		MonaImGui::BindMainViewportToWindow(RootWindow);

		auto EditorRootWidget = std::make_shared<MFunctionWidget>();
		auto WorkspaceManager = std::make_shared<FEditorWorkspaceManager>();
		auto bWorkspaceReady = std::make_shared<bool>(false);
		auto ProjectBrowser = std::make_shared<FProjectBrowser>();
		auto ProfilingTools = std::make_shared<FProfilingToolService>(FPaths::RootDir());
		const std::weak_ptr<MWindow> WeakRootWindow = RootWindow;
		if (HasCurrentProject())
		{
			*bWorkspaceReady = RegisterEditorWorkspaces(*WorkspaceManager, LevelEditorModule, MaterialEditorModule, TextureEditorModule);
			if (!*bWorkspaceReady) ProjectBrowser->SetError("Could not initialize the editor workspaces.");
			else
			{
				ProjectBrowser->RecordCurrentProject();
				if (GEditor) GEditor->StartAssetUpgradeAudit();
			}
		}

		RootWindow->SetTitle(GetCurrentProject() ? std::format("Durin Editor - {}", GetCurrentProject()->Name) : "Durin Editor - Project Browser");
		RootWindow->ReshapeWindow({100.0f, 100.0f}, {static_cast<float>(WindowSize.x), static_cast<float>(WindowSize.y)});

		ProjectBrowser->SetOpenProject([WorkspaceManager, bWorkspaceReady, WeakRootWindow, LevelEditorModulePtr, MaterialEditorModulePtr, TextureEditorModulePtr](std::string_view ProjectFile, std::string& OutError) {
			const std::array<std::string_view, 2> Arguments = {"--project", ProjectFile};
			if (!InitializeCurrentProject(Arguments, &OutError)) return false;
			DURIN_PROFILE_PROGRAM_IDENTITY(
				DURIN_RUNTIME_VARIANT,
				GetCurrentProject() ? std::string_view{GetCurrentProject()->Name} : std::string_view{},
				FPlatformProcess::CurrentProcessId()
			);
			PathUtilities::InitDefaultMountPoints();
			*bWorkspaceReady = RegisterEditorWorkspaces(*WorkspaceManager, *LevelEditorModulePtr, *MaterialEditorModulePtr, *TextureEditorModulePtr);
			if (!*bWorkspaceReady)
			{
				OutError = "Could not initialize the editor workspaces.";
				return false;
			}
			if (GEditor) GEditor->StartAssetUpgradeAudit();
			if (const std::shared_ptr<MWindow> RootWindow = WeakRootWindow.lock())
				RootWindow->SetTitle(std::format("Durin Editor - {}", GetCurrentProject()->Name));
			return true;
		});

		EditorRootWidget->Construct([WorkspaceManager, bWorkspaceReady, ProjectBrowser, ProfilingTools, HostSettings, WeakRootWindow,
			bAboutDialogOpen = false, bEditorPreferencesOpen = false, ProfilingStatusMessage = std::string{},
			bProfilingStatusOpen = false]() mutable {
			const std::shared_ptr<MWindow> RootWindow = WeakRootWindow.lock();
			if (RootWindow) ObserveEditorHostWindowState(*HostSettings, *RootWindow);
			if (*bWorkspaceReady)
			{
				if (RootWindow)
				{
					DrawWorkspaceHost(
						*WorkspaceManager,
						*HostSettings,
						*RootWindow,
						*ProfilingTools,
						bAboutDialogOpen,
						bEditorPreferencesOpen,
						ProfilingStatusMessage,
						bProfilingStatusOpen
					);
				}
				return;
			}
			ProjectBrowser->Draw();
		});
		RootWindow->SetContent(EditorRootWidget);

		// Keep the native window hidden until its persisted display state has been
		// applied. Showing it before maximizing causes a visible normal-size frame
		// during editor startup.
		Mona::FMonaApplication::Get().AddWindow(RootWindow, false);
		Mona::FMonaApplication::Get().GetRenderer()->CreateViewport(RootWindow);

		if (HostSettings->IsWindowMaximized())
		{
			RootWindow->MaximizeWindow();
		}
		RootWindow->ShowWindow();
	}
} // namespace Durin
