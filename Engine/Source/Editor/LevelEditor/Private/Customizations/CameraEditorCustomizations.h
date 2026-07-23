#pragma once

#include "LevelEditorCustomizations.h"

namespace Durin
{
	auto CreateCameraComponentVisualizer() -> std::shared_ptr<IComponentEditorVisualizer>;
	auto CreateCameraDetailsCustomization() -> std::shared_ptr<IObjectDetailsCustomization>;
}
