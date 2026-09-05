#pragma once

#include "LevelEditorAPI.h"

namespace Durin::Editor::Level
{
	enum class EImportDialogType : uint8
	{
		Scene,
	};

	// Allows Level-owned workflows to notify and reveal through a host-owned browser.
	struct FContentBrowserCallbacks
	{
		std::function<bool(std::string_view)> RevealAsset;
		std::function<bool(std::string_view)> RevealDirectory;
		std::function<bool()> NotifyMountedContentChanged;
	};
} // namespace Durin::Editor::Level
