#pragma once

#include "CoreMinimal.h"
#include "LevelEditorAPI.h"
#include "LevelEditorViewportEditing.h"

namespace Durin::LevelEditor
{
	LEVELEDITOR_API auto RegisterSelectViewportEditMode() -> FLevelViewportEditModeHandle;
}
