#pragma once

#include "Renderers/SceneFrameGraphTypes.h"

namespace Durin
{
	class FSceneFrameFeatureRecorders;
	struct FSceneRenderTelemetry;

	struct FSceneFrameGraphServices final
	{
		FSceneFrameFeatureRecorders& Recorders;
		FDefaultTextureResources& DefaultTextures;
		FEnvironmentLightingResources& EnvironmentLighting;
		FDirectionalShadowRenderer& DirectionalShadowRenderer;
		FResolvedSceneFrame& ResolvedFrame;
		FSceneRenderTelemetry& Telemetry;
		std::function<ERenderViewResult(const FSceneFrameTopology&)>
			ResolveTargets;
	};

	struct FSceneFrameGraphComposeInputs final
	{
		FSceneFrameGraphServices& Services;
		const FSceneRenderPlan& PreparedView;
		const FSceneView& View;
		FRHITexture* OutputTarget = nullptr;
		const FSceneViewRenderOptions& Options;
		FSceneFrameTopology& Topology;
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

	struct FSceneFrameGraphComposition final
	{
		std::optional<FDeferredDirectionalLightingRenderer::FRenderParameters>
			DeferredParameters;
		std::optional<FDeferredDirectionalLightingRenderer::FRenderParameters>
			ProductionDeferredParameters;
		ERenderViewResult TargetResolutionResult = ERenderViewResult::Success;
		FSceneFrameGraphResources Resources;
		FSceneFrameGraphExecutionChannels Channels;
		FSceneColorPassResult SceneColorPublication;
		FPostProcessPassResult PostProcessPublication;
	};

	// Wires feature contributions into the caller-owned parent graph.
	class FSceneFrameGraphComposer final
	{
	public:
		static auto Compose(
			FRenderGraphBuilder& Graph,
			const FSceneFrameGraphComposeInputs& Inputs,
			FSceneFrameGraphComposition& Composition) -> void;
	};
} // namespace Durin
