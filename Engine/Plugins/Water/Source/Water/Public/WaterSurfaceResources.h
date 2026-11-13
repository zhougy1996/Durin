#pragma once

#include "CoreMinimal.h"
#include "WaterAPI.h"
#include "WaterSurfaceTypes.h"
#include "Materials/MaterialProgramCompiler.h"
#include "Materials/MaterialRenderProxy.h"

namespace Durin
{
	struct FStaticMeshRenderData;
	// Detached, ordinary material IR; the development Game only decodes its compiled form.
	WATER_API auto MakeWaterMaterialInput() -> MIR::FCompilerInput;
	WATER_API auto MakeWaterGrid(const FWaterSurfaceSettings& Settings) -> std::unique_ptr<FStaticMeshRenderData>;
	WATER_API auto CreateWaterMaterial(ERHIFeatureLevel FeatureLevel) -> std::expected<FMaterialRenderProxyRef, std::string>;
#if DURIN_WITH_EDITOR
	// Explicit authoring operation. Output is not written by ordinary component registration.
	WATER_API auto PrecompileWaterMaterial(const std::filesystem::path& OutputDirectory) -> std::expected<void, std::string>;
#endif
}
