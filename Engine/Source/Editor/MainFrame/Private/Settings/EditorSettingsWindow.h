#pragma once

#include "CoreMinimal.h"
#include "MonaImGui.h"

namespace Durin { class MWindow; }
namespace Durin::Editor::MainFrame
{
	class FHostSettings;
	// Presents host and feature settings in a searchable two-pane window.
	class FEditorSettingsWindow
	{
	public:
		auto Draw(FHostSettings& Settings, MWindow& RootWindow) -> void;
	private:
		friend struct FEditorSettingsWindowTestAccess;
		bool bOpen = false;
		std::string SelectedPage = "editor.appearance";
		std::string Error;
		ImGuiTextFilter Search;
	};
}
