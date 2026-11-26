#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialRenderTypes.h"

namespace Durin::RendererPrivate
{
	inline auto UsesGBuffer(const FMaterialPlanningPassIdentity& Material, bool bSupportsGBuffer) -> bool
	{
		return bSupportsGBuffer && Material.ShaderMap.BlendMode != EMaterialBlendMode::Translucent
			&& (Material.ShaderMap.ShadingModel == EMaterialShadingModel::DefaultLit
				|| Material.ShaderMap.ShadingModel == EMaterialShadingModel::ClearCoat);
	}
}
