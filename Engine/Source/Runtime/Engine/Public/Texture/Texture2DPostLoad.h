#pragma once

#include "EngineAPI.h"

namespace Durin
{
	class DTexture2D;

	using FTexture2DUncookedPostLoadHandler =
		std::function<bool(DTexture2D&, std::string&)>;

	// Installs the editor-authoring policy used for uncooked Texture2D loads.
	// Runtime cooked loading remains owned by DTexture2D.
	ENGINE_API auto RegisterTexture2DUncookedPostLoadHandler(
		FTexture2DUncookedPostLoadHandler Handler) -> bool;
	ENGINE_API auto UnregisterTexture2DUncookedPostLoadHandler() -> void;
	ENGINE_API auto InvokeTexture2DUncookedPostLoadHandler(
		DTexture2D& Texture, std::string& OutError) -> bool;
}
