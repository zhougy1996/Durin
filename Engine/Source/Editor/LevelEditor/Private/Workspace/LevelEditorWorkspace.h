#pragma once

#include "CoreMinimal.h"

#include "Toolkits/AssetEditorTypes.h"

namespace Durin::LevelEditor::Workspace
{
	inline const FAssetEditorToolkitId Type{"LevelEditor"};
	inline constexpr uint32 LayoutVersion = 6;
	inline constexpr const char* RootKey = "LevelEditor";
}
