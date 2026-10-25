#pragma once

#include "CoreMinimal.h"
#include "Engine/HostStartup.h"

#include "Editor/EditorHost.h"

namespace Durin::Editor::MainFrame
{
	struct FBootstrapContext;
}

namespace Durin
{
	// Owns the editor host window and workspace manager for the process.
	class FMainFrameModule : public IEditorHost
	{
	public:
		FMainFrameModule() = default;
		~FMainFrameModule() = default;

		auto ShutdownModule() -> void override;
		auto CreateEditorHost(
			DEditorEngine& Editor, std::shared_ptr<MWindow> StartupWindow, std::string InitialProjectError) -> void override;
		auto DestroyEditorHost() -> void override;
		auto IsShellPresented() const -> bool override;
		auto IsScenePresented(const FViewportOutputReceipt& Receipt, uint64 Generation) const -> bool override;
		auto PublishBootstrapState(Editor::Host::EBootstrapState State, std::string Error) -> void override;
		auto ActivateWorkspaces(std::string& OutError) -> bool override;
		auto OpenDefaultDocument(std::string& OutError) -> EHostStartupState override;
		auto AdvanceDocumentOperations() -> void override;

	private:
		std::shared_ptr<Editor::MainFrame::FBootstrapContext> BootstrapContext;
	};
}
