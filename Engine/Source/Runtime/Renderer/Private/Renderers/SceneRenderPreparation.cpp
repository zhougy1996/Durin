#include "Renderers/SceneRenderPipeline.h"

#include "Renderers/SceneRenderPlan.h"
#include "Renderers/SceneRendererProfiling.h"
#include "Rendering/TerrainSceneProxy.h"
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
	auto FSceneRenderPipeline::PrepareView_RenderThread(
		FRHICommandListImmediate& CommandList,
		FScene* Scene,
		FSceneView& RenderView,
		const FSceneViewRenderOptions& Options
	) -> FSceneRenderPreparationResult
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
				*Scene, RenderView, Telemetry.View
			);
			const FSkyBoxSceneInfo* SkyBoxInfo =
				Scene->GetActiveSkyBoxSceneInfo_RenderThread();
			if (!PreparedView.Environment && SkyBoxInfo != nullptr)
			{
				PreparedView.Environment = FPreparedEnvironment{
					.SkyBox = SkyBoxInfo->GetProxy().GetData()};
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
					Telemetry.View.DirectionalShadow.ShadowUniqueEligibleSkeletalMeshCasters =
						CasterTable.UniqueEligibleSkeletalMeshes;
					Telemetry.View.DirectionalShadow.ShadowUniqueEligibleTerrainCasters =
						CasterTable.UniqueEligibleTerrains;
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
						Telemetry.View.DirectionalShadow.ShadowStaticSplinePreparationNanoseconds +=
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
						Telemetry.View.DirectionalShadow.ShadowSkeletalPreparationNanoseconds +=
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
						Telemetry.View.DirectionalShadow.ShadowTerrainLogicalPreparationNanoseconds +=
							static_cast<uint64>(std::chrono::duration_cast<
													std::chrono::nanoseconds>(
													std::chrono::steady_clock::now() - TerrainStart
							)
													.count());
						Telemetry.View.DirectionalShadow.ShadowSortingBatchingNanoseconds +=
							StaticMeshes.SortingNanoseconds
							+ SkeletalMeshes.SortingNanoseconds
							+ Terrains.BatchConstructionNanoseconds;
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
						Telemetry.View.DirectionalShadow.ShadowSkeletalPrimitiveFactBuilds +=
							SkeletalMeshes.SharedPrimitiveFactBuilds;
						Telemetry.View.DirectionalShadow.ShadowSkeletalPrimitiveFactReuses +=
							SkeletalMeshes.SharedPrimitiveFactReuses;
						Telemetry.View.DirectionalShadow.ShadowSkeletalSectionFactBuilds +=
							SkeletalMeshes.SharedSectionFactBuilds;
						Telemetry.View.DirectionalShadow.ShadowSkeletalSectionFactReuses +=
							SkeletalMeshes.SharedSectionFactReuses;
						Telemetry.View.DirectionalShadow.ShadowTerrainPrimitiveFactBuilds +=
							Terrains.SharedPrimitiveFactBuilds;
						Telemetry.View.DirectionalShadow.ShadowTerrainPrimitiveFactReuses +=
							Terrains.SharedPrimitiveFactReuses;
						Telemetry.View.DirectionalShadow.ShadowTerrainPatchFactBuilds +=
							Terrains.SharedPatchFactBuilds;
						Telemetry.View.DirectionalShadow.ShadowTerrainPatchFactReuses +=
							Terrains.SharedPatchFactReuses;
						Telemetry.View.DirectionalShadow.
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
						CascadeTelemetry.PreparedStaticMeshCasters =
							StaticMeshes.PreparedLocalPrimitives;
						CascadeTelemetry.PreparedSplineMeshCasters =
							StaticMeshes.PreparedSplinePrimitives;
						CascadeTelemetry.PreparedSkeletalMeshCasters =
							SkeletalMeshes.Primitives.size();
						CascadeTelemetry.PreparedTerrainCasters =
							Terrains.Opaque.size() + Terrains.Masked.size();
						size_t TerrainShadowTriangles = 0;
						for (const auto* Bucket : {&Terrains.Opaque, &Terrains.Masked})
							for (const FPreparedTerrainDraw& Draw : *Bucket)
								TerrainShadowTriangles += Draw.TriangleCount;
						CascadeTelemetry.PreparedTriangles =
							StaticMeshes.SelectedTriangles
							+ SkeletalMeshes.SelectedTriangles + TerrainShadowTriangles;
						Telemetry.View.DirectionalShadow.ShadowPreparedStaticMeshCasters +=
							CascadeTelemetry.PreparedStaticMeshCasters;
						Telemetry.View.DirectionalShadow.ShadowPreparedSplineMeshCasters +=
							CascadeTelemetry.PreparedSplineMeshCasters;
						Telemetry.View.DirectionalShadow.ShadowPreparedSkeletalMeshCasters +=
							CascadeTelemetry.PreparedSkeletalMeshCasters;
						Telemetry.View.DirectionalShadow.ShadowPreparedTerrainCasters +=
							CascadeTelemetry.PreparedTerrainCasters;
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
				auto AddTerrainDrawOverlay = [&PreparedView](const FPreparedTerrainDraw& Draw) {
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
						const FVector4f Color = bStitched
							? FVector4f{1.0f, 0.1f, 0.1f, 1.0f} : LevelColor;
						const FSimpleElementLineStyle Style{
							.WidthPixels = bStitched ? 3.0f : 2.0f};
						auto AppendLine = [&](ESceneDepthPriorityGroup Depth,
							ESimpleElementBlendMode Blend, FVector4f DrawColor) {
							auto& Elements =
								PreparedView.Context.RendererSimpleElements;
							Elements.push_back({
								.Type = ESimpleElementType::Line,
								.BlendMode = Blend,
								.DepthPriorityGroup = Depth,
								.SubmissionOrder = static_cast<uint64>(Elements.size()),
								.Value = FSimpleElementLine{World[Edge],
									World[(Edge + 1) % 4], DrawColor, Style},
							});
						};
						FVector4f ForegroundColor = Color;
						ForegroundColor.w *= 0.32f;
						AppendLine(ESceneDepthPriorityGroup::Foreground,
							ESimpleElementBlendMode::Translucent, ForegroundColor);
						AppendLine(ESceneDepthPriorityGroup::World,
							Color.w < 1.0f ? ESimpleElementBlendMode::Translucent
								: ESimpleElementBlendMode::Opaque, Color);
					}
				};
				for (const auto* Bucket : {&PreparedView.Receiver.Terrains.Opaque, &PreparedView.Receiver.Terrains.Masked, &PreparedView.Receiver.Terrains.Translucent})
					for (const FPreparedTerrainDraw& Draw : *Bucket)
						AddTerrainDrawOverlay(Draw);
			}
		}
		PrepareCombinedTranslucentGeometry(PreparedView.Receiver);
		Telemetry.View.CombinedTranslucentGeometryDraws =
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

	auto FSceneRenderPipeline::ResolveFrameResources_RenderThread(
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
				ResolvedFrame.Receiver.SkeletalPalettes, Telemetry.View);
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
		Telemetry.View.Lighting.PackedLightBytes = sizeof(Lighting);
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

	auto FSceneRenderPipeline::BuildFrameTopology(
		const FSceneRenderPlan& PreparedView,
		const FSceneViewRenderOptions& Options,
		uint32 Width,
		uint32 Height
	) const -> FSceneRenderTopology
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
		const bool bForceContactShadowVisibilityFragment =
			Qualification.bForceFragmentContactVisibility
			|| View.Settings.DirectionalShadow.ContactRoutePreference
				== EContactShadowRoutePreference::Fragment;
		const bool bForceContactShadowVisibilityCompute =
			!Qualification.bForceFragmentContactVisibility
			&& View.Settings.DirectionalShadow.ContactRoutePreference
				== EContactShadowRoutePreference::Compute;
		const bool bCloudShadow = bProductionDeferred
			&& PreparedView.VolumetricCloud
			&& !PreparedView.Lighting.Lights.Directional.empty();
		const bool bCloudInputs = bProductionDeferred
			&& PreparedView.VolumetricCloud
			&& PreparedView.VolumetricCloud->Textures.BaseDensity
			&& PreparedView.VolumetricCloud->Textures.DetailDensity;
		const auto CloudQuality = CanonicalizeVolumetricCloudQuality(
			View.Settings.VolumetricCloud.Quality);
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
			.ContactShadowVisibility = !bContact ? ESceneRenderRoute::Disabled
				: (bForceContactShadowVisibilityFragment ? ESceneRenderRoute::Fragment
					: ESceneRenderRoute::Compute),
			.VolumetricCloudShadow = !bCloudShadow
				? ESceneRenderRoute::Disabled
				: (bForceCloudFragment ? ESceneRenderRoute::Fragment
					: ESceneRenderRoute::Compute),
			.bIsolatedDeferred = bIsolatedDeferred,
			.bGBufferDebug =
				Options.GBufferDebugMode != EGBufferDebugMode::Disabled,
			.VolumetricCloud = !bCloudInputs ? ESceneRenderRoute::Disabled
				: (bForceCloudFragment ? ESceneRenderRoute::Fragment
					: ESceneRenderRoute::Compute),
			.bVolumetricCloudComposite = bCloudInputs,
			.AmbientOcclusionQuality = View.Settings.AmbientOcclusion.Quality,
			.VolumetricCloudExtent = {
				static_cast<int32>(CloudExtent.Width),
				static_cast<int32>(CloudExtent.Height)}};
	}

} // namespace Durin
