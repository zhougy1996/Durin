#pragma once

#include "CoreMinimal.h"

namespace Durin
{
	class MWindow;
	class DEditorEngine;

	// Exposes the MainFrame module's lifecycle services without its concrete UI implementation.
	class IMainFrameModule : public IModuleInterface
	{
	public:
		virtual auto CreateDefaultMainFrame(
			DEditorEngine& Editor, std::shared_ptr<MWindow> StartupWindow) -> void = 0;
		virtual auto DestroyMainFrame() -> void = 0;
		virtual auto IsShellPresented() const -> bool = 0;
		// Synchronously registers workspaces after project schema and content initialization.
		virtual auto InitializeWorkspaces(std::string& OutError) -> bool = 0;
		// Normal-frame updates after tool/script and world ticks; independent of drawing.
		virtual auto TickMainFrame(float DeltaSeconds) -> void = 0;
	};
}
