#include "MainFrameModule.h"

#include "Editor/EditorWorkspace.h"
#include "Editor/EditorWorkspaceUI.h"
#include "Mona.h"
#include "LevelEditorModule.h"
#include "MonaImGui.h"
#include "Dialogs/FileDialog.h"
#include "Misc/Paths.h"
#include "Misc/Project.h"

#include "Widgets/MFunctionWidget.h"

namespace Durin
{
	namespace
	{
		constexpr uint32 EditorHostLayoutVersion = 1;

		auto BuildDefaultEditorHostLayout(ImGuiID DockSpaceId, const ImVec2& DockSpaceSize) -> void
		{
			ImGui::DockBuilderRemoveNode(DockSpaceId);
			ImGui::DockBuilderAddNode(DockSpaceId, ImGuiDockNodeFlags_DockSpace | ImGuiDockNodeFlags_NoWindowMenuButton);
			ImGui::DockBuilderSetNodeSize(DockSpaceId, DockSpaceSize);
			if (ImGuiDockNode* DockSpaceNode = ImGui::DockBuilderGetNode(DockSpaceId))
				DockSpaceNode->WindowClass = EditorWorkspaceUI::MakeEditorRootWindowClass();
			const std::string LevelEditorRootName = EditorWorkspaceUI::MakeEditorRootWindowName("Level Editor", "LevelEditor");
			ImGui::DockBuilderDockWindow(LevelEditorRootName.c_str(), DockSpaceId);
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
			if (ImGui::BeginMenuBar())
			{
				if (const FEditorDocumentTab* ActiveDocument = WorkspaceManager.GetActiveDocument())
				{
					if (const std::shared_ptr<IEditorWorkspace> Workspace = WorkspaceManager.FindWorkspace(ActiveDocument->WorkspaceType))
						Workspace->DrawMainMenu();
				}
				ImGui::EndMenuBar();
			}

			const ImVec2 DockSpaceSize = ImGui::GetContentRegionAvail();
			const ImGuiID DockSpaceId = EditorWorkspaceUI::MakeEditorHostDockSpaceId(EditorHostLayoutVersion);
			const bool bNeedsDefaultLayout = ImGui::DockBuilderGetNode(DockSpaceId) == nullptr;
			EditorWorkspaceUI::SubmitEditorHostDockSpace(EditorHostLayoutVersion, DockSpaceSize, ImGuiDockNodeFlags_NoWindowMenuButton);
			if (bNeedsDefaultLayout)
			{
				BuildDefaultEditorHostLayout(DockSpaceId, DockSpaceSize);
			}

			std::shared_ptr<IEditorWorkspace> ActiveWorkspace;
			if (const FEditorDocumentTab* ActiveDocument = WorkspaceManager.GetActiveDocument())
				ActiveWorkspace = WorkspaceManager.FindWorkspace(ActiveDocument->WorkspaceType);
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
		std::shared_ptr<std::string> ProjectBrowserError = std::make_shared<std::string>();
		const std::weak_ptr<MWindow> WeakRootWindow = RootWindow;
		if (HasCurrentProject())
		{
			*bWorkspaceReady = LevelEditorModule.RegisterLevelEditorWorkspace(*WorkspaceManager);
			if (!*bWorkspaceReady) *ProjectBrowserError = "Could not initialize the Level Editor workspace.";
		}

		RootWindow->SetTitle(GetCurrentProject() ? std::format("Durin Editor - {}", GetCurrentProject()->Name) : "Durin Editor - Project Browser");
		RootWindow->ReshapeWindow({100.0f, 100.0f}, {static_cast<float>(WindowSize.x), static_cast<float>(WindowSize.y)});

		EditorRootWidget->Construct([WorkspaceManager, bWorkspaceReady, ProjectBrowserError, WeakRootWindow, LevelEditorModulePtr]() {
			if (*bWorkspaceReady)
			{
				DrawWorkspaceHost(*WorkspaceManager);
				return;
			}
			ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
			ImGui::SetNextWindowSize(ImVec2(640.0f, 420.0f), ImGuiCond_Once);
			if (ImGui::Begin("Open a Durin Project", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking))
			{
				ImGui::TextUnformatted("Open a .dproject file to start the editor.");
				ImGui::Separator();
				if (ImGui::Button("Open Project...", ImVec2(140.0f, 0.0f)))
				{
					FFileDialogRequest Request;
					Request.ParentWindowHandle = ImGui::GetMainViewport()->PlatformHandleRaw;
					Request.Title = "Open a Durin Project";
					Request.Filters = {{"Durin Project", "*.dproject"}};
					const FFileDialogResult Result = OpenFileDialog(Request);
					if (Result.Status == EFileDialogStatus::Selected)
					{
						const std::array<std::string_view, 2> Arguments = {"--project", Result.FilePath};
						if (InitializeCurrentProject(Arguments, ProjectBrowserError.get()))
						{
							PathUtilities::InitDefaultMountPoints();
							*bWorkspaceReady = LevelEditorModulePtr->RegisterLevelEditorWorkspace(*WorkspaceManager);
							if (!*bWorkspaceReady)
							{
								*ProjectBrowserError = "Could not initialize the Level Editor workspace.";
							}
							else if (const std::shared_ptr<MWindow> RootWindow = WeakRootWindow.lock())
								RootWindow->SetTitle(std::format("Durin Editor - {}", GetCurrentProject()->Name));
						}
					}
					else if (Result.Status == EFileDialogStatus::Error) *ProjectBrowserError = Result.ErrorMessage;
				}
				if (!ProjectBrowserError->empty()) ImGui::TextColored(ImVec4(1, 0.35f, 0.35f, 1), "%s", ProjectBrowserError->c_str());
			}
			ImGui::End();
		});
		RootWindow->SetContent(EditorRootWidget);

		Mona::FMonaApplication::Get().AddWindow(RootWindow, true);
		Mona::FMonaApplication::Get().GetRenderer()->CreateViewport(RootWindow);

		if (LevelEditorModule.IsWindowMaximized())
		{
			RootWindow->MaximizeWindow();
		}
	}
} // namespace Durin
