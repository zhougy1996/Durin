#include "Renderers/SceneFrameFeatureRecorders.h"

namespace Durin
{
	FSceneFrameFeatureRecorders::FSceneFrameFeatureRecorders(
		FSceneRenderer& Renderer,
		FSceneRenderTelemetry& InTelemetry,
		FResolvedSceneFrame& InResolvedFrame,
		FSceneViewTemporalContext& InTemporalContext,
		FSceneViewState*& InViewState)
		: TransientTargets(Renderer.TransientTargets)
		, DefaultTextures(Renderer.DefaultTextures)
		, EnvironmentLighting(Renderer.EnvironmentLighting)
		, DirectionalShadowRenderer(Renderer.DirectionalShadowRenderer)
		, GBufferRenderer(Renderer.GBufferRenderer)
		, GBufferDebugRenderer(Renderer.GBufferDebugRenderer)
		, DeferredDirectionalLightingRenderer(
			Renderer.DeferredDirectionalLightingRenderer)
		, GroundTruthAmbientOcclusionRenderer(
			Renderer.GroundTruthAmbientOcclusionRenderer)
		, StaticMeshRenderer(Renderer.StaticMeshRenderer)
		, TerrainRenderer(Renderer.TerrainRenderer)
		, SkeletalMeshRenderer(Renderer.SkeletalMeshRenderer)
		, SkyBoxRenderer(Renderer.SkyBoxRenderer)
		, PostProcessRenderer(Renderer.PostProcessRenderer)
		, ContactShadowRenderer(Renderer.ContactShadowRenderer)
		, VolumetricCloudRenderer(Renderer.VolumetricCloudRenderer)
		, VolumetricCloudShadowRenderer(Renderer.VolumetricCloudShadowRenderer)
		, EditorAssistanceRenderer(Renderer.EditorAssistanceRenderer)
		, Qualification(GetRendererQualificationPolicy())
		, Telemetry(InTelemetry)
		, ResolvedFrame(InResolvedFrame)
		, TemporalContext(InTemporalContext)
		, ViewState(InViewState)
	{
	}
} // namespace Durin
