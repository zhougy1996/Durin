#pragma once

#include "LevelEditorCustomizations.h"

namespace Durin::Editor::Level
{
	auto CreateCameraComponentVisualizer() -> std::shared_ptr<IComponentEditorVisualizer>;
	auto CreateCameraDetailsCustomization() -> std::shared_ptr<IObjectDetailsCustomization>;
}
