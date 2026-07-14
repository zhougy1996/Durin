#include "MainFrameModule.h"

#include "Mona.h"
#include "LevelEditorModule.h"
#include "MonaImGui.h"
#include "Dialogs/FileDialog.h"
#include "Misc/Paths.h"
#include "Misc/Project.h"

#include "Widgets/MFunctionWidget.h"

namespace Durin
{
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
		auto LevelEditorWidget = std::make_shared<std::shared_ptr<MWidget>>();
		std::shared_ptr<std::string> ProjectBrowserError = std::make_shared<std::string>();
		const std::weak_ptr<MWindow> WeakRootWindow = RootWindow;
		if (HasCurrentProject()) *LevelEditorWidget = LevelEditorModule.CreateLevelEditorWidget();

		RootWindow->SetTitle(GetCurrentProject() ? std::format("Durin Editor - {}", GetCurrentProject()->Name) : "Durin Editor - Project Browser");
		RootWindow->ReshapeWindow({100.0f, 100.0f}, {static_cast<float>(WindowSize.x), static_cast<float>(WindowSize.y)});

		EditorRootWidget->Construct([LevelEditorWidget, ProjectBrowserError, WeakRootWindow, LevelEditorModulePtr]() {
			if (*LevelEditorWidget != nullptr)
			{
				(*LevelEditorWidget)->Draw();
				return;
			}
			ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
			ImGui::SetNextWindowSize(ImVec2(640.0f, 420.0f), ImGuiCond_Once);
			if (ImGui::Begin("Open a Durin Project", nullptr, ImGuiWindowFlags_NoCollapse))
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
							*LevelEditorWidget = LevelEditorModulePtr->CreateLevelEditorWidget();
							if (const std::shared_ptr<MWindow> RootWindow = WeakRootWindow.lock())
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
