#pragma once

#include "TextureEditorAPI.h"

namespace Durin::Editor::Texture
{
	// Installs the Texture2D build-setting adapter into DurinEd's reflected
	// property transaction pipeline. Repeated registration is idempotent.
	TEXTUREEDITOR_API auto RegisterTexture2DPropertyEditing() -> bool;
	// Removes the adapter when present and prevents future property callbacks.
	TEXTUREEDITOR_API auto UnregisterTexture2DPropertyEditing() -> void;
}
