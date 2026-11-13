#pragma once

#include "CoreMinimal.h"
#include "DObject/ObjectKey.h"

namespace Durin
{
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
