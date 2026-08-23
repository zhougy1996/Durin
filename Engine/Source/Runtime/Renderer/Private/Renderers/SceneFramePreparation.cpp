#include "Renderers/SceneFramePreparation.h"

#include "Renderers/SceneRenderer.h"
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
	auto FSceneFramePreparation::Prepare_RenderThread(
		FSceneRenderer& Renderer,
		FRHICommandListImmediate& CommandList,
		FScene* Scene,
		FSceneView& View,
		const FSceneViewRenderOptions& Options,
		FSceneRenderPlan& Plan
	) const -> ERenderViewResult
	{
		return Renderer.PrepareView_RenderThread(
			CommandList, Scene, View, Options, Plan
		);
	}

	auto FSceneRenderer::PrepareView_RenderThread(
		FRHICommandListImmediate& CommandList,
		FScene* Scene,
		FSceneView& RenderView,
		const FSceneViewRenderOptions& Options,
		FSceneRenderPlan& PreparedView
	) -> ERenderViewResult
	{
		PreparedView.Context.View = RenderView;
		if (Options.Environment)
		{
			const FViewEnvironmentOverride& Environment = *Options.Environment;
			FRHITexture* Texture = Environment.TextureReference != nullptr ? Environment.TextureReference->GetReferencedTexture_RenderThread() : nullptr;
			if (Texture == nullptr
				|| Texture->GetDimension() != ETextureDimension::TextureCube)
			{
				return ERenderViewResult::RequiredEnvironmentUnavailable;
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
				*Scene, RenderView, PreparedView.Telemetry.Counters
			);
			const FSkyBoxSceneInfo* SkyBoxInfo =
				Scene->GetActiveSkyBoxSceneInfo_RenderThread();
			if (!PreparedView.Environment && SkyBoxInfo != nullptr)
			{
				PreparedView.Environment = FPreparedEnvironment{
					.SkyBox = SkyBoxInfo->GetProxy().GetData()};
			}
			PreparedView.Lighting.Lights = PrepareLightView_RenderThread(
				*Scene, RenderView, PreparedView.Telemetry.Counters
			);
			if (!PreparedView.Lighting.Lights.Directional.empty())
			{
				++PreparedView.Telemetry.Counters.ShadowSelectedLights;
				const FPreparedDirectionalLight& Selected =
					PreparedView.Lighting.Lights.Directional.front();
				const auto ShadowPreparationStart =
					std::chrono::steady_clock::now();
				PreparedView.DirectionalShadow.emplace();
				PreparedView.ResolvedDirectionalShadow.emplace();
				if (TryPrepareDirectionalShadowView(
						RenderView, Selected.Id, Selected.Data,
						PreparedView.DirectionalShadow->View
					))
				{
					++PreparedView.Telemetry.Counters.ShadowValidReceiverViews;
					const size_t DiagnosticIndex = static_cast<size_t>(
						PreparedView.DirectionalShadow->View.DiagnosticMode
					);
					if (DiagnosticIndex
						< PreparedView.Telemetry.Counters.ShadowDiagnosticViews.size())
						++PreparedView.Telemetry.Counters.ShadowDiagnosticViews[DiagnosticIndex];
					PreparedView.Telemetry.Counters.ShadowCandidate =
						PreparedView.DirectionalShadow->View.Candidate;
					PreparedView.Telemetry.Counters.ShadowCascadeCount =
						PreparedView.DirectionalShadow->View.CascadeCount;
					const FDirectionalShadowFilter& Filter =
						PreparedView.DirectionalShadow->View.Cascades[0].Filter;
					const size_t QualityIndex = static_cast<size_t>(Filter.Quality);
					if (QualityIndex < PreparedView.Telemetry.Counters.ShadowQualityViews.size())
						++PreparedView.Telemetry.Counters.ShadowQualityViews[QualityIndex];
					PreparedView.Telemetry.Counters.ShadowComparisonOperations +=
						Filter.ComparisonOperations;
					PreparedView.Telemetry.Counters.ShadowTransitionComparisonOperations +=
						PreparedView.DirectionalShadow->View.CascadeCount > 1 ? 2u * Filter.ComparisonOperations : Filter.ComparisonOperations;
					PreparedView.Telemetry.Counters.ShadowGuardTexels +=
						Filter.GuardTexels;
					PreparedView.Telemetry.Counters.ShadowInvalidQualityFallbacks +=
						Filter.bUsedInvalidQualityFallback ? 1u : 0u;
					const auto DiscoveryStart = std::chrono::steady_clock::now();
					PreparedView.DirectionalShadow->Casters =
						PrepareDirectionalShadowCasterTable(
							*Scene, PreparedView.DirectionalShadow->View
						);
					PreparedView.Telemetry.Counters.ShadowDiscoveryMembershipNanoseconds =
						static_cast<uint64>(std::chrono::duration_cast<
												std::chrono::nanoseconds>(
												std::chrono::steady_clock::now() - DiscoveryStart
						)
												.count());
					const auto& CasterTable = PreparedView.DirectionalShadow->Casters;
					PreparedView.Telemetry.Counters.ShadowSceneTraversals =
						CasterTable.SceneTraversals;
					PreparedView.Telemetry.Counters.ShadowUniqueSubmittedCasters =
						CasterTable.UniqueSubmitted;
					PreparedView.Telemetry.Counters.ShadowUniqueHiddenCasters =
						CasterTable.UniqueHidden;
					PreparedView.Telemetry.Counters.ShadowUniqueEligibleStaticMeshCasters =
						CasterTable.UniqueEligibleStaticMeshes;
					PreparedView.Telemetry.Counters.ShadowUniqueEligibleSplineMeshCasters =
						CasterTable.UniqueEligibleSplineMeshes;
					PreparedView.Telemetry.Counters.ShadowUniqueEligibleSkeletalMeshCasters =
						CasterTable.UniqueEligibleSkeletalMeshes;
					PreparedView.Telemetry.Counters.ShadowUniqueEligibleTerrainCasters =
						CasterTable.UniqueEligibleTerrains;
					PreparedView.Telemetry.Counters.ShadowCascadeClassificationTests =
						CasterTable.CascadeClassificationTests;
					PreparedView.Telemetry.Counters.ShadowMembershipPopcount =
						CasterTable.MembershipPopcount;
					PreparedView.Telemetry.Counters.ShadowTemporaryBytes =
						CasterTable.TemporaryBytes;
					for (uint32 CascadeIndex = 0;
						 CascadeIndex < PreparedView.DirectionalShadow->View.CascadeCount;
						 ++CascadeIndex)
					{
						const auto& Cascade =
							PreparedView.DirectionalShadow->View.Cascades[CascadeIndex];
						auto& CascadeCounters =
							PreparedView.Telemetry.Counters.ShadowCascades[CascadeIndex];
						CascadeCounters.NearDepth = Cascade.NearDepth;
						CascadeCounters.FarDepth = Cascade.FarDepth;
						CascadeCounters.TransitionStartDepth =
							Cascade.TransitionStartDepth;
						CascadeCounters.TexelWorldSizeX = Cascade.TexelWorldSize.x;
						CascadeCounters.TexelWorldSizeY = Cascade.TexelWorldSize.y;
						CascadeCounters.ComparisonOperations =
							Cascade.Filter.ComparisonOperations;
						CascadeCounters.GuardTexels = Cascade.Filter.GuardTexels;
						PreparedView.Telemetry.Counters.ShadowBiasFallbacks +=
							Cascade.Bias.bUsedFallback ? 1u : 0u;
						PreparedView.Telemetry.Counters.ShadowBiasClamps +=
							Cascade.Bias.bTotalClamped ? 1u : 0u;
						const FDirectionalShadowCasterCandidates& Casters =
							CasterTable.Cascades[CascadeIndex];
						CascadeCounters.SubmittedCasters = Casters.Submitted;
						CascadeCounters.HiddenCasters = Casters.Hidden;
						CascadeCounters.CulledCasters = Casters.Culled;
						CascadeCounters.InvalidBoundsFallbacks =
							Casters.InvalidBoundsFallbacks;
						PreparedView.Telemetry.Counters.ShadowSubmittedCasters += Casters.Submitted;
						PreparedView.Telemetry.Counters.ShadowHiddenCasters += Casters.Hidden;
						PreparedView.Telemetry.Counters.ShadowCulledCasters += Casters.Culled;
						PreparedView.Telemetry.Counters.ShadowInvalidBoundsFallbacks +=
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
						PreparedView.Telemetry.Counters.ShadowStaticSplinePreparationNanoseconds +=
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
						PreparedView.Telemetry.Counters.ShadowSkeletalPreparationNanoseconds +=
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
						PreparedView.Telemetry.Counters.ShadowTerrainLogicalPreparationNanoseconds +=
							static_cast<uint64>(std::chrono::duration_cast<
													std::chrono::nanoseconds>(
													std::chrono::steady_clock::now() - TerrainStart
							)
													.count());
						PreparedView.Telemetry.Counters.ShadowSortingBatchingNanoseconds +=
							StaticMeshes.SortingNanoseconds
							+ SkeletalMeshes.SortingNanoseconds
							+ Terrains.BatchConstructionNanoseconds;
						PreparedView.Telemetry.Counters.
							ShadowStaticSplinePrimitiveFactBuilds +=
							StaticMeshes.SharedPrimitiveFactBuilds;
						PreparedView.Telemetry.Counters.
							ShadowStaticSplinePrimitiveFactReuses +=
							StaticMeshes.SharedPrimitiveFactReuses;
						PreparedView.Telemetry.Counters.ShadowSelectedLODFactBuilds +=
							StaticMeshes.SelectedLODFactBuilds;
						PreparedView.Telemetry.Counters.ShadowSelectedLODFactReuses +=
							StaticMeshes.SelectedLODFactReuses;
						PreparedView.Telemetry.Counters.
							ShadowStaticSplineSectionFactBuilds +=
							StaticMeshes.SharedSectionFactBuilds;
						PreparedView.Telemetry.Counters.
							ShadowStaticSplineSectionFactReuses +=
							StaticMeshes.SharedSectionFactReuses;
						PreparedView.Telemetry.Counters.ShadowSkeletalPrimitiveFactBuilds +=
							SkeletalMeshes.SharedPrimitiveFactBuilds;
						PreparedView.Telemetry.Counters.ShadowSkeletalPrimitiveFactReuses +=
							SkeletalMeshes.SharedPrimitiveFactReuses;
						PreparedView.Telemetry.Counters.ShadowSkeletalSectionFactBuilds +=
							SkeletalMeshes.SharedSectionFactBuilds;
						PreparedView.Telemetry.Counters.ShadowSkeletalSectionFactReuses +=
							SkeletalMeshes.SharedSectionFactReuses;
						PreparedView.Telemetry.Counters.ShadowTerrainPrimitiveFactBuilds +=
							Terrains.SharedPrimitiveFactBuilds;
						PreparedView.Telemetry.Counters.ShadowTerrainPrimitiveFactReuses +=
							Terrains.SharedPrimitiveFactReuses;
						PreparedView.Telemetry.Counters.ShadowTerrainPatchFactBuilds +=
							Terrains.SharedPatchFactBuilds;
						PreparedView.Telemetry.Counters.ShadowTerrainPatchFactReuses +=
							Terrains.SharedPatchFactReuses;
						PreparedView.Telemetry.Counters.
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
						PreparedView.Telemetry.Counters.ShadowPreparedStaticMeshCasters +=
							CascadeCounters.PreparedStaticMeshCasters;
						PreparedView.Telemetry.Counters.ShadowPreparedSplineMeshCasters +=
							CascadeCounters.PreparedSplineMeshCasters;
						PreparedView.Telemetry.Counters.ShadowPreparedSkeletalMeshCasters +=
							CascadeCounters.PreparedSkeletalMeshCasters;
						PreparedView.Telemetry.Counters.ShadowPreparedTerrainCasters +=
							CascadeCounters.PreparedTerrainCasters;
						PreparedView.Telemetry.Counters.ShadowPreparedTriangles +=
							CascadeCounters.PreparedTriangles;
					}
					PreparedView.Telemetry.Counters.ShadowLogicalPreparationNanoseconds =
						static_cast<uint64>(std::chrono::duration_cast<
												std::chrono::nanoseconds>(
												std::chrono::steady_clock::now()
												- ShadowPreparationStart
						)
												.count());
				}
				else if (Selected.Data.bCastShadows)
				{
					++PreparedView.Telemetry.Counters.ShadowInvalidReceiverViews;
					PreparedView.Telemetry.Counters.ShadowLogicalPreparationNanoseconds =
						static_cast<uint64>(std::chrono::duration_cast<
												std::chrono::nanoseconds>(
												std::chrono::steady_clock::now()
												- ShadowPreparationStart
						)
												.count());
					PreparedView.DirectionalShadow.reset();
					PreparedView.ResolvedDirectionalShadow.reset();
				}
				else
				{
					PreparedView.DirectionalShadow.reset();
					PreparedView.ResolvedDirectionalShadow.reset();
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
		const bool bRequiresDeferredOpaque =
			RenderView.Settings.Mode.RenderMode == ERenderMode::Lit
			&& RenderView.Settings.Mode.RasterMode == ERasterMode::Solid;
		StaticMeshRenderer.PrepareResources_RenderThread(
			CommandList, PreparedView.Receiver.StaticMeshes,
			PreparedView.ResolvedReceiver.StaticMeshes,
			!bRequiresDeferredOpaque
		);
		SkeletalMeshRenderer.PrepareResources_RenderThread(
			CommandList, PreparedView.Receiver.SkeletalPalettes,
			PreparedView.Receiver.SkeletalMeshes,
			PreparedView.ResolvedReceiver.SkeletalMeshes,
			!bRequiresDeferredOpaque
		);
		TerrainRenderer.PrepareResources_RenderThread(
			CommandList, PreparedView.Receiver.Terrains,
			PreparedView.ResolvedReceiver.Terrains,
			!bRequiresDeferredOpaque
		);
		if (PreparedView.DirectionalShadow)
			DirectionalShadowRenderer.PrepareResources_RenderThread(
				CommandList, StaticMeshRenderer, SkeletalMeshRenderer,
				TerrainRenderer, *PreparedView.DirectionalShadow,
				*PreparedView.ResolvedDirectionalShadow,
				PreparedView.Receiver.SkeletalPalettes, PreparedView.Telemetry.Counters
			);
		FRHITexture* DirectionalShadowTexture =
			DirectionalShadowRenderer.GetTexture_RenderThread();
		FRHISampler* DirectionalShadowSampler =
			DirectionalShadowRenderer.GetSampler_RenderThread();
		PreparedView.ResolvedReceiver.StaticMeshes.DirectionalShadowTexture =
			DirectionalShadowTexture;
		PreparedView.ResolvedReceiver.StaticMeshes.DirectionalShadowSampler =
			DirectionalShadowSampler;
		PreparedView.ResolvedReceiver.SkeletalMeshes.DirectionalShadowTexture =
			DirectionalShadowTexture;
		PreparedView.ResolvedReceiver.SkeletalMeshes.DirectionalShadowSampler =
			DirectionalShadowSampler;
		PreparedView.ResolvedReceiver.Terrains.DirectionalShadowTexture =
			DirectionalShadowTexture;
		PreparedView.ResolvedReceiver.Terrains.DirectionalShadowSampler =
			DirectionalShadowSampler;
		PrepareCombinedTranslucentGeometry(PreparedView.Receiver);
		PreparedView.Telemetry.Counters.CombinedTranslucentGeometryDraws =
			PreparedView.Receiver.TranslucentGeometry.size();
		const FForwardLightingUniform Lighting = BuildForwardLightingUniform(
			PreparedView.Lighting.Lights, RenderView,
			PreparedView.DirectionalShadow
					&& PreparedView.DirectionalShadow->View.bEnabled
					&& DirectionalShadowTexture != nullptr
					&& DirectionalShadowSampler != nullptr ?
				&PreparedView.DirectionalShadow->View :
				nullptr
		);
		PreparedView.Telemetry.Counters.PackedLightBytes = sizeof(Lighting);
		PreparedView.Lighting.UniformBuffer =
			CommandList.AllocateDynamicUniformBuffer(&Lighting, sizeof(Lighting));
		if (PreparedView.Lighting.UniformBuffer.Buffer == nullptr
			|| PreparedView.Lighting.UniformBuffer.Size != sizeof(Lighting))
		{
			return ERenderViewResult::RendererResourcesUnavailable;
		}

		// Every enabled view regenerates the shared fixed target before Scene Color.
		if (PreparedView.DirectionalShadow)
			DirectionalShadowRenderer.Render_RenderThread(
				CommandList, StaticMeshRenderer, SkeletalMeshRenderer,
				TerrainRenderer, *PreparedView.DirectionalShadow,
				*PreparedView.ResolvedDirectionalShadow,
				PreparedView.Telemetry.Counters
			);
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
				PreparedView.VolumetricCloud->Textures.DensitySampler =
					VolumetricCloudRenderer.EnsureDensitySampler_RenderThread();
			}
		}
		const FVolumetricCloudPreparationSink CloudPreparationSink =
			GetVolumetricCloudPreparationSink();
		if (CloudPreparationSink != nullptr && PreparedView.VolumetricCloud)
		{
			FVolumetricCloudQualificationOptions Qualification;
			CloudPreparationSink(Qualification);
			PreparedView.VolumetricCloud->bForceFragmentForQualification =
				Qualification.bForceFragment;
		}

		return ERenderViewResult::Success;
	}


} // namespace Durin
