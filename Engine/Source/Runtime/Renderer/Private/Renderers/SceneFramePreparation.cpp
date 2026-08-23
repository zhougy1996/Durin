#include "Renderers/FixedSceneFrameExecutor.h"

#include "Renderers/SceneRenderPlan.h"
#include "Renderers/SceneRendererProfiling.h"
#include "Engine/TerrainSceneProxy.h"
#include "Renderers/DirectionalShadowView.h"
#include "Renderers/ForwardLighting.h"
#include "Renderers/TerrainRenderPreparation.h"
#include "Renderers/VolumetricCloudScenePreparation.h"
#include "Asset.h"
#include "EnvironmentLighting/EnvironmentLighting.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Scene.h"
#include "SceneView.h"

namespace Durin
{
	auto FFixedSceneFrameExecutor::PrepareView_RenderThread(
		FRHICommandListImmediate& CommandList,
		FScene* Scene,
		FSceneView& RenderView,
		const FSceneViewRenderOptions& Options
	) -> FSceneFramePreparationResult
	{
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
				*Scene, RenderView, Telemetry.Counters
			);
			const FSkyBoxSceneInfo* SkyBoxInfo =
				Scene->GetActiveSkyBoxSceneInfo_RenderThread();
			if (!PreparedView.Environment && SkyBoxInfo != nullptr)
			{
				PreparedView.Environment = FPreparedEnvironment{
					.SkyBox = SkyBoxInfo->GetProxy().GetData()};
			}
			PreparedView.Lighting.Lights = PrepareLightView_RenderThread(
				*Scene, RenderView, Telemetry.Counters
			);
			if (!PreparedView.Lighting.Lights.Directional.empty())
			{
				++Telemetry.Counters.DirectionalShadow.ShadowSelectedLights;
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
					++Telemetry.Counters.DirectionalShadow.ShadowValidReceiverViews;
					const size_t DiagnosticIndex = static_cast<size_t>(
						PreparedView.DirectionalShadow->View.DiagnosticMode
					);
					if (DiagnosticIndex
						< Telemetry.Counters.DirectionalShadow.ShadowDiagnosticViews.size())
						++Telemetry.Counters.DirectionalShadow.ShadowDiagnosticViews[DiagnosticIndex];
					Telemetry.Counters.DirectionalShadow.ShadowCandidate =
						PreparedView.DirectionalShadow->View.Candidate;
					Telemetry.Counters.DirectionalShadow.ShadowCascadeCount =
						PreparedView.DirectionalShadow->View.CascadeCount;
					const FDirectionalShadowFilter& Filter =
						PreparedView.DirectionalShadow->View.Cascades[0].Filter;
					const size_t QualityIndex = static_cast<size_t>(Filter.Quality);
					if (QualityIndex < Telemetry.Counters.DirectionalShadow.ShadowQualityViews.size())
						++Telemetry.Counters.DirectionalShadow.ShadowQualityViews[QualityIndex];
					Telemetry.Counters.DirectionalShadow.ShadowComparisonOperations +=
						Filter.ComparisonOperations;
					Telemetry.Counters.DirectionalShadow.ShadowTransitionComparisonOperations +=
						PreparedView.DirectionalShadow->View.CascadeCount > 1 ? 2u * Filter.ComparisonOperations : Filter.ComparisonOperations;
					Telemetry.Counters.DirectionalShadow.ShadowGuardTexels +=
						Filter.GuardTexels;
					Telemetry.Counters.DirectionalShadow.ShadowInvalidQualityFallbacks +=
						Filter.bUsedInvalidQualityFallback ? 1u : 0u;
					const auto DiscoveryStart = std::chrono::steady_clock::now();
					PreparedView.DirectionalShadow->Casters =
						PrepareDirectionalShadowCasterTable(
							*Scene, PreparedView.DirectionalShadow->View
						);
					Telemetry.Counters.DirectionalShadow.ShadowDiscoveryMembershipNanoseconds =
						static_cast<uint64>(std::chrono::duration_cast<
												std::chrono::nanoseconds>(
												std::chrono::steady_clock::now() - DiscoveryStart
						)
												.count());
					const auto& CasterTable = PreparedView.DirectionalShadow->Casters;
					Telemetry.Counters.DirectionalShadow.ShadowSceneTraversals =
						CasterTable.SceneTraversals;
					Telemetry.Counters.DirectionalShadow.ShadowUniqueSubmittedCasters =
						CasterTable.UniqueSubmitted;
					Telemetry.Counters.DirectionalShadow.ShadowUniqueHiddenCasters =
						CasterTable.UniqueHidden;
					Telemetry.Counters.DirectionalShadow.ShadowUniqueEligibleStaticMeshCasters =
						CasterTable.UniqueEligibleStaticMeshes;
					Telemetry.Counters.DirectionalShadow.ShadowUniqueEligibleSplineMeshCasters =
						CasterTable.UniqueEligibleSplineMeshes;
					Telemetry.Counters.DirectionalShadow.ShadowUniqueEligibleSkeletalMeshCasters =
						CasterTable.UniqueEligibleSkeletalMeshes;
					Telemetry.Counters.DirectionalShadow.ShadowUniqueEligibleTerrainCasters =
						CasterTable.UniqueEligibleTerrains;
					Telemetry.Counters.DirectionalShadow.ShadowCascadeClassificationTests =
						CasterTable.CascadeClassificationTests;
					Telemetry.Counters.DirectionalShadow.ShadowMembershipPopcount =
						CasterTable.MembershipPopcount;
					Telemetry.Counters.DirectionalShadow.ShadowTemporaryBytes =
						CasterTable.TemporaryBytes;
					for (uint32 CascadeIndex = 0;
						 CascadeIndex < PreparedView.DirectionalShadow->View.CascadeCount;
						 ++CascadeIndex)
					{
						const auto& Cascade =
							PreparedView.DirectionalShadow->View.Cascades[CascadeIndex];
						auto& CascadeCounters =
							Telemetry.Counters.DirectionalShadow.ShadowCascades[CascadeIndex];
						CascadeCounters.NearDepth = Cascade.NearDepth;
						CascadeCounters.FarDepth = Cascade.FarDepth;
						CascadeCounters.TransitionStartDepth =
							Cascade.TransitionStartDepth;
						CascadeCounters.TexelWorldSizeX = Cascade.TexelWorldSize.x;
						CascadeCounters.TexelWorldSizeY = Cascade.TexelWorldSize.y;
						CascadeCounters.ComparisonOperations =
							Cascade.Filter.ComparisonOperations;
						CascadeCounters.GuardTexels = Cascade.Filter.GuardTexels;
						Telemetry.Counters.DirectionalShadow.ShadowBiasFallbacks +=
							Cascade.Bias.bUsedFallback ? 1u : 0u;
						Telemetry.Counters.DirectionalShadow.ShadowBiasClamps +=
							Cascade.Bias.bTotalClamped ? 1u : 0u;
						const FDirectionalShadowCasterCandidates& Casters =
							CasterTable.Cascades[CascadeIndex];
						CascadeCounters.SubmittedCasters = Casters.Submitted;
						CascadeCounters.HiddenCasters = Casters.Hidden;
						CascadeCounters.CulledCasters = Casters.Culled;
						CascadeCounters.InvalidBoundsFallbacks =
							Casters.InvalidBoundsFallbacks;
						Telemetry.Counters.DirectionalShadow.ShadowSubmittedCasters += Casters.Submitted;
						Telemetry.Counters.DirectionalShadow.ShadowHiddenCasters += Casters.Hidden;
						Telemetry.Counters.DirectionalShadow.ShadowCulledCasters += Casters.Culled;
						Telemetry.Counters.DirectionalShadow.ShadowInvalidBoundsFallbacks +=
							Casters.InvalidBoundsFallbacks;
						auto& StaticMeshes =
							PreparedView.DirectionalShadow->StaticMeshes[CascadeIndex];
						auto& SkeletalMeshes =
							PreparedView.DirectionalShadow->SkeletalMeshes[CascadeIndex];
						auto& Terrains = PreparedView.DirectionalShadow->Terrains[CascadeIndex];
						const auto StaticSplineStart =
							std::chrono::steady_clock::now();
						StaticMeshes = PrepareStaticMeshView_RenderThread(
							CommandList, Casters.StaticMeshes, Cascade.CasterView,
							ERasterMode::Solid, Casters.SplineMeshes,
							ERenderPreparationMode::ShadowDepth
						);
						Telemetry.Counters.DirectionalShadow.ShadowStaticSplinePreparationNanoseconds +=
							static_cast<uint64>(std::chrono::duration_cast<
													std::chrono::nanoseconds>(
													std::chrono::steady_clock::now()
													- StaticSplineStart
							)
													.count());
						const auto SkeletalStart = std::chrono::steady_clock::now();
						SkeletalMeshes = PrepareSkeletalMeshView_RenderThread(
							CommandList, Casters.SkeletalMeshes, Cascade.CasterView,
							ERasterMode::Solid, PreparedView.Receiver.SkeletalPalettes,
							ERenderPreparationMode::ShadowDepth
						);
						Telemetry.Counters.DirectionalShadow.ShadowSkeletalPreparationNanoseconds +=
							static_cast<uint64>(std::chrono::duration_cast<
													std::chrono::nanoseconds>(
													std::chrono::steady_clock::now() - SkeletalStart
							)
													.count());
						const auto TerrainStart = std::chrono::steady_clock::now();
						Terrains = PrepareTerrainView_RenderThread(
							Casters.Terrains, Cascade.CasterView, ERasterMode::Solid,
							ERenderPreparationMode::ShadowDepth
						);
						Telemetry.Counters.DirectionalShadow.ShadowTerrainLogicalPreparationNanoseconds +=
							static_cast<uint64>(std::chrono::duration_cast<
													std::chrono::nanoseconds>(
													std::chrono::steady_clock::now() - TerrainStart
							)
													.count());
						Telemetry.Counters.DirectionalShadow.ShadowSortingBatchingNanoseconds +=
							StaticMeshes.SortingNanoseconds
							+ SkeletalMeshes.SortingNanoseconds
							+ Terrains.BatchConstructionNanoseconds;
						Telemetry.Counters.
							ShadowStaticSplinePrimitiveFactBuilds +=
							StaticMeshes.SharedPrimitiveFactBuilds;
						Telemetry.Counters.
							ShadowStaticSplinePrimitiveFactReuses +=
							StaticMeshes.SharedPrimitiveFactReuses;
						Telemetry.Counters.DirectionalShadow.ShadowSelectedLODFactBuilds +=
							StaticMeshes.SelectedLODFactBuilds;
						Telemetry.Counters.DirectionalShadow.ShadowSelectedLODFactReuses +=
							StaticMeshes.SelectedLODFactReuses;
						Telemetry.Counters.
							ShadowStaticSplineSectionFactBuilds +=
							StaticMeshes.SharedSectionFactBuilds;
						Telemetry.Counters.
							ShadowStaticSplineSectionFactReuses +=
							StaticMeshes.SharedSectionFactReuses;
						Telemetry.Counters.DirectionalShadow.ShadowSkeletalPrimitiveFactBuilds +=
							SkeletalMeshes.SharedPrimitiveFactBuilds;
						Telemetry.Counters.DirectionalShadow.ShadowSkeletalPrimitiveFactReuses +=
							SkeletalMeshes.SharedPrimitiveFactReuses;
						Telemetry.Counters.DirectionalShadow.ShadowSkeletalSectionFactBuilds +=
							SkeletalMeshes.SharedSectionFactBuilds;
						Telemetry.Counters.DirectionalShadow.ShadowSkeletalSectionFactReuses +=
							SkeletalMeshes.SharedSectionFactReuses;
						Telemetry.Counters.DirectionalShadow.ShadowTerrainPrimitiveFactBuilds +=
							Terrains.SharedPrimitiveFactBuilds;
						Telemetry.Counters.DirectionalShadow.ShadowTerrainPrimitiveFactReuses +=
							Terrains.SharedPrimitiveFactReuses;
						Telemetry.Counters.DirectionalShadow.ShadowTerrainPatchFactBuilds +=
							Terrains.SharedPatchFactBuilds;
						Telemetry.Counters.DirectionalShadow.ShadowTerrainPatchFactReuses +=
							Terrains.SharedPatchFactReuses;
						Telemetry.Counters.
							ShadowTerrainPatchClassificationTests +=
							Terrains.PatchClassificationTests;
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
						ApplyRasterBias(SkeletalMeshes);
						ApplyRasterBias(Terrains);
						CascadeCounters.PreparedStaticMeshCasters =
							StaticMeshes.PreparedLocalPrimitives;
						CascadeCounters.PreparedSplineMeshCasters =
							StaticMeshes.PreparedSplinePrimitives;
						CascadeCounters.PreparedSkeletalMeshCasters =
							SkeletalMeshes.Primitives.size();
						CascadeCounters.PreparedTerrainCasters =
							Terrains.Opaque.size() + Terrains.Masked.size();
						size_t TerrainShadowTriangles = 0;
						for (const auto* Bucket : {&Terrains.Opaque, &Terrains.Masked})
							for (const FPreparedTerrainDraw& Draw : *Bucket)
								TerrainShadowTriangles += Draw.TriangleCount;
						CascadeCounters.PreparedTriangles =
							StaticMeshes.SelectedTriangles
							+ SkeletalMeshes.SelectedTriangles + TerrainShadowTriangles;
						Telemetry.Counters.DirectionalShadow.ShadowPreparedStaticMeshCasters +=
							CascadeCounters.PreparedStaticMeshCasters;
						Telemetry.Counters.DirectionalShadow.ShadowPreparedSplineMeshCasters +=
							CascadeCounters.PreparedSplineMeshCasters;
						Telemetry.Counters.DirectionalShadow.ShadowPreparedSkeletalMeshCasters +=
							CascadeCounters.PreparedSkeletalMeshCasters;
						Telemetry.Counters.DirectionalShadow.ShadowPreparedTerrainCasters +=
							CascadeCounters.PreparedTerrainCasters;
						Telemetry.Counters.DirectionalShadow.ShadowPreparedTriangles +=
							CascadeCounters.PreparedTriangles;
					}
					Telemetry.Counters.DirectionalShadow.ShadowLogicalPreparationNanoseconds =
						static_cast<uint64>(std::chrono::duration_cast<
												std::chrono::nanoseconds>(
												std::chrono::steady_clock::now()
												- ShadowPreparationStart
						)
												.count());
				}
				else if (Selected.Data.bCastShadows)
				{
					++Telemetry.Counters.DirectionalShadow.ShadowInvalidReceiverViews;
					Telemetry.Counters.DirectionalShadow.ShadowLogicalPreparationNanoseconds =
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
			PreparedView.Receiver.SkeletalMeshes = PrepareSkeletalMeshView_RenderThread(
				CommandList, Visibility.SkeletalMeshSceneInfos, RenderView,
				RenderView.Settings.Mode.RasterMode,
				PreparedView.Receiver.SkeletalPalettes
			);
			PreparedView.Receiver.Terrains = PrepareTerrainView_RenderThread(
				Visibility.TerrainSceneInfos, RenderView,
				RenderView.Settings.Mode.RasterMode
			);
			if (RenderView.Settings.Terrain.bShowLODOverlay)
			{
				auto AddTerrainDrawOverlay = [&RenderView](const FPreparedTerrainDraw& Draw) {
					if (!Draw.SceneInfo || !Draw.Patch) return;
					const FBox& Bounds = Draw.Patch->LocalBounds;
					const FMatrix& Transform = Draw.SceneInfo->GetTransform();
					if (!Bounds.bIsValid || !Math::IsFinite(Transform)) return;
					const std::array<FVector3, 4> Local{
						FVector3{Bounds.Min.x, Bounds.Min.y, Bounds.Max.z},
						FVector3{Bounds.Max.x, Bounds.Min.y, Bounds.Max.z},
						FVector3{Bounds.Max.x, Bounds.Max.y, Bounds.Max.z},
						FVector3{Bounds.Min.x, Bounds.Max.y, Bounds.Max.z}
					};
					std::array<FVector3, 4> World;
					for (size_t Index = 0; Index < 4; ++Index)
						World[Index] = FVector3(Transform * FVector4(Local[Index], 1.0));
					const float Level = std::min(1.0f, Draw.ResolvedLOD / 6.0f);
					const FVector4f LevelColor{Level, 1.0f - Level, 0.2f, 0.9f};
					for (uint8 Edge = 0; Edge < 4; ++Edge)
					{
						const bool bStitched = (Draw.StitchMask & (1u << Edge)) != 0;
						RenderView.OverlayLines.push_back({.Start = World[Edge], .End = World[(Edge + 1) % 4], .Color = bStitched ? FVector4f{1.0f, 0.1f, 0.1f, 1.0f} : LevelColor, .WidthPixels = bStitched ? 3.0f : 2.0f});
					}
				};
				for (const auto* Bucket : {&PreparedView.Receiver.Terrains.Opaque, &PreparedView.Receiver.Terrains.Masked, &PreparedView.Receiver.Terrains.Translucent})
					for (const FPreparedTerrainDraw& Draw : *Bucket)
						AddTerrainDrawOverlay(Draw);
			}
		}
		PrepareCombinedTranslucentGeometry(PreparedView.Receiver);
		Telemetry.Counters.CombinedTranslucentGeometryDraws =
			PreparedView.Receiver.TranslucentGeometry.size();
		if (Scene != nullptr)
		{
			FVolumetricCloudSceneData Cloud;
			if (Scene->GetActiveVolumetricCloud_RenderThread(Cloud))
			{
				PreparedView.VolumetricCloud.emplace();
				PreparedView.VolumetricCloud->HistoryKey = GetTypeHash(Cloud.PersistentId)
														 ^ (Cloud.InstanceId + 0x9e3779b97f4a7c15ull
															+ (Cloud.PublicationRevision << 6)
															+ (Cloud.PublicationRevision >> 2));
				PreparedView.VolumetricCloud->HistoryKey ^=
					CalculateVolumetricCloudLightingKey(PreparedView.Lighting.Lights);
				PreparedView.VolumetricCloud->Parameters =
					BuildVolumetricCloudParameters(Cloud, PreparedView.Lighting.Lights);
				auto ResolveDimension = [](const FRHITextureReferenceRef& Reference,
										   ETextureDimension Dimension) -> FRHITexture* {
					FRHITexture* Texture = Reference != nullptr ? Reference->GetReferencedTexture_RenderThread() : nullptr;
					return Texture != nullptr && Texture->GetDimension() == Dimension ? Texture : nullptr;
				};
				PreparedView.VolumetricCloud->Textures.BaseDensity = ResolveDimension(
					Cloud.BaseDensityTexture, ETextureDimension::Texture3D
				);
				PreparedView.VolumetricCloud->Textures.DetailDensity = ResolveDimension(
					Cloud.DetailDensityTexture, ETextureDimension::Texture3D
				);
				PreparedView.VolumetricCloud->Textures.Weather = ResolveDimension(
					Cloud.WeatherTexture, ETextureDimension::Texture2D
				);
			}
		}
		return {
			.Result = ERenderViewResult::Success,
			.Plan = std::move(PreparedView)};
	}

	auto FFixedSceneFrameExecutor::ResolveFrameResources_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneRenderPlan& PreparedView
	) -> ERenderViewResult
	{
		const FSceneView& View = PreparedView.Context.View;
		const bool bRequiresDeferredOpaque =
			View.Settings.Mode.RenderMode == ERenderMode::Lit
			&& View.Settings.Mode.RasterMode == ERasterMode::Solid;
		StaticMeshRenderer.PrepareResources_RenderThread(
			CommandList, PreparedView.Receiver.StaticMeshes,
			ResolvedFrame.Receiver.StaticMeshes, !bRequiresDeferredOpaque);
		SkeletalMeshRenderer.PrepareResources_RenderThread(
			CommandList, PreparedView.Receiver.SkeletalPalettes,
			ResolvedFrame.Receiver.SkeletalPalettes,
			PreparedView.Receiver.SkeletalMeshes,
			ResolvedFrame.Receiver.SkeletalMeshes, !bRequiresDeferredOpaque);
		TerrainRenderer.PrepareResources_RenderThread(
			CommandList, PreparedView.Receiver.Terrains,
			ResolvedFrame.Receiver.Terrains, !bRequiresDeferredOpaque);

		if (PreparedView.DirectionalShadow)
		{
			ResolvedFrame.DirectionalShadow.emplace();
			DirectionalShadowRenderer.PrepareResources_RenderThread(
				CommandList, StaticMeshRenderer, SkeletalMeshRenderer,
				TerrainRenderer, *PreparedView.DirectionalShadow,
				*ResolvedFrame.DirectionalShadow,
				PreparedView.Receiver.SkeletalPalettes,
				ResolvedFrame.Receiver.SkeletalPalettes, Telemetry.Counters);
		}

		const bool bShadowReady = ResolvedFrame.DirectionalShadow
			&& ResolvedFrame.DirectionalShadow->bEnabled;
		FRHITexture* DirectionalShadowTexture =
			DirectionalShadowRenderer.GetTexture_RenderThread();
		FRHISampler* DirectionalShadowSampler =
			DirectionalShadowRenderer.GetSampler_RenderThread();
		ResolvedFrame.Receiver.StaticMeshes.DirectionalShadowTexture =
			DirectionalShadowTexture;
		ResolvedFrame.Receiver.StaticMeshes.DirectionalShadowSampler =
			DirectionalShadowSampler;
		ResolvedFrame.Receiver.SkeletalMeshes.DirectionalShadowTexture =
			DirectionalShadowTexture;
		ResolvedFrame.Receiver.SkeletalMeshes.DirectionalShadowSampler =
			DirectionalShadowSampler;
		ResolvedFrame.Receiver.Terrains.DirectionalShadowTexture =
			DirectionalShadowTexture;
		ResolvedFrame.Receiver.Terrains.DirectionalShadowSampler =
			DirectionalShadowSampler;

		const FForwardLightingUniform Lighting = BuildForwardLightingUniform(
			PreparedView.Lighting.Lights, View,
			bShadowReady && DirectionalShadowTexture != nullptr
				&& DirectionalShadowSampler != nullptr
				? &PreparedView.DirectionalShadow->View : nullptr);
		Telemetry.Counters.Lighting.PackedLightBytes = sizeof(Lighting);
		ResolvedFrame.Lighting.UniformBuffer =
			CommandList.AllocateDynamicUniformBuffer(&Lighting, sizeof(Lighting));
		if (ResolvedFrame.Lighting.UniformBuffer.Buffer == nullptr
			|| ResolvedFrame.Lighting.UniformBuffer.Size != sizeof(Lighting))
			return ERenderViewResult::RendererResourcesUnavailable;

		if (PreparedView.VolumetricCloud)
		{
			ResolvedFrame.VolumetricCloud.emplace();
			ResolvedFrame.VolumetricCloud->Textures =
				PreparedView.VolumetricCloud->Textures;
			ResolvedFrame.VolumetricCloud->Textures.DensitySampler =
				VolumetricCloudRenderer.EnsureDensitySampler_RenderThread();
		}
		return ERenderViewResult::Success;
	}

	auto FFixedSceneFrameExecutor::BuildFrameRequirements(
		const FSceneRenderPlan& PreparedView,
		const FSceneViewRenderOptions& Options,
		uint32 Width,
		uint32 Height
	) const -> FSceneFrameRequirements
	{
		const FSceneView& View = PreparedView.Context.View;
		const bool bProductionDeferred =
			View.Settings.Mode.RenderMode == ERenderMode::Lit
			&& View.Settings.Mode.RasterMode == ERasterMode::Solid;
		const bool bIsolatedDeferred =
			Qualification.bEnableDeferredDirectional
			|| Options.DeferredDirectionalDebugMode
				!= EDeferredDirectionalDebugMode::Disabled
			|| Options.GroundTruthAmbientOcclusionDebugMode
				!= EGroundTruthAmbientOcclusionDebugMode::Disabled;
		const bool bAmbientOcclusion =
			Qualification.bEnableGroundTruthAmbientOcclusion
			|| Options.GroundTruthAmbientOcclusionDebugMode
				!= EGroundTruthAmbientOcclusionDebugMode::Disabled
			|| (bProductionDeferred
				&& View.Settings.AmbientOcclusion.bEnabled);
		const bool bGBuffer = Qualification.bEnableGBuffer
			|| Options.GBufferDebugMode != EGBufferDebugMode::Disabled
			|| bIsolatedDeferred || bProductionDeferred || bAmbientOcclusion;
		const bool bContact = bProductionDeferred
			&& View.Settings.DirectionalShadow.bEnableContactShadows
			&& PreparedView.DirectionalShadow
			&& PreparedView.DirectionalShadow->View.bEnabled;
		const bool bForceContactFragment =
			Qualification.bForceFragmentContactVisibility
			|| View.Settings.DirectionalShadow.ContactRoutePreference
				== EContactShadowRoutePreference::Fragment;
		const bool bForceContactCompute =
			!Qualification.bForceFragmentContactVisibility
			&& View.Settings.DirectionalShadow.ContactRoutePreference
				== EContactShadowRoutePreference::Compute;
		const bool bCloudShadow = bProductionDeferred
			&& PreparedView.VolumetricCloud
			&& !PreparedView.Lighting.Lights.Directional.empty();
		const bool bCloudInputs = PreparedView.VolumetricCloud
			&& PreparedView.VolumetricCloud->Textures.BaseDensity
			&& PreparedView.VolumetricCloud->Textures.DetailDensity;
		const auto CloudQuality = View.Settings.VolumetricCloud.Quality
			< EVolumetricCloudQuality::Count
			? View.Settings.VolumetricCloud.Quality
			: EVolumetricCloudQuality::High;
		const auto CloudPolicy =
			FVolumetricCloudSpatialRenderer::ResolveQualityPolicy(CloudQuality);
		const auto CloudExtent =
			FVolumetricCloudSpatialRenderer::CalculateScaledExtent(
				Width, Height, CloudPolicy);
		const bool bForceCloudFragment = PreparedView.VolumetricCloud
			&& Qualification.bForceFragmentVolumetricCloud;
		return {
			.Width = Width,
			.Height = Height,
			.bGBuffer = bGBuffer,
			.bGroundTruthAmbientOcclusion = bAmbientOcclusion,
			.bContactFragment = bContact && !bForceContactCompute,
			.bContactCompute = bContact && !bForceContactFragment,
			.bVolumetricCloudShadowFragment = bCloudShadow,
			.bVolumetricCloudShadowCompute =
				bCloudShadow && !bForceCloudFragment,
			.bIsolatedDeferred = bIsolatedDeferred,
			.bGBufferDebug =
				Options.GBufferDebugMode != EGBufferDebugMode::Disabled,
			.bVolumetricCloudFragment = bCloudInputs,
			.bVolumetricCloudCompute = bCloudInputs && !bForceCloudFragment,
			.bVolumetricCloudComposite = bCloudInputs,
			.AmbientOcclusionQuality = View.Settings.AmbientOcclusion.Quality,
			.VolumetricCloudExtent = {
				static_cast<int32>(CloudExtent.Width),
				static_cast<int32>(CloudExtent.Height)}};
	}

	auto FFixedSceneFrameExecutor::ResolveFrameTargets_RenderThread(
		const FSceneFrameRequirements& Requirements
	) -> ERenderViewResult
	{
		auto& Targets = ResolvedFrame.Targets;
		Targets.Scene = PostProcessRenderer.EnsureSceneTargets_RenderThread(
			Requirements.Width, Requirements.Height);
		if (!Targets.Scene || !Targets.Scene->Color || !Targets.Scene->Depth)
			return ERenderViewResult::RendererResourcesUnavailable;
		if (Requirements.bGBuffer)
			Targets.GBuffer = GBufferRenderer.EnsureTargets_RenderThread(
				Requirements.Width, Requirements.Height);
		if (Requirements.bGroundTruthAmbientOcclusion)
			Targets.GroundTruthAmbientOcclusion =
				GroundTruthAmbientOcclusionRenderer.EnsureTargets_RenderThread(
					Requirements.Width, Requirements.Height,
					Requirements.AmbientOcclusionQuality);
		if (Requirements.bContactFragment)
			Targets.ContactFragment = ContactShadowRenderer.EnsureTargets_RenderThread(
				Requirements.Width, Requirements.Height);
		if (Requirements.bContactCompute)
			Targets.ContactCompute =
				ContactShadowRenderer.EnsureComputeTargets_RenderThread(
					Requirements.Width, Requirements.Height);
		if (Requirements.bVolumetricCloudShadowFragment)
			Targets.VolumetricCloudShadowFragment =
				VolumetricCloudShadowRenderer.EnsureTargets_RenderThread(
					Requirements.Width, Requirements.Height);
		if (Requirements.bVolumetricCloudShadowCompute)
			Targets.VolumetricCloudShadowCompute =
				VolumetricCloudShadowRenderer.EnsureComputeTargets_RenderThread(
					Requirements.Width, Requirements.Height);
		if (Requirements.bIsolatedDeferred)
			Targets.IsolatedDeferred =
				DeferredDirectionalLightingRenderer.EnsureTargets_RenderThread(
					Requirements.Width, Requirements.Height);
		if (Requirements.bGBufferDebug)
			Targets.GBufferDebug = GBufferDebugRenderer.EnsureTargets_RenderThread(
				Requirements.Width, Requirements.Height);
		const uint32 CloudWidth = static_cast<uint32>(
			std::max(Requirements.VolumetricCloudExtent.x, 0));
		const uint32 CloudHeight = static_cast<uint32>(
			std::max(Requirements.VolumetricCloudExtent.y, 0));
		if (Requirements.bVolumetricCloudFragment)
			Targets.VolumetricCloudFragment =
				VolumetricCloudRenderer.EnsureTargets_RenderThread(
					CloudWidth, CloudHeight);
		if (Requirements.bVolumetricCloudCompute)
			Targets.VolumetricCloudCompute =
				VolumetricCloudRenderer.EnsureComputeTargets_RenderThread(
					CloudWidth, CloudHeight);
		if (Requirements.bVolumetricCloudComposite)
			Targets.VolumetricCloudComposite =
				VolumetricCloudRenderer.EnsureCompositeTargets_RenderThread(
					Requirements.Width, Requirements.Height);
		return ERenderViewResult::Success;
	}

	auto FFixedSceneFrameExecutor::RenderDirectionalShadow_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneRenderPlan& PreparedView
	) -> FDirectionalShadowPassResult
	{
		if (!PreparedView.DirectionalShadow
			|| !ResolvedFrame.DirectionalShadow
			|| !ResolvedFrame.DirectionalShadow->bEnabled)
			return {};
		DirectionalShadowRenderer.Render_RenderThread(
			CommandList, StaticMeshRenderer, SkeletalMeshRenderer,
			TerrainRenderer, *PreparedView.DirectionalShadow,
			*ResolvedFrame.DirectionalShadow, Telemetry.Counters);
		FRHITexture* Texture =
			DirectionalShadowRenderer.GetTexture_RenderThread();
		return {
			.Status = Texture != nullptr
				? EScenePassStatus::Complete : EScenePassStatus::Failed,
			.Texture = Texture,
			.Sampler = DirectionalShadowRenderer.GetSampler_RenderThread()};
	}

} // namespace Durin
