#pragma once

#include "CoreMinimal.h"
#include "PSOPrecacheMaterial.h"
#include "RenderResourceCreation.h"
#include "RendererAPI.h"

namespace Durin
{
	namespace RendererPrivate
	{
		RENDERER_API auto CollectMaterialPSOs(const FMaterialPSOPrecacheParams& Params,
			FMaterialPSOPrecacheCollection& Collection, const FRenderResourceGeneration& Generation)
			-> std::expected<bool, std::string>;
	}
}
