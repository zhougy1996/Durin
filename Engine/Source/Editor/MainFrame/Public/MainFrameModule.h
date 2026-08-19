#pragma once

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
			std::shared_ptr<MWindow> StartupWindow) -> void override;
		auto DestroyEditorHost() -> void override;
		auto AdvanceBootstrap(bool bFirstPresentAvailable)
			-> Editor::Host::FBootstrapProgress override;
		auto GetBootstrapProgress() const
			-> Editor::Host::FBootstrapProgress override;
		auto GetBootstrapState() const
			-> Editor::Host::EBootstrapState override;
		auto GetDefaultDocumentState() const
			-> Editor::Host::EDefaultDocumentState override;

	private:
		std::shared_ptr<Editor::MainFrame::FBootstrapContext> BootstrapContext;
	};
}
