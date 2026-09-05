#include "Renderers/SceneRenderPipeline.h"

#include "Renderers/SceneRenderPlan.h"
#include "Renderers/SceneRendererProfiling.h"
#include "Renderers/DirectionalShadowView.h"
#include "Renderers/ForwardLighting.h"
#include "Renderers/VolumetricCloudScenePreparation.h"
#include "Asset/Asset.h"
#include "EnvironmentLighting/EnvironmentLighting.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Scene.h"
#include "SceneInfo.h"
#include "SceneView.h"

namespace Durin
{
	auto FSceneRenderPipeline::PrepareView_RenderThread(
		FRHICommandListImmediate& CommandList,
		FSceneFrameContext& Context
	) -> FSceneRenderPreparationResult
	{
		FScene* Scene = Context.Logical.Scene;
		FSceneView& RenderView = Context.Logical.RenderView;
		const FSceneViewRenderOptions& Options = Context.Logical.Options;
		FSceneRenderTelemetry& Telemetry = Context.Observation.Telemetry;
		FSceneRenderPlan PreparedView;
		PreparedView.Context.View = RenderView;
		if (Options.Environment)
		{
			const FViewEnvironmentOverride& Environment = *Options.Environment;
			FRHITexture* Texture = Environment.TextureReference != nullptr ? Environment.TextureReference->GetReferencedTexture_RenderThread() : nullptr;
			if (Texture == nullptr
				|| Texture->GetDimension() != ETextureDimension::TextureCube)
			{
				return {
					.Result = ERenderViewResult::RequiredEnvironmentUnavailable};
			}
			PreparedView.Environment.emplace();
			PreparedView.Environment->SkyBox.TextureReference =
				Environment.TextureReference;
			PreparedView.Environment->SkyBox.Rotation = Environment.Rotation;
			PreparedView.Environment->SkyBox.Tint = Environment.Tint;
			PreparedView.Environment->SkyBox.Intensity = Environment.Intensity;
			PreparedView.Environment->Texture = Texture;
		}
		if (Scene != nullptr)
		{
			const FSceneVisibilityResult Visibility = PrepareSceneVisibility(
				*Scene, RenderView, Telemetry.View
			);
			const FSkyBoxSceneProxy* SkyBox =
				Scene->GetSkyBoxProxy_RenderThread();
			if (!PreparedView.Environment && SkyBox != nullptr)
			{
				PreparedView.Environment = FPreparedEnvironment{
					.SkyBox = SkyBox->GetData()};
			}
			PreparedView.Lighting.Lights = PrepareLightView_RenderThread(
				*Scene, RenderView, Telemetry.View
			);
			if (!PreparedView.Lighting.Lights.Directional.empty())
			{
				++Telemetry.View.DirectionalShadow.ShadowSelectedLights;
				const FPreparedDirectionalLight& Selected =
					PreparedView.Lighting.Lights.Directional.front();
				const auto ShadowPreparationStart =
					std::chrono::steady_clock::now();
				PreparedView.DirectionalShadow.emplace();
				if (TryPrepareDirectionalShadowView(
						RenderView, Selected.Id, Selected.Data,
						PreparedView.DirectionalShadow->View
					))
				{
					++Telemetry.View.DirectionalShadow.ShadowValidReceiverViews;
					const size_t DiagnosticIndex = static_cast<size_t>(
						PreparedView.DirectionalShadow->View.DiagnosticMode
					);
					if (DiagnosticIndex
						< Telemetry.View.DirectionalShadow.ShadowDiagnosticViews.size())
						++Telemetry.View.DirectionalShadow.ShadowDiagnosticViews[DiagnosticIndex];
					Telemetry.View.DirectionalShadow.ShadowCandidate =
						PreparedView.DirectionalShadow->View.Candidate;
					Telemetry.View.DirectionalShadow.ShadowCascadeCount =
						PreparedView.DirectionalShadow->View.CascadeCount;
					const FDirectionalShadowFilter& Filter =
						PreparedView.DirectionalShadow->View.Cascades[0].Filter;
					const size_t QualityIndex = static_cast<size_t>(Filter.Quality);
					if (QualityIndex < Telemetry.View.DirectionalShadow.ShadowQualityViews.size())
						++Telemetry.View.DirectionalShadow.ShadowQualityViews[QualityIndex];
					Telemetry.View.DirectionalShadow.ShadowComparisonOperations +=
						Filter.ComparisonOperations;
					Telemetry.View.DirectionalShadow.ShadowTransitionComparisonOperations +=
						PreparedView.DirectionalShadow->View.CascadeCount > 1 ? 2u * Filter.ComparisonOperations : Filter.ComparisonOperations;
					Telemetry.View.DirectionalShadow.ShadowGuardTexels +=
						Filter.GuardTexels;
					Telemetry.View.DirectionalShadow.ShadowInvalidQualityFallbacks +=
						Filter.bUsedInvalidQualityFallback ? 1u : 0u;
					const auto DiscoveryStart = std::chrono::steady_clock::now();
					PreparedView.DirectionalShadow->Casters =
						PrepareDirectionalShadowCasterTable(
							*Scene, PreparedView.DirectionalShadow->View
						);
					Telemetry.View.DirectionalShadow.ShadowDiscoveryMembershipNanoseconds =
						static_cast<uint64>(std::chrono::duration_cast<
												std::chrono::nanoseconds>(
												std::chrono::steady_clock::now() - DiscoveryStart
						)
												.count());
					const auto& CasterTable = PreparedView.DirectionalShadow->Casters;
					Telemetry.View.DirectionalShadow.ShadowSceneTraversals =
						CasterTable.SceneTraversals;
					Telemetry.View.DirectionalShadow.ShadowUniqueSubmittedCasters =
						CasterTable.UniqueSubmitted;
					Telemetry.View.DirectionalShadow.ShadowUniqueHiddenCasters =
						CasterTable.UniqueHidden;
					Telemetry.View.DirectionalShadow.ShadowUniqueEligibleStaticMeshCasters =
						CasterTable.UniqueEligibleStaticMeshes;
					Telemetry.View.DirectionalShadow.ShadowUniqueEligibleSplineMeshCasters =
						CasterTable.UniqueEligibleSplineMeshes;
					Telemetry.View.DirectionalShadow.ShadowCascadeClassificationTests =
						CasterTable.CascadeClassificationTests;
					Telemetry.View.DirectionalShadow.ShadowMembershipPopcount =
						CasterTable.MembershipPopcount;
					Telemetry.View.DirectionalShadow.ShadowTemporaryBytes =
						CasterTable.TemporaryBytes;
					for (uint32 CascadeIndex = 0;
						 CascadeIndex < PreparedView.DirectionalShadow->View.CascadeCount;
						 ++CascadeIndex)
					{
						const auto& Cascade =
							PreparedView.DirectionalShadow->View.Cascades[CascadeIndex];
						auto& CascadeTelemetry =
							Telemetry.View.DirectionalShadow.ShadowCascades[CascadeIndex];
						CascadeTelemetry.NearDepth = Cascade.NearDepth;
						CascadeTelemetry.FarDepth = Cascade.FarDepth;
						CascadeTelemetry.TransitionStartDepth =
							Cascade.TransitionStartDepth;
						CascadeTelemetry.TexelWorldSizeX = Cascade.TexelWorldSize.x;
						CascadeTelemetry.TexelWorldSizeY = Cascade.TexelWorldSize.y;
						CascadeTelemetry.ComparisonOperations =
							Cascade.Filter.ComparisonOperations;
						CascadeTelemetry.GuardTexels = Cascade.Filter.GuardTexels;
						Telemetry.View.DirectionalShadow.ShadowBiasFallbacks +=
							Cascade.Bias.bUsedFallback ? 1u : 0u;
						Telemetry.View.DirectionalShadow.ShadowBiasClamps +=
							Cascade.Bias.bTotalClamped ? 1u : 0u;
						const FDirectionalShadowCasterCandidates& Casters =
							CasterTable.Cascades[CascadeIndex];
						CascadeTelemetry.SubmittedCasters = Casters.Submitted;
						CascadeTelemetry.HiddenCasters = Casters.Hidden;
						CascadeTelemetry.CulledCasters = Casters.Culled;
						CascadeTelemetry.InvalidBoundsFallbacks =
							Casters.InvalidBoundsFallbacks;
						Telemetry.View.DirectionalShadow.ShadowSubmittedCasters += Casters.Submitted;
						Telemetry.View.DirectionalShadow.ShadowHiddenCasters += Casters.Hidden;
						Telemetry.View.DirectionalShadow.ShadowCulledCasters += Casters.Culled;
						Telemetry.View.DirectionalShadow.ShadowInvalidBoundsFallbacks +=
							Casters.InvalidBoundsFallbacks;
						auto& StaticMeshes =
							PreparedView.DirectionalShadow->StaticMeshes[CascadeIndex];
						const auto StaticSplineStart =
							std::chrono::steady_clock::now();
						StaticMeshes = PrepareStaticMeshView_RenderThread(
							CommandList, Casters.StaticMeshes, Cascade.CasterView,
							ERasterMode::Solid, Casters.SplineMeshes,
							ERenderPreparationMode::ShadowDepth
						);
						Telemetry.View.DirectionalShadow.ShadowStaticSplinePreparationNanoseconds +=
							static_cast<uint64>(std::chrono::duration_cast<
													std::chrono::nanoseconds>(
													std::chrono::steady_clock::now()
													- StaticSplineStart
							)
													.count());
						Telemetry.View.DirectionalShadow.ShadowSortingBatchingNanoseconds +=
							StaticMeshes.SortingNanoseconds;
						Telemetry.View.DirectionalShadow.
							ShadowStaticSplinePrimitiveFactBuilds +=
							StaticMeshes.SharedPrimitiveFactBuilds;
						Telemetry.View.DirectionalShadow.
							ShadowStaticSplinePrimitiveFactReuses +=
							StaticMeshes.SharedPrimitiveFactReuses;
						Telemetry.View.DirectionalShadow.ShadowSelectedLODFactBuilds +=
							StaticMeshes.SelectedLODFactBuilds;
						Telemetry.View.DirectionalShadow.ShadowSelectedLODFactReuses +=
							StaticMeshes.SelectedLODFactReuses;
						Telemetry.View.DirectionalShadow.
							ShadowStaticSplineSectionFactBuilds +=
							StaticMeshes.SharedSectionFactBuilds;
						Telemetry.View.DirectionalShadow.
							ShadowStaticSplineSectionFactReuses +=
							StaticMeshes.SharedSectionFactReuses;
						auto ApplyRasterBias = [&Cascade](auto& Geometry) {
							for (auto* Bucket : {&Geometry.Opaque, &Geometry.Masked})
								for (auto& Draw : *Bucket)
								{
									auto& Raster = Draw.PipelineKey.Rasterizer;
									Raster.bEnableDepthBias = true;
									Raster.DepthBiasConstantFactor =
										Cascade.Bias.RasterConstant;
									Raster.DepthBiasSlopeFactor =
										Cascade.Bias.RasterSlope;
									Raster.DepthBiasClamp =
										Cascade.Bias.RasterClamp;
								}
						};
						ApplyRasterBias(StaticMeshes);
						CascadeTelemetry.PreparedStaticMeshCasters =
							StaticMeshes.PreparedLocalPrimitives;
						CascadeTelemetry.PreparedSplineMeshCasters =
							StaticMeshes.PreparedSplinePrimitives;
					CascadeTelemetry.PreparedTriangles =
						StaticMeshes.SelectedTriangles;
						Telemetry.View.DirectionalShadow.ShadowPreparedStaticMeshCasters +=
							CascadeTelemetry.PreparedStaticMeshCasters;
						Telemetry.View.DirectionalShadow.ShadowPreparedSplineMeshCasters +=
							CascadeTelemetry.PreparedSplineMeshCasters;
						Telemetry.View.DirectionalShadow.ShadowPreparedTriangles +=
							CascadeTelemetry.PreparedTriangles;
					}
					Telemetry.View.DirectionalShadow.ShadowLogicalPreparationNanoseconds =
						static_cast<uint64>(std::chrono::duration_cast<
												std::chrono::nanoseconds>(
												std::chrono::steady_clock::now()
												- ShadowPreparationStart
						)
												.count());
				}
				else if (Selected.Data.bCastShadows)
				{
					++Telemetry.View.DirectionalShadow.ShadowInvalidReceiverViews;
					Telemetry.View.DirectionalShadow.ShadowLogicalPreparationNanoseconds =
						static_cast<uint64>(std::chrono::duration_cast<
												std::chrono::nanoseconds>(
												std::chrono::steady_clock::now()
												- ShadowPreparationStart
						)
												.count());
					PreparedView.DirectionalShadow.reset();
				}
				else
				{
					PreparedView.DirectionalShadow.reset();
				}
			}
			PreparedView.Receiver.StaticMeshes = PrepareStaticMeshView_RenderThread(
				CommandList,
				Visibility.StaticMeshSceneInfos,
				RenderView,
				RenderView.Settings.Mode.RasterMode,
				Visibility.SplineMeshSceneInfos
			);
		}
		PrepareCombinedTranslucentGeometry(PreparedView.Receiver);
		Telemetry.View.CombinedTranslucentGeometryDraws =
			PreparedView.Receiver.TranslucentGeometry.size();
		if (Scene != nullptr)
		{
			FVolumetricCloudSceneSnapshot Cloud;
			if (Scene->GetActiveVolumetricCloud_RenderThread(Cloud))
			{
				PreparedView.VolumetricCloud.emplace();
				PreparedView.VolumetricCloud->HistoryKey = Cloud.Desc.HistoryKey;
				PreparedView.VolumetricCloud->HistoryKey ^=
					CalculateVolumetricCloudLightingKey(PreparedView.Lighting.Lights);
				PreparedView.VolumetricCloud->Parameters =
					BuildVolumetricCloudParameters(
						Cloud.Desc.Data, PreparedView.Lighting.Lights);
				auto ResolveDimension = [](const FRHITextureReferenceRef& Reference,
										   ETextureDimension Dimension) -> FRHITexture* {
					FRHITexture* Texture = Reference != nullptr ? Reference->GetReferencedTexture_RenderThread() : nullptr;
					return Texture != nullptr && Texture->GetDimension() == Dimension ? Texture : nullptr;
				};
				PreparedView.VolumetricCloud->Textures.BaseDensity = ResolveDimension(
					Cloud.Desc.Data.BaseDensityTexture, ETextureDimension::Texture3D
				);
				PreparedView.VolumetricCloud->Textures.DetailDensity = ResolveDimension(
					Cloud.Desc.Data.DetailDensityTexture, ETextureDimension::Texture3D
				);
				PreparedView.VolumetricCloud->Textures.Weather = ResolveDimension(
					Cloud.Desc.Data.WeatherTexture, ETextureDimension::Texture2D
				);
			}
		}
		return {
			.Result = ERenderViewResult::Success,
			.Plan = std::move(PreparedView)};
	}

	auto FSceneRenderPipeline::ResolveSceneRenderResources_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneRenderPlan& PreparedView,
		FSceneFrameContext& Context
	) -> ERenderViewResult
	{
		FResolvedSceneResources& ResolvedSceneResources = Context.Resolved.Scene;
		FSceneRenderTelemetry& Telemetry = Context.Observation.Telemetry;
		const FSceneView& View = PreparedView.Context.View;
		const bool bRequiresDeferredOpaque =
			View.Settings.Mode.RenderMode == ERenderMode::Lit
			&& View.Settings.Mode.RasterMode == ERasterMode::Solid;
		Renderer.StaticMeshRenderer.PrepareResources_RenderThread(
			CommandList, PreparedView.Receiver.StaticMeshes,
			ResolvedSceneResources.Receiver.StaticMeshes, !bRequiresDeferredOpaque);
		if (PreparedView.DirectionalShadow)
		{
			ResolvedSceneResources.DirectionalShadow.emplace();
			Renderer.DirectionalShadowRenderer.PrepareResources_RenderThread(
				CommandList, Renderer.StaticMeshRenderer,
				*PreparedView.DirectionalShadow,
				*ResolvedSceneResources.DirectionalShadow, Telemetry.View);
		}

		const bool bShadowReady = ResolvedSceneResources.DirectionalShadow
			&& ResolvedSceneResources.DirectionalShadow->bEnabled;
		FRHITexture* DirectionalShadowTexture =
			Renderer.DirectionalShadowRenderer.GetTexture_RenderThread();
		FRHISampler* DirectionalShadowSampler =
			Renderer.DirectionalShadowRenderer.GetSampler_RenderThread();
		ResolvedSceneResources.Receiver.StaticMeshes.DirectionalShadowTexture =
			DirectionalShadowTexture;
		ResolvedSceneResources.Receiver.StaticMeshes.DirectionalShadowSampler =
			DirectionalShadowSampler;

		const FForwardLightingUniform Lighting = BuildForwardLightingUniform(
			PreparedView.Lighting.Lights, View,
			bShadowReady && DirectionalShadowTexture != nullptr
				&& DirectionalShadowSampler != nullptr
				? &PreparedView.DirectionalShadow->View : nullptr);
		Telemetry.View.Lighting.PackedLightBytes = sizeof(Lighting);
		ResolvedSceneResources.Lighting.UniformBuffer =
			CommandList.AllocateDynamicUniformBuffer(&Lighting, sizeof(Lighting));
		if (ResolvedSceneResources.Lighting.UniformBuffer.Buffer == nullptr
			|| ResolvedSceneResources.Lighting.UniformBuffer.Size != sizeof(Lighting))
			return ERenderViewResult::RendererResourcesUnavailable;

		if (PreparedView.VolumetricCloud)
		{
			ResolvedSceneResources.VolumetricCloud.emplace();
			ResolvedSceneResources.VolumetricCloud->Textures =
				PreparedView.VolumetricCloud->Textures;
			ResolvedSceneResources.VolumetricCloud->Textures.DensitySampler =
				Renderer.VolumetricCloudRenderer.EnsureDensitySampler_RenderThread();
		}
		return ERenderViewResult::Success;
	}

	auto FSceneRenderPipeline::BuildSceneFrameFeaturePlan(
		const FSceneRenderPlan& PreparedView,
		const FSceneViewRenderOptions& Options,
		uint32 Width,
		uint32 Height,
		const FRendererQualificationPolicy& Qualification
	) const -> FSceneFrameFeaturePlan
	{
		const FSceneView& View = PreparedView.Context.View;
		FSceneFrameFeaturePlan Plan;
		auto AddPurpose = [](FSceneFeatureDecision& Feature,
			ESceneFeaturePurpose Purpose, bool bEnabled) {
			if (bEnabled) Feature.Purposes = Feature.Purposes | Purpose;
		};
		const bool bProductionDeferred =
			View.Settings.Mode.RenderMode == ERenderMode::Lit
			&& View.Settings.Mode.RasterMode == ERasterMode::Solid;
		AddPurpose(Plan.Deferred, ESceneFeaturePurpose::Production,
			bProductionDeferred);
		AddPurpose(Plan.Deferred, ESceneFeaturePurpose::Qualification,
			Qualification.bEnableDeferredDirectional);
		AddPurpose(Plan.Deferred, ESceneFeaturePurpose::Debug,
			Options.DeferredDirectionalDebugMode
				!= EDeferredDirectionalDebugMode::Disabled
			|| Options.GroundTruthAmbientOcclusionDebugMode
				!= EGroundTruthAmbientOcclusionDebugMode::Disabled);
		AddPurpose(Plan.AmbientOcclusion, ESceneFeaturePurpose::Production,
			bProductionDeferred && View.Settings.AmbientOcclusion.bEnabled);
		AddPurpose(Plan.AmbientOcclusion, ESceneFeaturePurpose::Qualification,
			Qualification.bEnableGroundTruthAmbientOcclusion);
		AddPurpose(Plan.AmbientOcclusion, ESceneFeaturePurpose::Debug,
			Options.GroundTruthAmbientOcclusionDebugMode
				!= EGroundTruthAmbientOcclusionDebugMode::Disabled);
		Plan.AmbientOcclusion.Quality = View.Settings.AmbientOcclusion.Quality;
		AddPurpose(Plan.GBuffer, ESceneFeaturePurpose::Qualification,
			Qualification.bEnableGBuffer);
		AddPurpose(Plan.GBuffer, ESceneFeaturePurpose::Debug,
			Options.GBufferDebugMode != EGBufferDebugMode::Disabled);
		AddPurpose(Plan.GBuffer, ESceneFeaturePurpose::Dependency,
			Plan.RequiresDeferredInputs());
		AddPurpose(Plan.GBufferDebug, ESceneFeaturePurpose::Debug,
			Options.GBufferDebugMode != EGBufferDebugMode::Disabled);
		const bool bContact = bProductionDeferred
			&& View.Settings.DirectionalShadow.bEnableContactShadows
			&& PreparedView.DirectionalShadow
			&& PreparedView.DirectionalShadow->View.bEnabled;
		AddPurpose(Plan.ContactVisibility, ESceneFeaturePurpose::Production,
			bContact);
		const bool bCloudShadow = bProductionDeferred
			&& PreparedView.VolumetricCloud
			&& !PreparedView.Lighting.Lights.Directional.empty();
		AddPurpose(Plan.CloudShadow, ESceneFeaturePurpose::Production,
			bCloudShadow);
		const bool bCloudInputs = bProductionDeferred
			&& PreparedView.VolumetricCloud
			&& PreparedView.VolumetricCloud->Textures.BaseDensity
			&& PreparedView.VolumetricCloud->Textures.DetailDensity;
		AddPurpose(Plan.CloudSpatial, ESceneFeaturePurpose::Production,
			bCloudInputs);
		const auto CloudQuality = CanonicalizeVolumetricCloudQuality(
			View.Settings.VolumetricCloud.Quality);
		const auto CloudPolicy =
			FVolumetricCloudSpatialRenderer::ResolveQualityPolicy(CloudQuality);
		const auto CloudExtent =
			FVolumetricCloudSpatialRenderer::CalculateScaledExtent(
				Width, Height, CloudPolicy);
		Plan.CloudSpatial.Extent = {
			static_cast<int32>(CloudExtent.Width),
			static_cast<int32>(CloudExtent.Height)};
		Plan.PostProcess.Purposes = ESceneFeaturePurpose::Production;
		return Plan;
	}

} // namespace Durin
