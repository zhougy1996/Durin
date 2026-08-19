#pragma once

#include "Renderers/DirectionalShadowView.h"
#include "Renderers/StaticMeshRenderPreparation.h"

namespace Durin::RendererPrivate
{
	inline auto GetMaterialSamplerKey(const FMaterialSamplerState& State) -> size_t
	{
		return static_cast<size_t>(State.MinFilter)
			+ 6 * (static_cast<size_t>(State.MagFilter)
				+ 2 * (static_cast<size_t>(State.AddressU)
					+ 3 * static_cast<size_t>(State.AddressV)));
	}

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
		const FEffectiveStaticMeshPipelineKey& Source
	) -> FEffectiveStaticMeshPipelineKey
	{
		FEffectiveStaticMeshPipelineKey Result = Source;
		Result.Rasterizer = MakeShadowRasterizerState(Source.Rasterizer);
		// Bias magnitudes are dynamic draw state and do not identify pipeline slots.
		Result.Rasterizer.DepthBiasConstantFactor = 0.0f;
		Result.Rasterizer.DepthBiasSlopeFactor = 0.0f;
		Result.Rasterizer.DepthBiasClamp = 0.0f;
		Result.Depth.bEnableTest = true;
		Result.Depth.bEnableWrite = true;
		Result.Depth.CompareOp = ERHIDepthCompareOp::Less;
		Result.ColorBlend = {};
		return Result;
	}
}
