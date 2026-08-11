#pragma once

#include "LevelEditorCustomizations.h"

namespace Durin::Editor::Level
{
	auto CreateDirectionalLightComponentVisualizer() -> std::shared_ptr<IComponentEditorVisualizer>;
}
