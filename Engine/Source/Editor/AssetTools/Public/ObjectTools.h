#pragma once

#include "CoreMinimal.h"
#include "AssetToolsAPI.h"

namespace Durin { class DObject; }
namespace Durin::ObjectTools
{
	using FDuplicatePrompt = std::function<std::optional<std::string>(std::string_view Destination)>;
	ASSETTOOLS_API auto SetDuplicatePrompt(FDuplicatePrompt Prompt) -> void;
	// Chooses unique identities and retains successful Dirty copies; it never forces a save.
	ASSETTOOLS_API auto DuplicateObjects(std::span<DObject* const> SelectedObjects,
		std::string_view SourcePath = {}, std::string_view DestinationPath = {},
		bool bOpenDialog = true, std::vector<DObject*>* OutNewObjects = nullptr) -> void;
}
