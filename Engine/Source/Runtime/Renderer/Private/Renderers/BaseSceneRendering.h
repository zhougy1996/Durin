#pragma once

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
	};

	struct FBaseScenePassResources final
	{
		std::optional<FRDGTextureParameter> DirectionalShadow;
		std::optional<FRDGTextureParameter> DefaultWhite;
		std::optional<FRDGTextureParameter> DefaultShadowArray;
		std::optional<FRDGTextureParameter> EnvironmentIrradiance;
		std::optional<FRDGTextureParameter> EnvironmentPrefiltered;
		std::optional<FRDGTextureParameter> EnvironmentBrdfLut;
		std::optional<FRDGColorAttachmentParameter> SceneColorOutput;
		std::optional<FRDGManagedTextureParameter> SceneDepthGraphicsToGraphics;
		std::optional<FRDGManagedTextureParameter> SceneDepthGraphicsToDepth;
		std::optional<FRDGManagedTextureParameter> SceneDepthDepthToGraphics;
		std::optional<FRDGManagedTextureParameter> SceneDepthDepthToDepth;

		static RENDERER_API auto GetRDGParametersMetadata()
			-> const FRDGParametersMetadata*;
	};

	struct FBaseScenePassParameters final
	{
		TRDGValueRead<FIsolatedDeferredPassResult> DeferredLighting;
		TRDGValueWrite<FSceneColorPassResult> Completion;
		FBaseScenePassResources Resources;

		static RENDERER_API auto GetRDGParametersMetadata()
			-> const FRDGParametersMetadata*;
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
		std::optional<FDeferredDirectionalLightingRenderer::FRenderParameters>&
			ProductionDeferredParameters;
		const FSceneFeatureDecision& DeferredFeature;
		const FSceneFeatureDecision& GBufferFeature;
	};

	inline constexpr std::string_view BaseScenePassName = "Scene.Base";
	auto AddBaseScenePasses(FRDGBuilder& Graph, const FBaseSceneFeatureInputs& Inputs)
		-> FBaseSceneGraphOutput;
} // namespace Durin
