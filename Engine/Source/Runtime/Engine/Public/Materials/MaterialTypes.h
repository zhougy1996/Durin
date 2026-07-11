#pragma once

#include "EngineAPI.h"

namespace Durin
{
	inline constexpr std::string_view MaterialParameterBaseColor = "BaseColor";
	inline constexpr std::string_view MaterialParameterOpacity = "Opacity";

	struct FMaterialRenderData
	{
		FVector4f BaseColor{0.95f, 0.62f, 0.22f, 1.0f};
	};
}
