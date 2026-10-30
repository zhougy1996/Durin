#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialPSOPrecache.h"
#include "RenderResourceCreation.h"
#include "RendererAPI.h"
#include "Renderers/MeshPipelineDescription.h"

namespace Durin
{
	namespace RendererPrivate
	{
		class FMeshVertexFactoryImplementation;
		struct FMaterialPSOPrecachePass
		{
			EMeshPipelinePass Pass;
			FMeshPipelineRasterOptions Raster;
			bool bRequired;
		};
		RENDERER_API auto SelectMaterialPSOPrecachePasses(const FMaterialPSOPrecacheParams& Params,
			bool bSupportsGBuffer, const FSceneRenderPreparationConfig& Config = {})
			-> std::vector<FMaterialPSOPrecachePass>;

		RENDERER_API auto SelectMaterialPSOPrecachePasses(const FMaterialPSOPrecacheParams& Params,
			const FMeshVertexFactoryImplementation& Factory, const FSceneRenderPreparationConfig& Config = {})
			-> std::vector<FMaterialPSOPrecachePass>;

		RENDERER_API auto CollectMaterialPSOs(const FMaterialPSOPrecacheParams& Params,
			FMaterialPSOPrecacheCollection& Collection, const FRenderResourceGeneration& Generation)
			-> std::expected<bool, std::string>;
	}
}
