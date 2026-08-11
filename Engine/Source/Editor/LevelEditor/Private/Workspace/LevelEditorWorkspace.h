#pragma once

#include "Editor/WorkspaceTypes.h"

namespace Durin::LevelEditorWorkspace
{
	inline const Editor::FWorkspaceTypeId Type{"LevelEditor"};
		inline constexpr uint32 LayoutVersion = 4;
	inline constexpr const char* RootKey = "LevelEditor";
}
