#pragma once

#include "CoreMinimal.h"
#include "PropertyEditor/DetailCustomization.h"

#include "LevelEditorAPI.h"

namespace Durin::Editor::Level
{

	LEVELEDITOR_API auto CreateVolumetricCloudDetailsCustomization()
		-> std::shared_ptr<IDetailCustomization>;
}
