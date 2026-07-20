#include "MainFrameModule.h"

#include "ProjectBrowser.h"

#include "Editor/EditorWorkspace.h"
#include "Editor/EditorWorkspaceUI.h"
#include "Mona.h"
#include "LevelEditorModule.h"
#include "MonaImGui.h"
#include "Misc/Paths.h"
#include "Misc/Project.h"

#include "Widgets/MFunctionWidget.h"

namespace Durin
{
	namespace
	{
		constexpr uint32 EditorHostLayoutVersion = 2;

		auto BuildDefaultEditorHostLayout(ImGuiID DockSpaceId, const ImVec2& DockSpaceSize) -> void
		{
			ImGui::DockBuilderRemoveNode(DockSpaceId);
			ImGui::DockBuilderAddNode(DockSpaceId, ImGuiDockNodeFlags_DockSpace | ImGuiDockNodeFlags_NoWindowMenuButton);
			ImGui::DockBuilderSetNodeSize(DockSpaceId, DockSpaceSize);
			if (ImGuiDockNode* DockSpaceNode = ImGui::DockBuilderGetNode(DockSpaceId))
				DockSpaceNode->WindowClass = EditorWorkspaceUI::MakeEditorRootWindowClass();
			const std::string LevelEditorRootName = EditorWorkspaceUI::MakeEditorRootWindowName("Level Editor", "LevelEditor");
			const std::string MaterialEditorRootName = EditorWorkspaceUI::MakeEditorRootWindowName("Material Editor", "MaterialEditor");
			ImGui::DockBuilderDockWindow(LevelEditorRootName.c_str(), DockSpaceId);
			ImGui::DockBuilderDockWindow(MaterialEditorRootName.c_str(), DockSpaceId);
			ImGui::DockBuilderFinish(DockSpaceId);
		}

		auto DrawWorkspaceHost(FEditorWorkspaceManager& WorkspaceManager) -> void
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
			if (ImGui::BeginMenuBar())
			{
				if (ActiveWorkspace) ActiveWorkspace->DrawMainMenu();
				if (ImGui::BeginMenu("Window"))
				{
					const FEditorWorkspaceTypeId LevelEditorType{"LevelEditor"};
					const auto LevelDocument = std::ranges::find(
						WorkspaceManager.GetDocuments(), LevelEditorType, &FEditorDocumentTab::WorkspaceType
					);
					const bool bLevelEditorOpen = LevelDocument != WorkspaceManager.GetDocuments().end();
					if (ImGui::MenuItem("Level Editor", nullptr, bLevelEditorOpen))
					{
						if (bLevelEditorOpen) WorkspaceManager.ActivateDocument(LevelDocument->Id);
						else
						{
							WorkspaceManager.OpenDocument({
								.WorkspaceType = LevelEditorType,
								.DocumentKey = "LevelEditor",
								.Label = "Level Editor",
								.bClosable = true,
							});
						}
					}
					if (ActiveWorkspace) ActiveWorkspace->DrawWindowMenu();
					ImGui::EndMenu();
				}
				ImGui::EndMenuBar();
			}

			const ImVec2 DockSpaceSize = ImGui::GetContentRegionAvail();
			const ImGuiID DockSpaceId = EditorWorkspaceUI::MakeEditorHostDockSpaceId(EditorHostLayoutVersion);
			const bool bNeedsDefaultLayout = ImGui::DockBuilderGetNode(DockSpaceId) == nullptr;
			if (bNeedsDefaultLayout)
			{
				// DockBuilder must finish before DockSpace submission so the new tree retains this frame's host window.
				BuildDefaultEditorHostLayout(DockSpaceId, DockSpaceSize);
			}
			EditorWorkspaceUI::SubmitEditorHostDockSpace(EditorHostLayoutVersion, DockSpaceSize, ImGuiDockNodeFlags_NoWindowMenuButton);

			for (const std::shared_ptr<IEditorWorkspace>& Workspace : WorkspaceManager.GetRegisteredWorkspaces())
			{
				if (Workspace->DrawWorkspace(Workspace == ActiveWorkspace))
					WorkspaceManager.ActivateWorkspace(Workspace->GetWorkspaceType());
			}

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
		FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");
		const FIntPoint WindowSize{LevelEditorModule.GetWindowWidth(), LevelEditorModule.GetWindowHeight()};
		MonaImGui::SetGlobalUIScale(LevelEditorModule.GetUIScale());
		FLevelEditorModule* LevelEditorModulePtr = &LevelEditorModule;
		auto RootWindow = std::make_shared<MWindow>();
		MonaImGui::BindMainViewportToWindow(RootWindow);

		auto EditorRootWidget = std::make_shared<MFunctionWidget>();
		auto WorkspaceManager = std::make_shared<FEditorWorkspaceManager>();
		auto bWorkspaceReady = std::make_shared<bool>(false);
		auto ProjectBrowser = std::make_shared<FProjectBrowser>();
		const std::weak_ptr<MWindow> WeakRootWindow = RootWindow;
		if (HasCurrentProject())
		{
			*bWorkspaceReady = LevelEditorModule.RegisterLevelEditorWorkspace(*WorkspaceManager);
			if (!*bWorkspaceReady) ProjectBrowser->SetError("Could not initialize the Level Editor workspace.");
			else ProjectBrowser->RecordCurrentProject();
		}

		RootWindow->SetTitle(GetCurrentProject() ? std::format("Durin Editor - {}", GetCurrentProject()->Name) : "Durin Editor - Project Browser");
		RootWindow->ReshapeWindow({100.0f, 100.0f}, {static_cast<float>(WindowSize.x), static_cast<float>(WindowSize.y)});

		ProjectBrowser->SetOpenProject([WorkspaceManager, bWorkspaceReady, WeakRootWindow, LevelEditorModulePtr](std::string_view ProjectFile, std::string& OutError) {
			const std::array<std::string_view, 2> Arguments = {"--project", ProjectFile};
			if (!InitializeCurrentProject(Arguments, &OutError)) return false;
			PathUtilities::InitDefaultMountPoints();
			*bWorkspaceReady = LevelEditorModulePtr->RegisterLevelEditorWorkspace(*WorkspaceManager);
			if (!*bWorkspaceReady)
			{
				OutError = "Could not initialize the Level Editor workspace.";
				return false;
			}
			if (const std::shared_ptr<MWindow> RootWindow = WeakRootWindow.lock())
				RootWindow->SetTitle(std::format("Durin Editor - {}", GetCurrentProject()->Name));
			return true;
		});

		EditorRootWidget->Construct([WorkspaceManager, bWorkspaceReady, ProjectBrowser]() {
			if (*bWorkspaceReady)
			{
				DrawWorkspaceHost(*WorkspaceManager);
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

		if (LevelEditorModule.IsWindowMaximized())
		{
			RootWindow->MaximizeWindow();
		}
		RootWindow->ShowWindow();
	}
} // namespace Durin
