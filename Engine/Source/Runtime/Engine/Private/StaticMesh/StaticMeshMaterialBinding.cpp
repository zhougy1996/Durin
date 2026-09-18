#include "StaticMesh/StaticMeshMaterialBinding.h"

#include "DObject/DObjectGlobals.h"
#include "Materials/MaterialInterface.h"
#include "StaticMesh/StaticMeshDerivedData.h"

namespace Durin
{
	auto FormatStaticMeshMaterialOverrideError(const FStaticMeshMaterialOverrideError& Error,
		std::string_view ConsumerName) -> std::string
	{
		switch (Error.Code)
		{
		case EStaticMeshMaterialOverrideError::None: return {};
		case EStaticMeshMaterialOverrideError::TooManySlots:
			return std::format("A {} contains {} positional material entries, exceeding the limit of {}.",
				ConsumerName, Error.ActualCount, Error.MaximumCount);
		case EStaticMeshMaterialOverrideError::IncompatibleObject:
			return std::format("A {} contains an incompatible object at material index {}.", ConsumerName, Error.Index);
		}
		return {};
	}

	auto ValidateStaticMeshMaterialOverrides(
		std::span<const TObjectPtr<DMaterialInterface>> Overrides) -> FStaticMeshMaterialOverrideResult
	{
		if (Overrides.size() > MaximumMeshMaterialSlots)
			return {{.Code = EStaticMeshMaterialOverrideError::TooManySlots,
				.ActualCount = Overrides.size(), .MaximumCount = MaximumMeshMaterialSlots}};
		for (size_t Index = 0; Index < Overrides.size(); ++Index)
		{
			auto* Object = reinterpret_cast<DObject*>(Overrides[Index].Get());
			if (Object && !Cast<DMaterialInterface>(Object))
				return {{.Code = EStaticMeshMaterialOverrideError::IncompatibleObject,
					.Index = Index, .ObjectPath = Object->GetObjectPath(),
					.ActualType = Object->GetClass()->GetQualifiedName().ToString()}};
		}
		return {};
	}
}
