#pragma once

#include "CoreMinimal.h"

#include "Editor/IMainFrameModule.h"

namespace Durin::Editor::MainFrame
{
	struct FMainFrameContext;
	struct FMainFrameToolsetState;
}

namespace Durin
{
	// Owns the editor host window and workspace manager for the process.
	class FMainFrameModule : public IMainFrameModule
	{
	public:
		FMainFrameModule() = default;
		~FMainFrameModule() = default;

		auto StartupModule() -> void override;
		auto ShutdownModule() -> void override;
		auto CreateDefaultMainFrame(
			DEditorEngine& Editor, std::shared_ptr<MWindow> StartupWindow) -> void override;
		auto DestroyMainFrame() -> void override;
		auto IsShellPresented() const -> bool override;
		auto InitializeWorkspaces(std::string& OutError) -> bool override;
		auto TickMainFrame(float DeltaSeconds) -> void override;

	private:
		std::shared_ptr<Editor::MainFrame::FMainFrameToolsetState> Toolsets;
		std::shared_ptr<Editor::MainFrame::FMainFrameContext> MainFrameContext;
	};
}
