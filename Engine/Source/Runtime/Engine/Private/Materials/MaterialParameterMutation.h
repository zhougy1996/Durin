#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialTypes.h"

namespace Durin
{
	inline auto ValidateNamedMaterialParameter(const FMaterialParameterDefinition* Definition,
		EMaterialParameterType Type) -> std::expected<void, std::string>
	{
		if (Definition && Definition->Type == Type) return {};
		return std::unexpected(Definition
			? std::format("Material declaration type is invalid. [parameter {}]", Definition->Id.ToString())
			: std::string("Material declaration does not exist."));
	}

}
