#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialPSOPrecache.h"
#include "RenderResourceCreation.h"
#include "RendererAPI.h"

namespace Durin
{
	class FShaderType;
	class FMeshMaterialShaderMap;
	namespace RendererPrivate
	{
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
