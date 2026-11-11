#pragma once

#include "CoreMinimal.h"

#include "LevelEditorCustomizations.h"

namespace Durin::LevelEditor
{
	auto CreateDirectionalLightComponentVisualizer() -> std::shared_ptr<IComponentEditorVisualizer>;
}
