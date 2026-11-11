#pragma once

#include "CoreMinimal.h"
#include "MonaImGui.h"

namespace Durin { class MWindow; }
namespace Durin::MainFrame
{
	class FMainFrameSettings;
	// Presents host and feature settings in a searchable two-pane window.
	class FEditorSettingsWindow
	{
	public:
		auto Draw(FMainFrameSettings& Settings, MWindow& RootWindow) -> void;
	private:
		friend struct FEditorSettingsWindowTestAccess;
		bool bOpen = false;
		std::string SelectedPage = "editor.appearance";
		std::string Error;
		ImGuiTextFilter Search;
	};
}
