#pragma once
#include "CoreMinimal.h"
#include "MaterialEditorAPI.h"
#include "PropertyEditor/DetailCustomization.h"

namespace Durin::Editor::Material
{
	MATERIALEDITOR_API auto CreateMaterialParameterCollectionDetails() -> std::shared_ptr<IDetailCustomization>;
}
