#pragma once
#include "CoreMinimal.h"
#include "TextureEditorAPI.h"
#include "PropertyEditor/DetailCustomization.h"

namespace Durin::TextureEditor
{
	TEXTUREEDITOR_API auto CreateTextureBuildSettingsDetails() -> std::shared_ptr<IDetailCustomization>;
}
