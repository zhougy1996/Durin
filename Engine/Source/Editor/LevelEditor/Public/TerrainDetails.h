#pragma once

#include "LevelEditorAPI.h"

namespace Durin::Editor::Level
{
	class IObjectDetailsCustomization;

	// Creates the Terrain component customization for authored controls and read-only health facts.
	LEVELEDITOR_API auto CreateTerrainDetailsCustomization()
		-> std::shared_ptr<IObjectDetailsCustomization>;
}
