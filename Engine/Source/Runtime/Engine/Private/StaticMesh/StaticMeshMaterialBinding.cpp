#include "StaticMesh/StaticMeshMaterialBinding.h"

#include "DObject/DObjectGlobals.h"
#include "Materials/MaterialInterface.h"
#include "StaticMesh/StaticMeshDerivedData.h"

namespace Durin
{
	auto ValidateStaticMeshMaterialOverrides(
		std::span<const TObjectPtr<DMaterialInterface>> Overrides,
		std::string_view ConsumerName,
		std::string& OutError) -> bool
	{
		if (Overrides.size() > MaximumStaticMeshMaterialSlots)
		{
			OutError = std::format("A {} contains {} positional material entries, exceeding the limit of {}.",
				ConsumerName, Overrides.size(), MaximumStaticMeshMaterialSlots);
			return false;
		}
		for (size_t Index = 0; Index < Overrides.size(); ++Index)
		{
			if (Overrides[Index]
				&& !Cast<DMaterialInterface>(reinterpret_cast<DObject*>(Overrides[Index].Get())))
			{
				OutError = std::format("A {} contains an incompatible object at material index {}.",
					ConsumerName, Index);
				return false;
			}
		}
		return true;
	}

	auto TrimTrailingNullStaticMeshMaterialOverrides(
		std::vector<TObjectPtr<DMaterialInterface>>& Overrides) -> void
	{
		while (!Overrides.empty() && !Overrides.back()) Overrides.pop_back();
	}
}
