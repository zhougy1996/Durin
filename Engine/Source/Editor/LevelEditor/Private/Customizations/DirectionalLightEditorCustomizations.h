#pragma once

#include "CoreMinimal.h"

#include "LevelEditorCustomizations.h"

namespace Durin::Editor::Level
{
	auto CreateDirectionalLightComponentVisualizer() -> std::shared_ptr<IComponentEditorVisualizer>;
}
