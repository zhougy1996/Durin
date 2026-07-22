#pragma once

#include "LevelEditorCustomizations.h"

namespace Durin
{
	auto CreateActorDetailsCustomization() -> std::shared_ptr<IObjectDetailsCustomization>;
	auto CreateStaticMeshDetailsCustomization() -> std::shared_ptr<IObjectDetailsCustomization>;
}
