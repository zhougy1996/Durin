#pragma once

#include "TextureEditorAPI.h"

namespace Durin::Editor::Texture
{
	// Installs Texture2D and TextureCube handlers into DurinEd's mounted-source
	// relocation transaction pipeline. Repeated registration is idempotent.
	TEXTUREEDITOR_API auto RegisterTextureSourceRelocation() -> bool;
	// Removes both handlers when present and prevents future relocation callbacks.
	TEXTUREEDITOR_API auto UnregisterTextureSourceRelocation() -> void;
}
