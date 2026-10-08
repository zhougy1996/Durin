#pragma once

#include "CoreMinimal.h"

#include "Renderers/DeferredDirectionalLightingRendering.h"
#include "RDG/RDGParameters.h"

namespace Durin
{
	class FDefaultTextureResources;
	class FDeferredDirectionalLightingRenderer;
	class FDirectionalShadowRenderer;
	class FSkyBoxRenderer;
	class FStaticMeshRenderer;
	struct FPreparedEnvironment;
	struct FPreparedReceiverGeometry;
	struct FSceneRenderTelemetry;

	struct FSceneGeometryRecordInputs final
	{
		const FSceneView& View;
		const FPreparedEnvironment* Environment = nullptr;
		const FPreparedReceiverGeometry& Receiver;
		bool bRequireEnvironmentTexture = false;
	};

	struct FBaseScenePassResources final
	{
		std::optional<FRDGTextureParameter> DirectionalShadow;
		std::optional<FRDGTextureParameter> DefaultWhite;
		std::optional<FRDGTextureParameter> DefaultShadowArray;
		std::optional<FRDGTextureParameter> EnvironmentIrradiance;
		std::optional<FRDGTextureParameter> EnvironmentPrefiltered;
		std::optional<FRDGTextureParameter> EnvironmentBrdfLut;
		std::optional<FRDGTextureParameter> EnvironmentSky;
		std::optional<FRDGColorAttachmentBinding> SceneColorOutput;
		std::optional<FRDGDepthStencilAttachmentBinding> SceneDepthOutput;

		static RENDERER_API auto GetRDGParametersMetadata()
			-> const FRDGParametersMetadata*;
	};

	struct FBaseScenePassParameters final
	{
		std::optional<TRDGValueRead<FGBufferPassResult>> GBufferCompletion;
		std::optional<TRDGValueRead<FSceneColorPassResult>> Predecessor;
		TRDGValueWrite<FSceneColorPassResult> Completion;
		FBaseScenePassResources Resources;

		static RENDERER_API auto GetRDGParametersMetadata()
			-> const FRDGParametersMetadata*;
	};

	struct FProductionDeferredPassParameters final
	{
		FDeferredLightingInputParameters Inputs;
		TRDGValueRead<FSceneColorPassResult> Predecessor;
		TRDGValueWrite<FSceneColorPassResult> Completion;
		FRDGColorAttachmentBinding SceneColorOutput;
		static RENDERER_API auto GetRDGParametersMetadata() -> const FRDGParametersMetadata*;
	};

	struct FBaseSceneGraphOutput final
	{
		TRDGValueHandle<FSceneColorPassResult> Completion;
		FRDGTextureHandle Color;
		FRDGTextureHandle Depth;
	};

	struct FBaseSceneFeatureInputs final
	{
		FSceneGeometryRecordInputs Record;
		const FDeferredLightingGraphOutput& Deferred;
		FRDGTextureHandle SceneColor;
		FRDGTextureHandle SceneDepth;
		const FDirectionalShadowGraphOutput& DirectionalShadow;
		FDefaultTextureResources& DefaultTextures;
		FDirectionalShadowRenderer& DirectionalShadowRenderer;
		FDeferredDirectionalLightingRenderer& DeferredRenderer;
		FStaticMeshRenderer& StaticMeshes;
		FSkyBoxRenderer& SkyBox;
		FResolvedSceneResources& Resolved;
		FSceneRenderTelemetry& Telemetry;
		std::optional<FRDGTextureHandle> DefaultWhite;
		std::optional<FRDGTextureHandle> DefaultShadowArray;
		FSceneEnvironmentInputs Environment;
		const FSceneFeatureDecision& DeferredFeature;
	};

	inline constexpr std::string_view BaseScenePassName = "Scene.Forward";
	inline constexpr std::string_view HybridBootstrapPassName = "Scene.HybridBootstrap";
	inline constexpr std::string_view ProductionDeferredPassName = "Scene.ProductionDeferred";
	inline constexpr std::string_view RetainedForwardPassName = "Scene.RetainedForward";
	auto AddBaseScenePasses(FRDGBuilder& Graph, const FBaseSceneFeatureInputs& Inputs)
		-> FBaseSceneGraphOutput;
} // namespace Durin
