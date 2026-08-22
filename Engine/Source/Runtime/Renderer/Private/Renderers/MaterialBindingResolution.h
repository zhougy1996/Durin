#pragma once

#include "Materials/MaterialRenderTypes.h"

#include <string_view>

namespace Durin::RendererPrivate
{
	auto ResolveMaterialBinding(
		FMaterialRenderData& Material,
		FMaterialRenderBinding& OutBinding,
		std::string_view DiagnosticResource
	) -> bool;
}
