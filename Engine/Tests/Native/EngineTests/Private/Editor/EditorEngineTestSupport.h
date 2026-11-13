#pragma once

#include "CoreMinimal.h"
#include "Editor/EditorEngine.h"
#include "NativeAssetRuntimeTestSupport.h"
#include "NativeTestSupport.h"
#include "Project/Project.h"

namespace Durin
{
	// Workspace bootstrap tests supply a real project with no default document.
	struct FWorkspaceProjectTestScope
	{
		FWorkspaceProjectTestScope()
		{
			requiref(!HasCurrentProject(), "Workspace fixture requires an unbound project.");
			const auto Root = Testing::GetTestWorkDirectory() / "WorkspaceProject";
			std::filesystem::create_directories(Root / "Configs");
			const auto Project = Root / "Workspace.dproject";
			{ std::ofstream File(Project); File << R"({"ProjectName":"Workspace","BaseModules":[]})"; }
			{ std::ofstream File(Root / "Configs" / "Project.yaml"); File << "Game:\n  DefaultLevel: ''\n"; }
			requiref(InitializeCurrentProject({.RequestedProjectFile = Project.string(), .bRecordInteractiveHistory = false}), "Workspace fixture project admission failed.");
		}
		~FWorkspaceProjectTestScope() { (void)InitializeCurrentProject({.bOpenProjectBrowser = true}); }
	};

	// Shared native-test access; production editor startup remains explicit.
	struct FEditorEngineTestAccess
	{
		static auto InitializeServices(DEditorEngine& Editor) -> void
		{
			Testing::InitializeAssetSystemForTests();
			Editor.InitializeEditorSubsystems();
		}

		static auto StartEditor(DEditorEngine& Editor) -> void
		{
			Testing::InitializeAssetSystemForTests();
			const bool bActivated = Editor.ActivateEditor();
			requiref(bActivated, "Editor activation failed.");
			InitializeServices(Editor);
		}

		static auto SetEditorWorld(DEditorEngine& Editor, DWorld* World) -> void
		{ Editor.EditorWorld = World; Editor.SetWorld(World); }

		static auto SetState(DEditorEngine& Editor, EPlayState State) -> void
		{ Editor.PlayState = State; }

		static auto SetStartupState(DEditorEngine& Editor, EHostStartupState State, std::string Error = {}) -> void
		{ Editor.StartupState = State; Editor.StartupError = std::move(Error); }
		static auto InitializeProjectWorkspace(DEditorEngine& Editor) -> bool { return Editor.InitializeProjectWorkspace(); }
		static auto InitializeWorkspaces(DEditorEngine& Editor) -> bool { return Editor.InitializeWorkspaces(); }
		static auto OpenDefaultLevel(DEditorEngine& Editor) -> void { Editor.OpenDefaultLevel(); }

		static auto BindMainFrameModule(DEditorEngine& Editor, IMainFrameModule* Module) -> IMainFrameModule*
		{ return std::exchange(Editor.MainFrameModule, Module); }

		static auto SetStartupWindow(DEditorEngine& Editor, const std::shared_ptr<MWindow>& Window) -> void
		{ Editor.StartupWindow = Window; }

		static auto SetStartupViewport(DEditorEngine& Editor, std::shared_ptr<FSceneViewport> Viewport) -> void
		{ Editor.MainSceneViewport = std::move(Viewport); }

		static auto HasInitialLevelPath(const DEditorEngine& Editor) -> bool
		{ return Editor.InitialLevelPath.IsValid(); }

		static auto SetInitialLevelPath(DEditorEngine& Editor, FSoftObjectPath Path) -> void
		{ Editor.InitialLevelPath = std::move(Path); }

		static auto StartPlaySession(
			DEditorEngine& Engine,
			const FPlayRequest& Request,
			DClass* GameModeClass,
			std::string* OutError = nullptr) -> bool
		{
			return Engine.StartPlaySessionInternal(
				Request, std::optional<DClass*>{GameModeClass}, OutError);
		}

		static auto ConfigureMouseCapture(
			DEditorEngine& Engine,
			EPlayDestination Destination = EPlayDestination::EmbeddedViewport) -> void
		{
			Engine.PlayState = EPlayState::Playing;
			Engine.PlayDestination = Destination;
		}

		static auto InitializePlayWindowViewportClient(
			DEditorEngine& Engine,
			const FViewportClient* SourceClient) -> void
		{
			Engine.InitializePlayWindowViewportClient(SourceClient);
		}

		static auto KeyDown(DEditorEngine& Engine, const std::shared_ptr<FGenericWindow>& Window, EKey Key, bool bRepeat = false) -> bool
		{
			return Engine.HandleGameInputKeyDown(Window, Key, bRepeat);
		}

		static auto Focus(DEditorEngine& Engine, const std::shared_ptr<FGenericWindow>& Window, bool bFocused) -> void
		{
			Engine.HandleGameInputWindowFocus(Window, bFocused);
		}

		static auto MouseDown(DEditorEngine& Engine, const std::shared_ptr<FGenericWindow>& Window, EMouseButton Button) -> bool
		{
			return Engine.HandleGameInputMouseDown(Window, Button);
		}

		static auto Close(DEditorEngine& Engine, const std::shared_ptr<FGenericWindow>& Window) -> void
		{
			Engine.HandleGameInputWindowClose(Window);
		}
	};
}
