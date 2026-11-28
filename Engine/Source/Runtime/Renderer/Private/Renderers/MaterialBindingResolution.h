#pragma once

#include "CoreMinimal.h"

#include "Materials/MaterialRenderTypes.h"

namespace Durin::RendererPrivate
{
	auto SelectMeshMaterial(
		FMaterialRenderData& Material,
		std::string_view DiagnosticResource
	) -> bool;
	auto IsPreparedMaterialReady(
		const FMaterialRenderData& Material,
		std::string_view DiagnosticResource
	) -> bool;
}
