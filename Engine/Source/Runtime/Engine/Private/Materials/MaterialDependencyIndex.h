#pragma once

#include "DObject/ObjectKey.h"
#include <unordered_map>
#include <unordered_set>

namespace Durin
{
	class DMaterialInterface;
	class FObjectReplacementMap;
	namespace Private
	{
		// Derived, non-owning edges. Canonical Parent storage remains authoritative.
		struct FMaterialDependencyIndex
		{
			std::unordered_map<FObjectKey, FObjectKey> Parents;
			std::unordered_map<FObjectKey, std::unordered_set<FObjectKey>> Children;
			uint64 Revision = 1;
		};
		auto GetMaterialDependencyIndex() -> FMaterialDependencyIndex&;
		auto RefreshMaterialDependency(DMaterialInterface& Material) -> void;
		auto RemoveMaterialDependency(DMaterialInterface& Material) -> void;
		// Replacement prepares allocations before its non-failing commit.
		auto PrepareMaterialDependencyReplacement(const FObjectReplacementMap& Map)
			-> FMaterialDependencyIndex;
	}
}
