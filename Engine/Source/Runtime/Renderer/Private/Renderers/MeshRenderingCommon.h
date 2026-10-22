#pragma once

#include "CoreMinimal.h"

#include "Renderers/DirectionalShadowView.h"
#include "Renderers/MeshRenderPreparationCommon.h"
#include "Renderers/MeshPipelineDescription.h"

namespace Durin::RendererPrivate
{
	inline auto ToRHIAddress(EMaterialSamplerAddressMode Address)
		-> ESamplerAddressMode
	{
		switch (Address)
		{
		case EMaterialSamplerAddressMode::MirroredRepeat:
			return ESamplerAddressMode::MirroredRepeat;
		case EMaterialSamplerAddressMode::ClampToEdge:
			return ESamplerAddressMode::ClampToEdge;
		case EMaterialSamplerAddressMode::Repeat:
		default:
			return ESamplerAddressMode::Repeat;
		}
	}

	inline auto MakeMaterialSamplerDesc(const FMaterialSamplerState& State)
		-> FRHISamplerDesc
	{
		FRHISamplerDesc Result;
		const uint8 Min = static_cast<uint8>(State.MinFilter);
		Result.MinFilter = (Min & 1u) != 0
			? ESamplerFilter::Linear : ESamplerFilter::Nearest;
		Result.MagFilter = State.MagFilter == EMaterialSamplerMagFilter::Linear
			? ESamplerFilter::Linear : ESamplerFilter::Nearest;
		Result.MipmapMode = Min >= 4
			? ESamplerMipmapMode::Linear : ESamplerMipmapMode::Nearest;
		Result.MaxLod = Min < 2 ? 0.0f : 1000.0f;
		Result.AddressU = ToRHIAddress(State.AddressU);
		Result.AddressV = ToRHIAddress(State.AddressV);
		Result.AddressW = ESamplerAddressMode::Repeat;
		return Result;
	}

	inline auto MakeShadowRasterizerState(const FRHIRasterizerState& Source)
		-> FRHIRasterizerState
	{
		FRHIRasterizerState Result = Source;
		Result.PolygonMode = ERHIPolygonMode::Fill;
		const bool bPreparedBias = Result.bEnableDepthBias;
		Result.bEnableDepthBias = true;
		if (!bPreparedBias)
		{
			Result.DepthBiasConstantFactor = DirectionalShadowDepthBiasConstant;
			Result.DepthBiasSlopeFactor = DirectionalShadowDepthBiasSlope;
			Result.DepthBiasClamp = DirectionalShadowDepthBiasClamp;
		}
		return Result;
	}

	inline auto MakeShadowPipelineKey(
		const FEffectiveMeshPipelineKey& Source
	) -> FEffectiveMeshPipelineKey
	{
		FEffectiveMeshPipelineKey Result = Source;
		const auto State = MakeShadowMeshPipelineState({Source.Rasterizer, Source.Depth, Source.ColorBlend});
		Result.Rasterizer = State.Rasterizer;
		Result.Depth = State.Depth;
		Result.ColorBlend = State.ColorBlend;
		return Result;
	}
}
