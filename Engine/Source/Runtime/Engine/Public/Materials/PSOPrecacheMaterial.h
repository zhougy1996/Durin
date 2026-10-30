#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialPSOPrecache.h"

namespace Durin
{
	struct FMaterialResource;

	// UE-shaped collection boundary. Implementations append descriptions only;
	// compilation, submission and request ownership belong to the host.
	class IPSOCollector
	{
	public:
		virtual ~IPSOCollector() = default;
		virtual auto CollectPSOInitializers(const FSceneRenderPreparationConfig& SceneConfig,
			const FMaterialResource& Material, const FPSOPrecacheVertexFactoryData& VertexFactoryData,
			const FPSOPrecacheParams& PrecacheParams, FPSOPrecacheDataArray& PSOInitializers) -> void = 0;
	};
}
