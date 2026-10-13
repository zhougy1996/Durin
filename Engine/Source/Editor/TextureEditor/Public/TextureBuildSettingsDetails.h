#pragma once
#include "CoreMinimal.h"
#include "TextureEditorAPI.h"
#include "PropertyEditor/DetailCustomization.h"

namespace Durin::Editor::Texture
{
	TEXTUREEDITOR_API auto CreateTextureBuildSettingsDetails() -> std::shared_ptr<IDetailCustomization>;
}
