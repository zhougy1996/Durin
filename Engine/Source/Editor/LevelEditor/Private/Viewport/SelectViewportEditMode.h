#pragma once

#include "CoreMinimal.h"
#include "LevelEditorAPI.h"
#include "EditorModeManager.h"

namespace Durin::LevelEditor
{
	LEVELEDITOR_API auto RegisterSelectViewportEditMode() -> FLevelViewportEditModeHandle;
}
