#pragma once

#include "CoreMinimal.h"
#include "LevelEditorAPI.h"
#include "Viewport/TransformGizmoTypes.h"

namespace Durin::LevelEditor
{
	LEVELEDITOR_API auto MakeActorTransformGizmoTargets(const FLevelEditorContext& Context) -> FTransformGizmoTargetSet;
} // namespace Durin::LevelEditor
