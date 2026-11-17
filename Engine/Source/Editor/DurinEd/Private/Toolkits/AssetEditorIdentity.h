#pragma once

#include "CoreMinimal.h"
#include "DObject/ObjectKey.h"
#include "Toolkits/AssetEditor.h"
#include "Toolkits/AssetEditorToolkit.h"

namespace Durin
{
	// Uniqueness uses family, method, host mode and role. Primary status and an
	// explicit host additionally constrain reuse; asset matching is chosen by the caller.
	inline auto MatchesAssetEditorIdentity(const FAssetEditorToolkit& Existing, const FAssetOpenArgs& Args,
		const DClass* EditorClass, const FAssetEditorToolkitId* DirectToolkitId = nullptr) -> bool
	{
		if (Existing.GetOpenMethod() != Args.OpenMethod || Existing.GetToolkitMode() != Args.ToolkitMode
			|| Existing.GetInstanceKey() != Args.GetNormalizedRole()) return false;
		const auto* Owner = Existing.GetOwningAssetEditor();
		return EditorClass ? Owner && Owner->GetClass() == EditorClass
			: !Owner && DirectToolkitId && Existing.GetToolkitId() == *DirectToolkitId;
	}

	// Asset identity ignores selection ordering and repeated inputs.
	inline auto HaveSameEditedAssets(std::span<DObject* const> Left, std::span<DObject* const> Right) -> bool
	{
		std::unordered_set<FObjectKey, FObjectKeyHash> Keys;
		for (auto* Object : Left) Keys.emplace(Object);
		std::unordered_set<FObjectKey, FObjectKeyHash> Other;
		for (auto* Object : Right) Other.emplace(Object);
		return Keys == Other;
	}
}
