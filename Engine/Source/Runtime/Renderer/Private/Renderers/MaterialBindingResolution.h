#pragma once

#include "CoreMinimal.h"

#include "Materials/MaterialRenderTypes.h"

namespace Durin::RendererPrivate
{
	auto ResolveMaterialBinding(
		FMaterialRenderData& Material,
		FMaterialRenderBinding& OutBinding,
		std::string_view DiagnosticResource
	) -> bool;
	auto ResolvePreparedMaterialBinding(
		const FMaterialRenderData& Material,
		FMaterialRenderBinding& OutBinding,
		std::string_view DiagnosticResource
	) -> bool;
}
