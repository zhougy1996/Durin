#include "Renderers/SceneRenderFeatureRecorders.h"

namespace Durin
{
	FSceneRenderFeatureRecorders::FSceneRenderFeatureRecorders(
		FSceneRenderer& Renderer,
		FSceneRenderTelemetry& InTelemetry,
		FResolvedSceneResources& InResolvedSceneResources,
		FSceneViewTemporalContext& InTemporalContext,
		FSceneViewState*& InViewState)
		: RDGAllocator(Renderer.RDGAllocator)
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
		, ResolvedSceneResources(InResolvedSceneResources)
		, TemporalContext(InTemporalContext)
		, ViewState(InViewState)
	{
	}
} // namespace Durin
