#pragma once

#include "Renderers/SceneRenderGraphTypes.h"

namespace Durin
{
	class FSceneRenderFeatureRecorders;
	struct FSceneRenderTelemetry;

	struct FSceneRenderGraphServices final
	{
		FSceneRenderFeatureRecorders& Recorders;
		FDefaultTextureResources& DefaultTextures;
		FEnvironmentLightingResources& EnvironmentLighting;
		FDirectionalShadowRenderer& DirectionalShadowRenderer;
		FResolvedSceneResources& ResolvedFrame;
		FSceneRenderTelemetry& Telemetry;
	};

	struct FSceneRenderGraphComposeInputs final
	{
		FSceneRenderGraphServices& Services;
		const FSceneRenderPlan& PreparedView;
		const FSceneView& View;
		FRHITexture* OutputTarget = nullptr;
		const FSceneViewRenderOptions& Options;
		FSceneRenderTopology& Topology;
		const RendererEditorAssistance::FPrepared& EditorAssistance;
		FContactShadowVisibilityRenderer::FRouteDecision ContactRoute;
		FVolumetricCloudShadowRenderer::ERoute CloudShadowRoute =
			FVolumetricCloudShadowRenderer::ERoute::FactorOne;
		FVolumetricCloudRenderer::ERoute CloudRoute =
			FVolumetricCloudRenderer::ERoute::Disabled;
		FRHITexture* CloudWeatherTexture = nullptr;
		uint32 Width = 0;
		uint32 Height = 0;
		bool bPresentOutput = false;
		bool bHasEditorAssistance = false;
		bool bRequiresDeferredOpaque = false;
		bool bWantsIsolatedDeferred = false;
		bool bWantsGroundTruthAmbientOcclusion = false;
		bool bWantsDeferredInputs = false;
		bool bWantsProductionDeferred = false;
		bool bHybridRetainedResourcesReady = false;
		bool bNeedsGBuffer = false;
	};

	struct FSceneRenderGraphComposition final
	{
		std::optional<FDeferredDirectionalLightingRenderer::FRenderParameters>
			DeferredParameters;
		std::optional<FDeferredDirectionalLightingRenderer::FRenderParameters>
			ProductionDeferredParameters;
		FSceneColorPassResult SceneColorPublication;
		FPostProcessPassResult PostProcessPublication;
	};

	// Wires feature contributions into the caller-owned parent graph.
	class FSceneRenderGraphComposer final
	{
	public:
		static auto Compose(
			FRDGBuilder& Graph,
			const FSceneRenderGraphComposeInputs& Inputs,
			FSceneRenderGraphComposition& Composition) -> void;
	};
} // namespace Durin
