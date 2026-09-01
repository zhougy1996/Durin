#pragma once

#include "Renderers/SceneRenderGraphTypes.h"

namespace Durin
{
	class FSceneRenderFeatureRecorders;
	struct FSceneRenderTelemetry;

	struct FSceneRenderGraphServices final
	{
		FSceneRenderFeatureRecorders& Recorders;
		FRendererRDGAllocator& RDGAllocator;
		FGBufferRenderer& GBufferRenderer;
		FStaticMeshRenderer& StaticMeshRenderer;
		FSkeletalMeshRenderer& SkeletalMeshRenderer;
		FTerrainRenderer& TerrainRenderer;
		FContactShadowVisibilityRenderer& ContactShadowRenderer;
		FDefaultTextureResources& DefaultTextures;
		FEnvironmentLightingResources& EnvironmentLighting;
		FDirectionalShadowRenderer& DirectionalShadowRenderer;
		FResolvedSceneResources& ResolvedSceneResources;
		FSceneRenderTelemetry& Telemetry;
	};

	struct FSceneRenderGraphComposeInputs final
	{
		FSceneRenderGraphServices& Services;
		const FSceneRenderPlan& PreparedView;
		const FSceneView& View;
		FRHITexture* OutputTarget = nullptr;
		const FSceneViewRenderOptions& Options;
		const FSceneFrameFeaturePlan& Features;
		const RendererEditorAssistance::FPrepared& EditorAssistance;
		FRHITexture* CloudWeatherTexture = nullptr;
		uint32 Width = 0;
		uint32 Height = 0;
		bool bPresentOutput = false;
		bool bHybridRetainedResourcesReady = false;
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
