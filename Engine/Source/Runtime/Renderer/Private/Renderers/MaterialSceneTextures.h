#pragma once

#include "CoreMinimal.h"
#include "RendererAPI.h"
#include "Materials/MaterialProgramCompiler.h"
#include "RDG/RDG.h"
#include "RHIShaderParameters.h"

namespace Durin
{
	// ScreenPass-style ownership: graph identity and active rectangle travel together.
	struct FScreenPassTexture
	{
		FRDGTextureHandle Texture;
		FIntPoint ViewMin{0, 0};
		FIntPoint ViewSize{0, 0};
	};
	struct alignas(16) FMaterialSceneTextureUniform
	{
		FVector4f Input0Viewport{0};
		FVector4f Input0TextureSize{0};
		FVector4f DepthViewport{0};
		FVector4f DepthTextureSize{0};
		std::array<FVector4f, 4> ProjectionRows{};
		FVector4f DepthParameters{0};
	};
	static_assert(sizeof(FMaterialSceneTextureUniform) == 144);
	struct FMaterialSceneTextureInputs
	{
		std::optional<FScreenPassTexture> PostProcessInput0;
		std::optional<FScreenPassTexture> SceneDepth;
		std::array<FVector4f, 4> ProjectionRows{};
		float ClearDepth = 1.f;
	};
	struct FMaterialSceneTexturePassParameters
	{
		std::optional<FRDGTextureParameter> PostProcessInput0;
		std::optional<FRDGTextureParameter> SceneDepth;
		FRDGColorAttachmentBinding Output;
		static RENDERER_API auto GetRDGParametersMetadata() -> const FRDGParametersMetadata*;
	};
	struct FMaterialSceneTexturePlan
	{
		FScreenPassTexture Output;
		FMaterialSceneTextureUniform Uniform;
		uint32 TextureCount = 0;
		bool bPointSampler = false;
		bool bLinearSampler = false;
		TRDGParametersRef<FMaterialSceneTexturePassParameters> Parameters;
	};
	// Validation precedes allocation. On failure the caller keeps its composed input.
	RENDERER_API auto PlanMaterialSceneTextures(FRDGBuilder& Graph,
		const FMaterialShaderEntryRequest& Entry, const FMaterialSceneTextureInputs& Inputs)
		-> std::expected<FMaterialSceneTexturePlan, std::string>;
	RENDERER_API auto MaterialSceneTextureUV(FVector2f ViewportUV, FIntPoint TextureExtent,
		FIntPoint ViewMin, FIntPoint ViewSize) -> FVector2f;
	struct FMaterialSceneTextureBindings
	{
		std::array<FTextureViewRHIRef, 2> Textures;
		std::array<FSamplerRHIRef, 2> Samplers;
		FRHIUniformBufferRange Uniform;
		std::vector<FRHIShaderParameterResource> Parameters;
	};
	// Counted bindings are built atomically from this pass's resolver authority.
	RENDERER_API auto ResolveMaterialSceneTextureBindings(FRHICommandListImmediate& Commands,
		const FShaderReflectionData& Reflection, const FMaterialSceneTexturePassParameters& Parameters,
		const FRDGParameterResolver& Resolver, const FMaterialSceneTextureUniform& Uniform)
		-> std::expected<FMaterialSceneTextureBindings, std::string>;
}
