#pragma once

#include "Interfaces/MainFrame.h"

namespace Durin::Editor::MainFrame
{
	struct FBootstrapContext;
}

namespace Durin
{
	// Owns the editor host window and workspace manager for the process.
	class FMainFrameModule : public IMainFrameModule
	{
	public:
		FMainFrameModule() = default;
		~FMainFrameModule() = default;

		auto StartupModule(FModuleContext& Context) -> void override;
		auto ShutdownModule(FModuleShutdownContext& Context) -> void override;
		auto CreateDefaultFrame() -> void override;
		auto DestroyDefaultFrame() -> void override;
		auto AdvanceDefaultBootstrap(bool bFirstPresentAvailable)
			-> Editor::MainFrame::FBootstrapProgress override;
		auto GetDefaultBootstrapProgress() const
			-> Editor::MainFrame::FBootstrapProgress override;
		auto GetDefaultBootstrapState() const
			-> Editor::MainFrame::EBootstrapState override;
		auto GetDefaultDocumentState() const
			-> Editor::MainFrame::EDefaultDocumentState override;

	private:
		std::shared_ptr<Editor::MainFrame::FBootstrapContext> BootstrapContext;
	};
}
