#pragma once

#include "CoreMinimal.h"

#include "LevelEditorAPI.h"

namespace Durin::Editor::Level
{
	class IObjectDetailsCustomization;

	LEVELEDITOR_API auto CreateVolumetricCloudDetailsCustomization()
		-> std::shared_ptr<IObjectDetailsCustomization>;
}
