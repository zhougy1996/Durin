#pragma once

#include "Renderers/AmbientOcclusionRendering.h"
#include "Renderers/ContactShadowVisibilityRendering.h"
#include "Renderers/DirectionalShadowRendering.h"
#include "RDG/RDGParameters.h"

namespace Durin
{
	class FDefaultTextureResources;
	class FDeferredDirectionalLightingRenderer;
	class FDirectionalShadowRenderer;
	struct FCloudShadowGraphOutput;
	struct FSceneRenderTelemetry;
	struct FSceneView;

	struct FDeferredDirectionalLightingPassResources final
	{
		std::optional<FRDGTextureParameter> DirectionalShadow;
		std::array<std::optional<FRDGTextureParameter>, 4> GBuffer;
		std::optional<FRDGTextureParameter> SceneDepth;
		std::array<std::optional<FRDGTextureParameter>, 4> AmbientOcclusion;
		std::optional<FRDGTextureParameter> ContactShadowFragment;
		std::optional<FRDGTextureParameter> ContactShadowCompute;
		std::optional<FRDGTextureParameter> CloudShadowFragment;
		std::optional<FRDGTextureParameter> CloudShadowCompute;
		std::optional<FRDGTextureParameter> DefaultWhite;
		std::optional<FRDGTextureParameter> DefaultShadowArray;
		std::optional<FRDGTextureParameter> EnvironmentIrradiance;
		std::optional<FRDGTextureParameter> EnvironmentPrefiltered;
		std::optional<FRDGTextureParameter> EnvironmentBrdfLut;
		std::optional<FRDGColorAttachmentParameter> IsolatedDeferredOutput;

		static RENDERER_API auto GetRDGParametersMetadata()
			-> const FRDGParametersMetadata*;
	};

	struct FDeferredDirectionalLightingPassParameters final
	{
		TRDGValueRead<FDirectionalShadowPassResult> DirectionalShadow;
		std::optional<TRDGValueRead<FGBufferPassResult>> GBufferCompletion;
		std::optional<TRDGValueRead<FGroundTruthAmbientOcclusionPassResult>> AmbientOcclusion;
		std::optional<TRDGValueRead<FContactShadowVisibilityPassResult>> ContactShadow;
		std::optional<TRDGValueRead<FVolumetricCloudShadowPassResult>> CloudShadow;
		TRDGValueWrite<FIsolatedDeferredPassResult> Completion;
		FDeferredDirectionalLightingPassResources Resources;

		static RENDERER_API auto GetRDGParametersMetadata()
			-> const FRDGParametersMetadata*;
	};

	struct FDeferredLightingGraphOutput final
	{
		TRDGValueHandle<FIsolatedDeferredPassResult> Completion;
		std::optional<FRDGTextureHandle> Isolated;
	};

	// Selected physical textures are setup metadata; callbacks use declared graph fields.
	struct FSceneEnvironmentInputs final
	{
		std::optional<FRDGTextureHandle> Irradiance;
		std::optional<FRDGTextureHandle> Prefiltered;
		std::optional<FRDGTextureHandle> BrdfLut;
		FRHITexture* SelectedIrradiance = nullptr;
		FRHITexture* SelectedPrefiltered = nullptr;
		FRHITexture* SelectedBrdfLut = nullptr;
		FRHISampler* Sampler = nullptr;
	};

	struct FDeferredLightingFeatureInputs final
	{
		const FSceneView& View;
		const FSceneViewRenderOptions& Options;
		const FDirectionalShadowGraphOutput& DirectionalShadow;
		const FGBufferGraphOutput& GBuffer;
		const FAmbientOcclusionGraphOutput& AmbientOcclusion;
		const FContactShadowGraphOutput& ContactShadow;
		const FCloudShadowGraphOutput& CloudShadow;
		FDefaultTextureResources& DefaultTextures;
		FDirectionalShadowRenderer& DirectionalShadowRenderer;
		FDeferredDirectionalLightingRenderer& Renderer;
		FResolvedSceneResources& Resolved;
		FSceneRenderTelemetry& Telemetry;
		std::optional<FRDGTextureHandle> DefaultWhite;
		std::optional<FRDGTextureHandle> DefaultShadowArray;
		FSceneEnvironmentInputs Environment;
		std::optional<FDeferredDirectionalLightingRenderer::FRenderParameters>&
			DeferredParameters;
		std::optional<FDeferredDirectionalLightingRenderer::FRenderParameters>&
			ProductionDeferredParameters;
		uint32 Width;
		uint32 Height;
		const FSceneFeatureDecision& Feature;
		const FSceneFrameFeaturePlan::FAmbientOcclusion& AmbientOcclusionFeature;
		bool bHybridRetainedResourcesReady;
	};

	inline constexpr std::string_view DeferredDirectionalLightingPassName = "Scene.DeferredDirectionalLighting";
	auto AddDeferredDirectionalLightingPasses(FRDGBuilder& Graph, const FDeferredLightingFeatureInputs& Inputs)
		-> FDeferredLightingGraphOutput;
} // namespace Durin
