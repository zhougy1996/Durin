#pragma once

#include "Editor/EditorWorkspace.h"

namespace Durin::LevelEditorWorkspace
{
	inline const FEditorWorkspaceTypeId Type{"LevelEditor"};
	inline constexpr uint32 LayoutVersion = 1;
	inline constexpr const char* RootKey = "LevelEditor";
}
