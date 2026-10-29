#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialPSOPrecache.h"
#include "RenderResourceCreation.h"
#include "RendererAPI.h"
#include "Renderers/MeshPipelineDescription.h"

namespace Durin
{
	class FShaderType;
	class FMeshMaterialShaderMap;
	namespace RendererPrivate
	{
		class FMeshVertexFactoryImplementation;
		// Renderer supports both selectable scene paths. HitProxy and alternate
		// raster/depth modes are optional; required work covers the preparation mode.
		struct FMaterialPSOPrecacheConfig
		{
			bool bForward = true;
			bool bHybrid = true;
			bool bHitProxy = true;
			bool bSupportsWireframe = true;
		};
		struct FMaterialPSOPrecachePass
		{
			EMeshPipelinePass Pass;
			FMeshPipelineRasterOptions Raster;
			bool bRequired;
		};
		RENDERER_API auto SelectMaterialPSOPrecachePasses(const FMaterialPSOPrecacheParams& Params,
			bool bSupportsGBuffer, const FMaterialPSOPrecacheConfig& Config = {})
			-> std::vector<FMaterialPSOPrecachePass>;

		RENDERER_API auto SelectMaterialPSOPrecachePasses(const FMaterialPSOPrecacheParams& Params,
			const FMeshVertexFactoryImplementation& Factory, const FMaterialPSOPrecacheConfig& Config = {})
			-> std::vector<FMaterialPSOPrecachePass>;

		struct FMaterialPSOFragmentShader
		{
			FShaderType* Type = nullptr;
			FRHIShader* (*Resolve)(const FMeshMaterialShaderMap&) = nullptr;
		};
		auto GetGBufferPSOPrecacheFragmentShader() -> FMaterialPSOFragmentShader;
		auto GetHitProxyPSOPrecacheFragmentShader() -> FMaterialPSOFragmentShader;
		RENDERER_API auto CollectMaterialPSOs(const FMaterialPSOPrecacheParams& Params,
			FMaterialPSOPrecacheCollection& Collection, const FRenderResourceGeneration& Generation)
			-> std::expected<bool, std::string>;
	}
}
