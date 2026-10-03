#pragma once

#include "CoreMinimal.h"

#include "Materials/MaterialTypes.h"

namespace Durin::Private
{
	template <typename TRange>
	auto IsFiniteMaterialComponents(const TRange& Components) -> bool
	{
		return std::ranges::all_of(Components, [](auto Component) {
			return std::isfinite(Component);
		});
	}

	inline auto IsFiniteMaterialParameterValue(const FMaterialParameterValue& Value) -> bool
	{
		switch (Value.GetType())
		{
		case EMaterialParameterType::Scalar:
			return IsFiniteMaterialComponents(std::array{Value.GetScalar()});
		case EMaterialParameterType::Vector2:
			return IsFiniteMaterialComponents(std::array{Value.GetVector2().x, Value.GetVector2().y});
		case EMaterialParameterType::Vector:
			return IsFiniteMaterialComponents(std::array{Value.GetVector().x, Value.GetVector().y, Value.GetVector().z});
		case EMaterialParameterType::Vector4:
			return IsFiniteMaterialComponents(std::array{Value.GetVector4().x, Value.GetVector4().y,
				Value.GetVector4().z, Value.GetVector4().w});
		case EMaterialParameterType::Texture:
			return true;
		}
		return false;
	}
}
