#pragma once

#include "LevelEditorCustomizations.h"

namespace Durin
{
	auto CreateSplineComponentVisualizer() -> std::shared_ptr<IComponentEditorVisualizer>;
	auto CreateSplineDetailsCustomization() -> std::shared_ptr<IObjectDetailsCustomization>;
} // namespace Durin
