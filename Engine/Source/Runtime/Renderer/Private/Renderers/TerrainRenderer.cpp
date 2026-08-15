#include "Renderers/TerrainRenderer.h"
#include "Renderers/TerrainRenderPreparation.h"

#include "Engine/TerrainSceneProxy.h"
#include "Math/Operations.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RendererResourceSlotCache.h"
#include "Renderers/RendererResourceDiagnostics.h"
#include "Renderers/GBufferRenderer.h"
#include "Renderers/ViewPreparationMath.h"
#include "Renderers/DirectionalShadowView.h"
#include "SceneViewProjection.h"
#include "RenderingThread.h"
#include "Resources/DefaultTextureResources.h"
#include "Resources/EnvironmentLightingResources.h"
#include "Resources/RendererResourceCoordinator.h"
#include "Resources/RenderTargetLayouts.h"
#include "Shader/Shader.h"
#include "Shader/ShaderCompilerCore.h"
#include "Terrain/TerrainHeightmap.h"
#include "Terrain/TerrainLOD.h"
#include "Terrain/TerrainTopology.h"
#include "Terrain/TerrainVertexFactory.h"

#include <glm/mat3x3.hpp>
#include <glm/matrix.hpp>

#include <bit>
#include <chrono>
#include <unordered_map>

namespace Durin
{
	namespace
	{
		constexpr size_t MaximumRetainedTerrainHeightRevisions = 64;
		constexpr size_t MaximumRetainedTerrainTopologies = 256;
		constexpr uint32 MaximumTerrainInstancesPerChunk = 256;

		struct alignas(16) FTerrainInstanceData
		{
			std::array<uint32, 2> SampleOrigin{};
			FVector2f Padding{0.0f};
			FVector4f AnchorClip{0.0f};
			FVector4f AnchorRelativeWorld{0.0f};
		};
		static_assert(sizeof(FTerrainInstanceData) == TerrainInstanceDataBytes);
		class FTerrainVertexShader final : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FTerrainVertexShader)
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Transform);
				DURIN_SHADER_PARAMETER_TEXTURE(HeightTexture);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Terrain);
				DURIN_SHADER_PARAMETER_STORAGE_BUFFER(TerrainPatchOrigins);
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_SHADER(FTerrainVertexShader, FShader, "/Engine/StaticMeshBasePass", EShaderFrequency::Vertex, "VertexMain");
		};

		class FTerrainFragmentShader final : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FTerrainFragmentShader)
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Lighting);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Material);
				DURIN_SHADER_PARAMETER_TEXTURE(BaseColorTexture);
				DURIN_SHADER_PARAMETER_TEXTURE(NormalTexture);
				DURIN_SHADER_PARAMETER_TEXTURE(MetallicTexture);
				DURIN_SHADER_PARAMETER_TEXTURE(RoughnessTexture);
				DURIN_SHADER_PARAMETER_TEXTURE(AmbientOcclusionTexture);
				DURIN_SHADER_PARAMETER_TEXTURE(EmissiveTexture);
				DURIN_SHADER_PARAMETER_TEXTURE(OpacityTexture);
				DURIN_SHADER_PARAMETER_TEXTURE(OpacityMaskTexture);
				DURIN_SHADER_PARAMETER_SAMPLER(BaseColorSampler);
				DURIN_SHADER_PARAMETER_SAMPLER(NormalSampler);
				DURIN_SHADER_PARAMETER_SAMPLER(MetallicSampler);
				DURIN_SHADER_PARAMETER_SAMPLER(RoughnessSampler);
				DURIN_SHADER_PARAMETER_SAMPLER(AmbientOcclusionSampler);
				DURIN_SHADER_PARAMETER_SAMPLER(EmissiveSampler);
				DURIN_SHADER_PARAMETER_SAMPLER(OpacitySampler);
				DURIN_SHADER_PARAMETER_SAMPLER(OpacityMaskSampler);
				DURIN_SHADER_PARAMETER_TEXTURE(EnvironmentIrradiance);
				DURIN_SHADER_PARAMETER_TEXTURE(EnvironmentPrefiltered);
				DURIN_SHADER_PARAMETER_TEXTURE(EnvironmentBrdfLut);
				DURIN_SHADER_PARAMETER_SAMPLER(EnvironmentSampler);
				DURIN_SHADER_PARAMETER_TEXTURE(DirectionalShadowTexture);
				DURIN_SHADER_PARAMETER_SAMPLER(DirectionalShadowSampler);
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_SHADER(FTerrainFragmentShader, FShader, "/Engine/StaticMeshBasePass", EShaderFrequency::Fragment, "FragmentMain");
		};

		class FTerrainOpaqueShadowFragmentShader final : public FShader
		{
		public:
			DURIN_DECLARE_SHADER(FTerrainOpaqueShadowFragmentShader, FShader, "/Engine/StaticMeshBasePass", EShaderFrequency::Fragment, "OpaqueShadowFragmentMain");
		};

		class FTerrainShadowFragmentShader final : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FTerrainShadowFragmentShader)
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Material);
				DURIN_SHADER_PARAMETER_TEXTURE(OpacityMaskTexture);
				DURIN_SHADER_PARAMETER_SAMPLER(OpacityMaskSampler);
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_SHADER(FTerrainShadowFragmentShader, FShader, "/Engine/StaticMeshBasePass", EShaderFrequency::Fragment, "ShadowFragmentMain");
		};

		struct FTransformUniform
		{
			FMatrix4f LocalToClip{1.0f};
			FMatrix4f LocalToWorld{1.0f};
			FMatrix4f NormalToWorld{1.0f};
			FVector4f TransformParams{1.0f, 0.0f, 0.0f, 0.0f};
		};

		struct FTerrainUniform
		{
			std::array<uint32, 4> SamplePatch{};
			FVector4f SpacingHeight{1.0f, 1.0f, 1.0f, 0.0f};
			FVector4f DistanceTransition{0.0f};
		};

		struct FMaterialUniform
		{
			FVector4f BaseColor{1.0f};
			FVector4f EmissiveMetallic{0.0f};
			FVector4f NormalRoughness{0.0f, 0.0f, 1.0f, 0.5f};
			FVector4f SurfaceParams{1.0f, 1.0f, 1.0f, 0.0f};
			std::array<FVector4f, 8> UVTransforms{};
			FVector4f UVChannels0{0.0f};
			FVector4f UVChannels1{0.0f};
			FVector4f UVRotations0{0.0f};
			FVector4f UVRotations1{0.0f};
		};

		auto ToShaderMatrix(const FMatrix& Matrix) -> FMatrix4f
		{
			FMatrix4f Result(0.0f);
			for (uint32 Column = 0; Column < 4; ++Column)
				for (uint32 Row = 0; Row < 4; ++Row)
					Result[Column][Row] = static_cast<float>(Matrix[Row][Column]);
			return Result;
		}

		auto TransformBounds(const FBox& Bounds, const FMatrix& Transform) -> FBox
		{
			FBox Result;
			if (!Bounds.bIsValid || !Math::IsFinite(Transform)) return Result;
			for (uint32 Corner = 0; Corner < 8; ++Corner)
			{
				const FVector3 Point(
					(Corner & 1u) ? Bounds.Max.x : Bounds.Min.x,
					(Corner & 2u) ? Bounds.Max.y : Bounds.Min.y,
					(Corner & 4u) ? Bounds.Max.z : Bounds.Min.z
				);
				Result.AddPoint(FVector3(Transform * FVector4(Point, 1.0)));
			}
			return Result;
		}

		struct FTerrainDistancePolicy
		{
			double FadeStart = SceneViewProjection::DefaultViewFadeStart;
			double RenderDistance = SceneViewProjection::DefaultViewRenderDistance;
			bool bFallback = false;
		};

		auto ResolveTerrainDistancePolicy(const FSceneView& View)
			-> FTerrainDistancePolicy
		{
			FTerrainDistancePolicy Result{
				View.ViewFadeStart, View.ViewRenderDistance, false
			};
			const bool bValid = std::isfinite(Result.FadeStart)
								&& std::isfinite(Result.RenderDistance) && Result.FadeStart >= 0.0
								&& Result.FadeStart < Result.RenderDistance
								&& Result.RenderDistance
										   + SceneViewProjection::GetTerrainFarPlaneSafetyMargin(
											   View.FarClipDistance
										   )
									   < View.FarClipDistance;
			if (bValid) return Result;
			Result = {};
			Result.RenderDistance = std::min(Result.RenderDistance, std::max(1.0, View.FarClipDistance - SceneViewProjection::GetTerrainFarPlaneSafetyMargin(View.FarClipDistance)));
			Result.FadeStart = std::min(Result.FadeStart, Result.RenderDistance * 0.9);
			Result.bFallback = true;
			return Result;
		}

		auto GetClosestHorizontalDistance(const FBox& Bounds, const FVector3& Location) -> double
		{
			const double DeltaX = Location.x < Bounds.Min.x ? Bounds.Min.x - Location.x : Location.x > Bounds.Max.x ? Location.x - Bounds.Max.x :
																													  0.0;
			const double DeltaY = Location.y < Bounds.Min.y ? Bounds.Min.y - Location.y : Location.y > Bounds.Max.y ? Location.y - Bounds.Max.y :
																													  0.0;
			return std::hypot(DeltaX, DeltaY);
		}

		auto GetSamplerKey(const FMaterialSamplerState& State) -> size_t
		{
			return static_cast<size_t>(State.MinFilter) + 6 * (static_cast<size_t>(State.MagFilter) + 2 * (static_cast<size_t>(State.AddressU) + 3 * static_cast<size_t>(State.AddressV)));
		}

		auto ToAddress(EMaterialSamplerAddressMode Address) -> ESamplerAddressMode
		{
			if (Address == EMaterialSamplerAddressMode::MirroredRepeat) return ESamplerAddressMode::MirroredRepeat;
			if (Address == EMaterialSamplerAddressMode::ClampToEdge) return ESamplerAddressMode::ClampToEdge;
			return ESamplerAddressMode::Repeat;
		}

		auto MakeSampler(const FMaterialSamplerState& State) -> FRHISamplerDesc
		{
			FRHISamplerDesc Result;
			const uint8 Min = static_cast<uint8>(State.MinFilter);
			Result.MinFilter = (Min & 1u) ? ESamplerFilter::Linear : ESamplerFilter::Nearest;
			Result.MagFilter = State.MagFilter == EMaterialSamplerMagFilter::Linear ? ESamplerFilter::Linear : ESamplerFilter::Nearest;
			Result.MipmapMode = Min >= 4 ? ESamplerMipmapMode::Linear : ESamplerMipmapMode::Nearest;
			Result.MaxLod = Min < 2 ? 0.0f : 1000.0f;
			Result.AddressU = ToAddress(State.AddressU);
			Result.AddressV = ToAddress(State.AddressV);
			Result.AddressW = ESamplerAddressMode::Repeat;
			return Result;
		}

		auto MakeShadowRasterizerState(const FRHIRasterizerState& Source)
			-> FRHIRasterizerState
		{
			FRHIRasterizerState Result = Source;
			Result.PolygonMode = ERHIPolygonMode::Fill;
			const bool bPreparedBias = Result.bEnableDepthBias;
			Result.bEnableDepthBias = true;
			if (!bPreparedBias)
			{
				Result.DepthBiasConstantFactor =
					DirectionalShadowDepthBiasConstant;
				Result.DepthBiasSlopeFactor =
					DirectionalShadowDepthBiasSlope;
				Result.DepthBiasClamp = DirectionalShadowDepthBiasClamp;
			}
			return Result;
		}

		auto MakeShadowPipelineKey(
			const FEffectiveStaticMeshPipelineKey& Source
		)
			-> FEffectiveStaticMeshPipelineKey
		{
			FEffectiveStaticMeshPipelineKey Result = Source;
			Result.Rasterizer = MakeShadowRasterizerState(Source.Rasterizer);
			Result.Rasterizer.DepthBiasConstantFactor = 0.0f;
			Result.Rasterizer.DepthBiasSlopeFactor = 0.0f;
			Result.Rasterizer.DepthBiasClamp = 0.0f;
			Result.Depth.bEnableTest = true;
			Result.Depth.bEnableWrite = true;
			Result.Depth.CompareOp = ERHIDepthCompareOp::Less;
			Result.ColorBlend = {};
			return Result;
		}

		auto ResolveMaterialBinding(FPreparedTerrainDraw& Draw) -> bool
		{
			FMaterialRenderValidationDiagnostic Diagnostic;
			if (TryGetMaterialRenderV3Binding(Draw.Material.Representation, Draw.MaterialBinding, Diagnostic)) return true;
			if (Draw.Material.Representation.GetLayout().Identity.Version == 2)
			{
				FMaterialRenderV2Binding Legacy;
				if (TryGetMaterialRenderV2Binding(Draw.Material.Representation, Legacy, Diagnostic))
				{
					static_cast<FMaterialRenderV2Binding&>(Draw.MaterialBinding) = std::move(Legacy);
					return true;
				}
			}
			RecordMaterialFallbackReason(EMaterialFallbackReason::UnsupportedLayout);
			Draw.Material = GetErrorMaterialRenderData();
			return TryGetMaterialRenderV3Binding(Draw.Material.Representation, Draw.MaterialBinding, Diagnostic);
		}

		auto AreTerrainDrawsBatchCompatible(
			const FPreparedTerrainDraw& A, const FPreparedTerrainDraw& B
		) -> bool
		{
			return A.SceneInfo == B.SceneInfo && A.Pass == B.Pass
				   && A.PipelineKey == B.PipelineKey
				   && A.Patch && B.Patch
				   && A.Patch->CellCountX == B.Patch->CellCountX
				   && A.Patch->CellCountY == B.Patch->CellCountY
				   && A.LODStep == B.LODStep && A.StitchMask == B.StitchMask
				   && A.DirectionalShadowTexture == B.DirectionalShadowTexture
				   && A.DirectionalShadowSampler == B.DirectionalShadowSampler;
		}

		auto BuildTerrainBatches(const std::vector<FPreparedTerrainDraw>& Draws, std::vector<FPreparedTerrainBatch>& OutBatches, bool bDisableBatching) -> void
		{
			OutBatches.clear();
			for (uint32 DrawIndex = 0; DrawIndex < Draws.size(); ++DrawIndex)
			{
				if (OutBatches.empty()
					|| bDisableBatching
					|| OutBatches.back().DrawIndices.size() >= MaximumTerrainInstancesPerChunk
					|| !AreTerrainDrawsBatchCompatible(
						Draws[OutBatches.back().DrawIndices.front()], Draws[DrawIndex]
					))
					OutBatches.emplace_back();
				OutBatches.back().DrawIndices.push_back(DrawIndex);
			}
		}
	} // namespace

	auto PrepareTerrainView_RenderThread(
		std::span<const FPrimitiveSceneInfo* const> SceneInfos,
		const FSceneView& View,
		ERasterMode RasterMode
	) -> FPreparedTerrainView
	{
		check(IsInRenderingThread());
		FPreparedTerrainView Result;
		const auto LogicalBegin = std::chrono::steady_clock::now();
		FViewFrustum Frustum;
		const bool bCull = View.Settings.VisibilityMode == EViewVisibilityMode::Normal;
		const bool bValidView = !bCull || TryBuildViewFrustum(View, Frustum);
		const bool bApplyDistance =
			View.DepthConvention == ESceneDepthConvention::ReversedZ;
		const FTerrainDistancePolicy DistancePolicy =
			ResolveTerrainDistancePolicy(View);
		Result.InvalidDistanceSettingFallbacks +=
			bApplyDistance && DistancePolicy.bFallback ? 1u : 0u;
		for (const FPrimitiveSceneInfo* Info : SceneInfos)
		{
			if (!Info || Info->GetKind() != EPrimitiveSceneProxyKind::Terrain) continue;
			const FTerrainSceneProxy& Proxy = Info->GetTerrainProxy();
			const FMatrix& Transform = Info->GetTransform();
			if (!Math::IsFinite(Transform)) continue;
			const double Determinant = glm::determinant(glm::mat3(Transform));
			if (!std::isfinite(Determinant)) continue;
			FPreparedTerrainDraw CommonDraw;
			CommonDraw.SceneInfo = Info;
			CommonDraw.Material = Proxy.ResolveMaterialRenderData_RenderThread();
			if (!ResolveMaterialBinding(CommonDraw)) continue;
			CommonDraw.PipelineKey.Material = CommonDraw.Material.PipelineIdentity;
			CommonDraw.PipelineKey.Rasterizer.PolygonMode =
				RasterMode == ERasterMode::Wireframe ? ERHIPolygonMode::Line : ERHIPolygonMode::Fill;
			CommonDraw.PipelineKey.Rasterizer.CullMode =
				CommonDraw.Material.PipelineIdentity.bTwoSided ? ERHICullMode::None : ERHICullMode::Back;
			CommonDraw.PipelineKey.Rasterizer.FrontFace = Determinant < 0.0 ? ERHIFrontFace::CounterClockwise : ERHIFrontFace::Clockwise;
			CommonDraw.PipelineKey.Depth.bEnableTest = true;
			CommonDraw.PipelineKey.Depth.CompareOp =
				View.DepthConvention == ESceneDepthConvention::ReversedZ ? ERHIDepthCompareOp::GreaterOrEqual : ERHIDepthCompareOp::Less;
			const auto CommonBlend =
				CommonDraw.Material.PipelineIdentity.ShaderMap.BlendMode;
			CommonDraw.Pass = CommonBlend == EMaterialBlendMode::Masked ? EStaticMeshBasePass::Masked : CommonBlend == EMaterialBlendMode::Translucent ? EStaticMeshBasePass::Translucent :
																																						 EStaticMeshBasePass::Opaque;
			const auto CommonDepth =
				CommonDraw.Material.PipelineIdentity.DepthWritePolicy;
			CommonDraw.PipelineKey.Depth.bEnableWrite =
				CommonDepth == EMaterialDepthWritePolicy::Enabled
				|| (CommonDepth == EMaterialDepthWritePolicy::Automatic
					&& CommonDraw.Pass != EStaticMeshBasePass::Translucent);
			if (CommonDraw.Pass == EStaticMeshBasePass::Translucent)
				CommonDraw.PipelineKey.ColorBlend = FRHIColorBlendState::StraightAlpha();
			const auto Patches = Proxy.GetPatches();
			std::vector<uint32> RequestedLODs;
			RequestedLODs.reserve(Patches.size());
			for (const FTerrainPatchDescriptor& Patch : Patches)
			{
				const FTerrainLODSelection Selection = SelectTerrainPatchLOD(View, Transform, Patch);
				RequestedLODs.push_back(Selection.LODIndex);
				Result.LODFallbacks += Selection.bFallback ? 1u : 0u;
				if (Result.RequestedLODHistogram.size() <= Selection.LODIndex)
					Result.RequestedLODHistogram.resize(Selection.LODIndex + 1);
				++Result.RequestedLODHistogram[Selection.LODIndex];
			}
			FTerrainLODResolution Resolution = ResolveTerrainPatchAdjacency(Patches, RequestedLODs);
			if (!Resolution.bValid)
			{
				++Result.LODResolutionFallbacks;
				Resolution.ResolvedLODs.assign(Patches.size(), 0);
				Resolution.StitchMasks.assign(Patches.size(), 0);
			}
			Result.AdjacencyPromotions += Resolution.Promotions;
			Result.AdjacencyIterations += Resolution.Iterations;
			for (uint32 LOD : Resolution.ResolvedLODs)
			{
				if (Result.ResolvedLODHistogram.size() <= LOD)
					Result.ResolvedLODHistogram.resize(LOD + 1);
				++Result.ResolvedLODHistogram[LOD];
			}
			for (size_t PatchIndex = 0; PatchIndex < Patches.size(); ++PatchIndex)
			{
				const FTerrainPatchDescriptor& Patch = Patches[PatchIndex];
				++Result.PatchCandidates;
				const FBox WorldBounds = TransformBounds(Patch.LocalBounds, Transform);
				if (bApplyDistance && WorldBounds.bIsValid
					&& Math::IsFinite(WorldBounds.Min)
					&& Math::IsFinite(WorldBounds.Max))
				{
					const double Distance = GetClosestHorizontalDistance(
						WorldBounds, View.ViewLocation
					);
					if (Distance > DistancePolicy.RenderDistance)
					{
						++Result.RadialRejectedPatches;
						continue;
					}
				}
				if (bCull && bValidView)
				{
					const auto Classification = ClassifyWorldBounds(Frustum, WorldBounds);
					if (Classification == EViewBoundsClassification::Outside)
					{
						++Result.CulledPatches;
						continue;
					}
					if (Classification == EViewBoundsClassification::InvalidBounds)
						++Result.InvalidBoundsFallbacks;
				}
				FPreparedTerrainDraw Draw = CommonDraw;
				Draw.Patch = &Patch;
				Draw.RequestedLOD = RequestedLODs[PatchIndex];
				Draw.ResolvedLOD = Resolution.ResolvedLODs[PatchIndex];
				Draw.LODStep = Patch.LODSteps[Draw.ResolvedLOD];
				Draw.StitchMask = Resolution.StitchMasks[PatchIndex];
				if (bApplyDistance && WorldBounds.bIsValid
					&& Math::IsFinite(WorldBounds.Min)
					&& Math::IsFinite(WorldBounds.Max))
				{
					const double Distance = GetClosestHorizontalDistance(
						WorldBounds, View.ViewLocation
					);
					if (Distance <= DistancePolicy.FadeStart)
						++Result.InnerPatches;
					else
					{
						++Result.TransitionPatches;
					}
				}
				else
					++Result.InnerPatches;
				const FTerrainTopologyKey TopologyKey{
					static_cast<uint16>(Patch.CellCountX),
					static_cast<uint16>(Patch.CellCountY),
					static_cast<uint16>(Draw.LODStep), Draw.StitchMask
				};
				Draw.TriangleCount = GetTerrainTopologyTriangleCount(TopologyKey);
				if (Draw.TriangleCount == 0) continue;
				const FVector3 Center = WorldBounds.bIsValid ? WorldBounds.GetCenter() : FVector3(Transform * FVector4(0.0, 0.0, 0.0, 1.0));
				const FVector3 Offset = Center - View.ViewLocation;
				Draw.TranslucentDistanceSquared = glm::dot(Offset, Offset);
				Draw.SortKey.Pipeline[0] = static_cast<uint32>(Draw.Pass);
				Draw.SortKey.Pipeline[1] = Draw.Material.PipelineIdentity.ShaderMap.RenderLayout.Version;
				Draw.SortKey.Geometry[0] = Patch.CellCountX;
				Draw.SortKey.Geometry[1] = Patch.CellCountY;
				Draw.SortKey.Geometry[2] = Draw.LODStep;
				Draw.SortKey.Geometry[3] = Draw.StitchMask;
				Draw.SortKey.PrimitiveId = Info->GetId().Value;
				Draw.SortKey.SectionIndex = static_cast<uint32>(Result.VisiblePatches);
				const auto Bytes = Draw.Material.Representation.GetUniformPayload();
				for (std::byte Byte : Bytes)
					Draw.SortKey.MaterialUniform.push_back(std::to_integer<uint8>(Byte));
				++Result.VisiblePatches;
				Result.Triangles += Draw.TriangleCount;
				++Result.StitchMaskHistogram[Draw.StitchMask];
				auto& Bucket = Draw.Pass == EStaticMeshBasePass::Opaque ? Result.Opaque : Draw.Pass == EStaticMeshBasePass::Masked ? Result.Masked :
																																	 Result.Translucent;
				Bucket.push_back(std::move(Draw));
			}
		}
		const auto LogicalEnd = std::chrono::steady_clock::now();
		auto SortOpaque = [](auto& Bucket) { std::ranges::sort(Bucket, [](const auto& A, const auto& B) { return std::tie(A.SortKey.Pipeline, A.SortKey.MaterialUniform, A.SortKey.PrimitiveId, A.SortKey.Geometry, A.SortKey.SectionIndex) < std::tie(B.SortKey.Pipeline, B.SortKey.MaterialUniform, B.SortKey.PrimitiveId, B.SortKey.Geometry, B.SortKey.SectionIndex); }); };
		SortOpaque(Result.Opaque);
		SortOpaque(Result.Masked);
		std::ranges::sort(Result.Translucent, [](const auto& A, const auto& B) {
			return A.TranslucentDistanceSquared != B.TranslucentDistanceSquared ? A.TranslucentDistanceSquared > B.TranslucentDistanceSquared : A.SortKey.PrimitiveId < B.SortKey.PrimitiveId;
		});
		BuildTerrainBatches(Result.Opaque, Result.OpaqueBatches, View.Settings.bDisableTerrainBatching);
		BuildTerrainBatches(Result.Masked, Result.MaskedBatches, View.Settings.bDisableTerrainBatching);
		Result.PreparedBatches = Result.OpaqueBatches.size() + Result.MaskedBatches.size();
		Result.BatchChunks = Result.PreparedBatches;
		for (const auto* Batches : {&Result.OpaqueBatches, &Result.MaskedBatches})
			for (const auto& Batch : *Batches)
				Result.InstanceCount += Batch.DrawIndices.size();
		check(Result.PatchCandidates == Result.VisiblePatches + Result.CulledPatches + Result.RadialRejectedPatches);
		check(Result.VisiblePatches == Result.InnerPatches + Result.TransitionPatches);
		const auto BatchEnd = std::chrono::steady_clock::now();
		Result.LogicalPreparationNanoseconds = std::chrono::duration_cast<
												   std::chrono::nanoseconds>(LogicalEnd - LogicalBegin)
												   .count();
		Result.BatchConstructionNanoseconds = std::chrono::duration_cast<
												  std::chrono::nanoseconds>(BatchEnd - LogicalEnd)
												  .count();
		return Result;
	}

	struct FTerrainRenderer::FState
	{
		struct FTopology
		{
			FBufferRHIRef Vertices;
			FBufferRHIRef Indices;
			FTerrainVertexFactory VertexFactory;
			uint32 IndexCount = 0;
		};
		struct FHeight
		{
			std::shared_ptr<const FTerrainHeightmapPayload> Payload;
			FTextureRHIRef Texture;
		};
		struct FShaderPayload
		{
			std::shared_ptr<FShaderMapBase> Map;
			TShaderRef<FTerrainVertexShader> Vertex;
			TShaderRef<FTerrainFragmentShader> Fragment;
			TShaderRef<FTerrainOpaqueShadowFragmentShader> OpaqueShadowFragment;
			TShaderRef<FTerrainShadowFragmentShader> ShadowFragment;
		};
		struct FPipelinePayload
		{
			std::shared_ptr<FShaderMapBase> Map;
			TShaderRef<FTerrainVertexShader> Vertex;
			TShaderRef<FTerrainFragmentShader> Fragment;
			TShaderRef<FTerrainOpaqueShadowFragmentShader> OpaqueShadowFragment;
			TShaderRef<FTerrainShadowFragmentShader> ShadowFragment;
			FGraphicsPipelineStateRHIRef Pipeline;
		};
		std::unordered_map<FTerrainTopologyKey, std::unique_ptr<FTopology>> Topologies;
		std::unordered_map<const FTerrainHeightmapPayload*, FHeight> Heights;
		std::unordered_map<size_t, FSamplerRHIRef> Samplers;
		TRendererResourceSlotCache<FMaterialShaderMapIdentity, FShaderPayload> Shaders{
			ERenderResourceGenerationDependency::Shader
		};
		TRendererResourceSlotCache<FMaterialShaderMapIdentity, FShaderPayload>
			ShadowShaders{ERenderResourceGenerationDependency::Shader};
		TRendererResourceSlotCache<FEffectiveStaticMeshPipelineKey, FPipelinePayload> Pipelines{
			ERenderResourceGenerationDependency::Shader | ERenderResourceGenerationDependency::Device
		};
		TRendererResourceSlotCache<FEffectiveStaticMeshPipelineKey, FPipelinePayload> ShadowPipelines{
			ERenderResourceGenerationDependency::Shader | ERenderResourceGenerationDependency::Device
		};
	};

	FTerrainRenderer::FTerrainRenderer(FRendererResourceCoordinator& InCoordinator, FDefaultTextureResources& InDefaultTextures, FEnvironmentLightingResources& InEnvironmentLighting)
		: Coordinator(InCoordinator)
		, DefaultTextures(InDefaultTextures)
		, EnvironmentLighting(InEnvironmentLighting)
		, State(std::make_unique<FState>())
	{
	}

	FTerrainRenderer::~FTerrainRenderer() = default;

	auto FTerrainRenderer::EnsureDrawResources_RenderThread(
		FRHICommandListImmediate& CommandList, FPreparedTerrainDraw& Draw, FPreparedTerrainView& View, bool bShadowDepth, bool bHybridRetained, bool bPrepareForwardPipeline
	) -> bool
	{
		if (!Draw.SceneInfo || !Draw.Patch || GDynamicRHI == nullptr) return false;
		const FTerrainSceneProxy& Proxy = Draw.SceneInfo->GetTerrainProxy();
		const auto Payload = Proxy.GetPayload();
		if (!Payload || !Payload->HasValidLayout()) return false;
		const auto HeightBegin = std::chrono::steady_clock::now();
		auto HeightIt = State->Heights.find(Payload.get());
		if (HeightIt == State->Heights.end())
		{
			if (State->Heights.size() >= MaximumRetainedTerrainHeightRevisions) return false;
			FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(
											 "TerrainHeight", Payload->Width, Payload->Height, EPixelFormat::R16_UINT
			)
											 .SetFlags(ETextureCreateFlags::ShaderResource);
			FState::FHeight Candidate{Payload, GDynamicRHI->RHICreateTexture(CommandList, Desc)};
			if (!Candidate.Texture) return false;
			GDynamicRHI->RHIUpdateTexture2D(CommandList, Candidate.Texture, 0, 0, FUpdateTextureRegion2D(0, 0, 0, 0, Payload->Width, Payload->Height), Payload->Width * sizeof(uint16), reinterpret_cast<const uint8*>(Payload->Samples.data()));
			View.HeightUploadBytes += Payload->GetSampleBytes();
			++View.HeightUploads;
			HeightIt = State->Heights.emplace(Payload.get(), std::move(Candidate)).first;
		}
		else
			++View.HeightReuses;
		View.HeightPreparationNanoseconds += std::chrono::duration_cast<
												 std::chrono::nanoseconds>(
												 std::chrono::steady_clock::now() - HeightBegin
		)
												 .count();
		const auto TopologyBegin = std::chrono::steady_clock::now();
		const FTerrainTopologyKey TopologyKey{
			static_cast<uint16>(Draw.Patch->CellCountX),
			static_cast<uint16>(Draw.Patch->CellCountY),
			static_cast<uint16>(Draw.LODStep), Draw.StitchMask
		};
		auto TopologyIt = State->Topologies.find(TopologyKey);
		if (TopologyIt == State->Topologies.end())
		{
			if (State->Topologies.size() >= MaximumRetainedTerrainTopologies) return false;
			FTerrainTopologyData TopologyData;
			if (!BuildTerrainTopology(TopologyKey, TopologyData)) return false;
			auto Candidate = std::make_unique<FState::FTopology>();
			FRHIBufferCreateDesc VertexDesc = FRHIBufferCreateDesc::CreateVertex("TerrainGrid", static_cast<uint32>(TopologyData.Vertices.size() * sizeof(TopologyData.Vertices[0])));
			VertexDesc.Usage |= EBufferUsageFlags::Static;
			VertexDesc.InitialData = {TopologyData.Vertices.data(), VertexDesc.Size};
			Candidate->Vertices = GDynamicRHI->RHICreateBuffer(CommandList, VertexDesc);
			FRHIBufferCreateDesc IndexDesc = FRHIBufferCreateDesc::CreateIndex("TerrainIndices", static_cast<uint32>(TopologyData.Indices.size() * sizeof(uint16)), sizeof(uint16));
			IndexDesc.Usage |= EBufferUsageFlags::Static;
			IndexDesc.InitialData = {TopologyData.Indices.data(), IndexDesc.Size};
			Candidate->Indices = GDynamicRHI->RHICreateBuffer(CommandList, IndexDesc);
			Candidate->IndexCount = static_cast<uint32>(TopologyData.Indices.size());
			if (!Candidate->Vertices || !Candidate->Indices
				|| !Candidate->VertexFactory.Initialize(Candidate->Vertices, static_cast<uint32>(TopologyData.Vertices.size()))) return false;
			Candidate->VertexFactory.InitResource(CommandList);
			if (!Candidate->VertexFactory.IsReady()) return false;
			View.TopologyBytes += TopologyData.Vertices.size() * sizeof(TopologyData.Vertices[0])
								  + TopologyData.Indices.size() * sizeof(uint16);
			++View.TopologyCreations;
			TopologyIt = State->Topologies.emplace(TopologyKey, std::move(Candidate)).first;
		}
		else
			++View.TopologyReuses;
		View.TopologyPreparationNanoseconds += std::chrono::duration_cast<
												   std::chrono::nanoseconds>(
												   std::chrono::steady_clock::now() - TopologyBegin
		)
												   .count();

		if (bPrepareForwardPipeline)
		{
			const auto ShaderBegin = std::chrono::steady_clock::now();
			auto& ShaderCache = bShadowDepth ? State->ShadowShaders : State->Shaders;
			auto& ShaderEntry = ShaderCache.FindOrAdd(
				Draw.Material.PipelineIdentity.ShaderMap
			);
			using FShaderResult = TRenderResourceCreateResult<FState::FShaderPayload>;
			bool bShaderCreated = false;
			++View.ShaderLookups;
			auto* Shader = ShaderEntry.Slot.Resolve(Coordinator.GetGeneration_RenderThread(), [this, &Draw, bShadowDepth, &bShaderCreated]() -> FShaderResult {
				bShaderCreated = true;
				const auto& Identity = Draw.Material.PipelineIdentity.ShaderMap;
				FShaderCompileOptions Options;
				Options.bForceRecompile = Coordinator.ShouldForceShaderRecompile_RenderThread();
				Options.Macros.emplace_back("DURIN_TERRAIN", "1");
				Options.Macros.emplace_back("DURIN_MATERIAL_BLEND_MODE", std::to_string(static_cast<uint8>(Identity.BlendMode)));
				Options.Macros.emplace_back("DURIN_MATERIAL_SHADING_MODEL", std::to_string(static_cast<uint8>(Identity.ShadingModel)));
				Options.Macros.emplace_back("DURIN_MATERIAL_OPACITY_MASK_THRESHOLD_BITS", std::to_string(std::bit_cast<uint32>(Identity.OpacityMaskThreshold)));
				if (bShadowDepth
					&& Identity.BlendMode != EMaterialBlendMode::Masked)
				{
					Options.Macros.emplace_back(
						"DURIN_OPAQUE_SHADOW_DEPTH", "1");
				}
				FShaderType& VertexType = FTerrainVertexShader::StaticType();
				FShaderType& FragmentType = FTerrainFragmentShader::StaticType();
				FShaderType& ShadowFragmentType =
					FTerrainShadowFragmentShader::StaticType();
				FShaderType& OpaqueShadowFragmentType =
					FTerrainOpaqueShadowFragmentShader::StaticType();
				std::vector<const FShaderType*> Types{&VertexType};
				if (!bShadowDepth)
					Types.push_back(&FragmentType);
				else if (Identity.BlendMode == EMaterialBlendMode::Masked)
					Types.push_back(&ShadowFragmentType);
				else Types.push_back(&OpaqueShadowFragmentType);
				auto Map = std::make_shared<FShaderMapBase>();
				std::string Error;
				if (!Map->InitializeFromShaderTypes(Types, Options, Error))
					return FShaderResult::Failure(MakeRendererResourceCreateError(ERenderResourceCreateErrorCategory::ShaderCompile, "TerrainShaderMap", "terrain", std::move(Error), ERenderResourceGenerationDependency::Shader | ERenderResourceGenerationDependency::Manual));
				auto* Vertex = static_cast<FTerrainVertexShader*>(Map->GetShader(&VertexType));
				auto* Fragment = !bShadowDepth
					? static_cast<FTerrainFragmentShader*>(
						Map->GetShader(&FragmentType)) : nullptr;
				auto* ShadowFragment = bShadowDepth
					&& Identity.BlendMode == EMaterialBlendMode::Masked
					? static_cast<FTerrainShadowFragmentShader*>(
						Map->GetShader(&ShadowFragmentType)) : nullptr;
				auto* OpaqueShadowFragment =
					bShadowDepth && Identity.BlendMode != EMaterialBlendMode::Masked
						? static_cast<FTerrainOpaqueShadowFragmentShader*>(
							Map->GetShader(&OpaqueShadowFragmentType)) : nullptr;
				if (!Vertex || (!bShadowDepth && !Fragment)) return FShaderResult::Failure(MakeRendererResourceCreateError(ERenderResourceCreateErrorCategory::ShaderBinding, "TerrainShaderMap", "terrain", "Typed shaders are missing.", ERenderResourceGenerationDependency::Shader));
				if (bShadowDepth
					&& Identity.BlendMode == EMaterialBlendMode::Masked && !ShadowFragment)
					return FShaderResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::ShaderBinding,
						"TerrainShaderMap", "terrain",
						"Masked shadow fragment shader is missing.",
						ERenderResourceGenerationDependency::Shader));
				if (bShadowDepth && Identity.BlendMode != EMaterialBlendMode::Masked
					&& !OpaqueShadowFragment)
					return FShaderResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::ShaderBinding,
						"TerrainShaderMap", "terrain",
						"Opaque shadow fragment shader is missing.",
						ERenderResourceGenerationDependency::Shader));
				FState::FShaderPayload Candidate;
				Candidate.Map = std::move(Map);
				Candidate.Vertex = {Vertex, Candidate.Map.get()};
				if (Fragment)
					Candidate.Fragment = {Fragment, Candidate.Map.get()};
				if (ShadowFragment)
					Candidate.ShadowFragment = {ShadowFragment, Candidate.Map.get()};
				if (OpaqueShadowFragment)
					Candidate.OpaqueShadowFragment = {
						OpaqueShadowFragment, Candidate.Map.get()};
				return FShaderResult::Success(std::move(Candidate)); }, ReportRendererResourceCreateDiagnostic);
			if (!Shader) return false;
			bShaderCreated ? ++View.ShaderCreations : ++View.ShaderReuses;
			View.ShaderPreparationNanoseconds += std::chrono::duration_cast<
													 std::chrono::nanoseconds>(
													 std::chrono::steady_clock::now() - ShaderBegin
			)
													 .count();

			const auto PipelineBegin = std::chrono::steady_clock::now();
			FEffectiveStaticMeshPipelineKey EffectivePipelineKey =
				bShadowDepth ? MakeShadowPipelineKey(Draw.PipelineKey) : Draw.PipelineKey;
			EffectivePipelineKey.bHybridRetained =
				!bShadowDepth && bHybridRetained;
			auto& PipelineCache = bShadowDepth ? State->ShadowPipelines : State->Pipelines;
			auto& PipelineEntry = PipelineCache.FindOrAdd(EffectivePipelineKey);
			using FPipelineResult = TRenderResourceCreateResult<FState::FPipelinePayload>;
			FRenderResourceGeneration Generation = Coordinator.GetGeneration_RenderThread();
			Generation.Shader = ShaderEntry.Slot.GetPayloadGeneration().Shader;
			bool bPipelineCreated = false;
			++View.PipelineLookups;
			auto* Pipeline = PipelineEntry.Slot.Resolve(Generation, [&EffectivePipelineKey, &PipelineEntry, Shader, &TopologyIt, bShadowDepth, &bPipelineCreated]() -> FPipelineResult {
				bPipelineCreated = true;
				FState::FPipelinePayload Candidate;
				Candidate.Map = Shader->Map;
				Candidate.Vertex = Shader->Vertex;
				Candidate.Fragment = Shader->Fragment;
				Candidate.ShadowFragment = Shader->ShadowFragment;
				Candidate.OpaqueShadowFragment = Shader->OpaqueShadowFragment;
				FGraphicsPipelineStateInitializer Initializer;
				Initializer.RenderTargetLayout = bShadowDepth
					? RenderTargetLayouts::MakeDirectionalShadowDepth()
					: (EffectivePipelineKey.bHybridRetained
						? RenderTargetLayouts::MakeHybridRetainedForward()
						: RenderTargetLayouts::MakeSceneTargets());
				Initializer.BoundShaders.VertexShader = Candidate.Vertex.GetRHIShader();
				Initializer.BoundShaders.FragmentShader = bShadowDepth
					? (EffectivePipelineKey.Material.ShaderMap.BlendMode
							== EMaterialBlendMode::Masked
						? Candidate.ShadowFragment.GetRHIShader()
						: Candidate.OpaqueShadowFragment.GetRHIShader())
					: Candidate.Fragment.GetRHIShader();
				Initializer.VertexDeclaration = TopologyIt->second->VertexFactory.GetDeclaration();
				Initializer.RasterizerState = EffectivePipelineKey.Rasterizer;
				Initializer.DepthStencilState = EffectivePipelineKey.Depth;
				if (!bShadowDepth)
				{
					Initializer.ColorBlendStates[0] = EffectivePipelineKey.ColorBlend;
					if (EffectivePipelineKey.Material.ShaderMap.BlendMode
						== EMaterialBlendMode::Translucent)
					{
						Initializer.ColorBlendStates[1].ColorWriteMask =
							ERHIColorWriteMask::None;
					}
				}
				Initializer.PipelineLayout = Candidate.Map->GetMergedPipelineLayout();
				Candidate.Pipeline = GDynamicRHI->RHICreateGraphicsPipelineState(FName(std::format("TerrainPipeline_{}", PipelineEntry.Index)), Initializer);
				return Candidate.Pipeline ? FPipelineResult::Success(std::move(Candidate))
					: FPipelineResult::Failure(MakeRendererResourceCreateError(ERenderResourceCreateErrorCategory::GraphicsPipeline, "TerrainPipeline", "terrain", "Pipeline creation returned null.", ERenderResourceGenerationDependency::Device)); }, ReportRendererResourceCreateDiagnostic);
			if (!Pipeline) return false;
			bPipelineCreated ? ++View.PipelineCreations : ++View.PipelineReuses;
			View.PipelinePreparationNanoseconds += std::chrono::duration_cast<
													   std::chrono::nanoseconds>(
													   std::chrono::steady_clock::now() - PipelineBegin
			)
													   .count();
		}
		for (const auto& Sampler : Draw.MaterialBinding.Samplers)
		{
			const size_t Key = GetSamplerKey(Sampler);
			if (!State->Samplers.contains(Key))
			{
				auto Created = RHICreateSampler(MakeSampler(Sampler));
				if (!Created) return false;
				State->Samplers.emplace(Key, std::move(Created));
			}
		}
		return true;
	}

	auto FTerrainRenderer::PrepareResources_RenderThread(
		FRHICommandListImmediate& CommandList, FPreparedTerrainView& View, bool bPrepareLitOpaqueForward
	) -> bool
	{
		check(View.Phase == EPreparedTerrainPhase::Prepared);
		const auto Begin = std::chrono::steady_clock::now();
		View.ResourceAttemptedDraws = View.GetNumDraws();
		for (auto [Draws, Batches] : {std::pair{&View.Opaque, &View.OpaqueBatches}, std::pair{&View.Masked, &View.MaskedBatches}})
			for (auto& Batch : *Batches)
			{
				++View.ResourceAttemptedBatches;
				if (Batch.DrawIndices.empty()) continue;
				FPreparedTerrainDraw& FirstDraw =
					(*Draws)[Batch.DrawIndices.front()];
				const bool bPrepareForwardPipeline =
					bPrepareLitOpaqueForward
					|| FirstDraw.Material.PipelineIdentity.ShaderMap.ShadingModel
						   != EMaterialShadingModel::Lit;
				Batch.bResourcesReady = EnsureDrawResources_RenderThread(
					CommandList, FirstDraw, View, false, false,
					bPrepareForwardPipeline
				);
				for (uint32 DrawIndex : Batch.DrawIndices)
					(*Draws)[DrawIndex].bResourcesReady = Batch.bResourcesReady;
				if (Batch.bResourcesReady)
				{
					++View.ResourceSuccessfulBatches;
					View.ResourceSuccessfulDraws += Batch.DrawIndices.size();
				}
			}
		for (auto& Draw : View.Translucent)
		{
			Draw.bResourcesReady = EnsureDrawResources_RenderThread(CommandList, Draw, View);
			View.ResourceSuccessfulDraws += Draw.bResourcesReady ? 1u : 0u;
		}
		View.ResourceRejectedBatches =
			View.ResourceAttemptedBatches - View.ResourceSuccessfulBatches;
		View.ResourceRejectedDraws = View.ResourceAttemptedDraws - View.ResourceSuccessfulDraws;
		View.Phase = EPreparedTerrainPhase::ResourcesPrepared;
		View.ResourcePreparationNanoseconds = std::chrono::duration_cast<
												  std::chrono::nanoseconds>(std::chrono::steady_clock::now() - Begin)
												  .count();
		return std::ranges::all_of(View.Opaque, [](const auto& D) { return D.bResourcesReady; })
			   && std::ranges::all_of(View.Masked, [](const auto& D) { return D.bResourcesReady; })
			   && std::ranges::all_of(View.Translucent, [](const auto& D) { return D.bResourcesReady; });
	}

	auto FTerrainRenderer::PrepareHybridRetainedResources_RenderThread(
		FRHICommandListImmediate& CommandList,
		FPreparedTerrainView& View
	) -> bool
	{
		check(View.Phase == EPreparedTerrainPhase::ResourcesPrepared);
		bool bReady = true;
		auto PrepareBucket = [this, &CommandList, &View, &bReady](
								 auto& Bucket, bool bAllMaterials
							 ) {
			for (FPreparedTerrainDraw& Draw : Bucket)
			{
				if (!bAllMaterials
					&& Draw.Material.PipelineIdentity.ShaderMap.ShadingModel
						   == EMaterialShadingModel::Lit)
					continue;
				bReady = EnsureDrawResources_RenderThread(
							 CommandList, Draw, View, false, true
						 )
						 && bReady;
			}
		};
		PrepareBucket(View.Opaque, false);
		PrepareBucket(View.Masked, false);
		PrepareBucket(View.Translucent, true);
		return bReady;
	}

	auto FTerrainRenderer::PrepareShadowResources_RenderThread(
		FRHICommandListImmediate& CommandList,
		FPreparedTerrainView& View
	) -> bool
	{
		check(View.Phase == EPreparedTerrainPhase::Prepared);
		const auto Begin = std::chrono::steady_clock::now();
		check(View.Translucent.empty());
		View.ResourceAttemptedDraws = View.GetNumDraws();
		for (auto [Draws, Batches] : {std::pair{&View.Opaque, &View.OpaqueBatches}, std::pair{&View.Masked, &View.MaskedBatches}})
			for (auto& Batch : *Batches)
			{
				++View.ResourceAttemptedBatches;
				if (Batch.DrawIndices.empty()) continue;
				Batch.bResourcesReady = EnsureDrawResources_RenderThread(
					CommandList, (*Draws)[Batch.DrawIndices.front()], View, true
				);
				for (uint32 DrawIndex : Batch.DrawIndices)
					(*Draws)[DrawIndex].bResourcesReady = Batch.bResourcesReady;
				if (Batch.bResourcesReady)
				{
					++View.ResourceSuccessfulBatches;
					View.ResourceSuccessfulDraws += Batch.DrawIndices.size();
				}
			}
		View.ResourceRejectedBatches =
			View.ResourceAttemptedBatches - View.ResourceSuccessfulBatches;
		View.ResourceRejectedDraws =
			View.ResourceAttemptedDraws - View.ResourceSuccessfulDraws;
		View.Phase = EPreparedTerrainPhase::ResourcesPrepared;
		View.ResourcePreparationNanoseconds = std::chrono::duration_cast<
												  std::chrono::nanoseconds>(std::chrono::steady_clock::now() - Begin)
												  .count();
		return View.ResourceRejectedDraws == 0;
	}

	auto FTerrainRenderer::ExecuteShadow_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneView& ShadowView,
		const FRHIUniformBufferRange& FallbackLighting,
		FPreparedTerrainView& View
	) -> void
	{
		check(CommandList.IsInsideRenderPass());
		check(View.Phase == EPreparedTerrainPhase::ResourcesPrepared);
		for (auto [Draws, Batches] : {std::pair{&View.Opaque, &View.OpaqueBatches}, std::pair{&View.Masked, &View.MaskedBatches}})
			for (const auto& Batch : *Batches)
			{
				const auto Begin = std::chrono::steady_clock::now();
				++View.AttemptedDraws;
				++View.InstanceAllocations;
				View.InstanceBytes += Batch.DrawIndices.size()
									  * sizeof(FTerrainInstanceData);
				uint64 DynamicNanoseconds = 0;
				if (DrawBatch_RenderThread(CommandList, ShadowView, FallbackLighting, ERenderMode::Unlit, *Draws, Batch, true, &DynamicNanoseconds))
				{
					++View.SuccessfulDraws;
					View.SubmittedLogicalPatches += Batch.DrawIndices.size();
				}
				else
					++View.RejectedDraws;
				View.DynamicAllocationNanoseconds += DynamicNanoseconds;
				View.CommandRecordingNanoseconds += std::chrono::duration_cast<
														std::chrono::nanoseconds>(std::chrono::steady_clock::now() - Begin)
														.count();
			}
		View.Phase = EPreparedTerrainPhase::Executed;
		check(View.AttemptedDraws == View.SuccessfulDraws + View.RejectedDraws);
	}

	auto FTerrainRenderer::Draw_RenderThread(FRHICommandListImmediate& CommandList, const FSceneView& SceneView, const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode, const FPreparedTerrainDraw& Draw, bool bShadowDepth, std::span<const std::array<uint32, 2>> InstanceOrigins, uint64* OutDynamicAllocationNanoseconds, FGBufferRenderer* GBuffer, bool bHybridRetained) -> bool
	{
		if (!Draw.bResourcesReady || !Draw.SceneInfo || !Draw.Patch) return false;
		const FTerrainSceneProxy& Proxy = Draw.SceneInfo->GetTerrainProxy();
		const auto Payload = Proxy.GetPayload();
		auto HeightIt = State->Heights.find(Payload.get());
		const FTerrainTopologyKey Key{
			static_cast<uint16>(Draw.Patch->CellCountX),
			static_cast<uint16>(Draw.Patch->CellCountY),
			static_cast<uint16>(Draw.LODStep), Draw.StitchMask
		};
		auto TopologyIt = State->Topologies.find(Key);
		FEffectiveStaticMeshPipelineKey EffectivePipelineKey =
			bShadowDepth ? MakeShadowPipelineKey(Draw.PipelineKey) : Draw.PipelineKey;
		EffectivePipelineKey.bHybridRetained =
			!bShadowDepth && bHybridRetained;
		auto* PipelineEntry = bShadowDepth ? State->ShadowPipelines.Find(EffectivePipelineKey) : State->Pipelines.Find(EffectivePipelineKey);
		auto* Pipeline = PipelineEntry ? PipelineEntry->Slot.GetPayload() : nullptr;
		if (HeightIt == State->Heights.end()
			|| TopologyIt == State->Topologies.end()
			|| (GBuffer == nullptr && Pipeline == nullptr)) return false;
		if (Draw.TriangleCount > std::numeric_limits<uint32>::max() / 3
			|| TopologyIt->second->IndexCount != Draw.TriangleCount * 3) return false;
		const uint32 PreparedIndexCount = static_cast<uint32>(Draw.TriangleCount * 3);
		const std::array<uint32, 2> ScalarOrigin{
			Draw.Patch->OriginX, Draw.Patch->OriginY
		};
		if (InstanceOrigins.empty())
			InstanceOrigins = std::span<const std::array<uint32, 2>>(
				&ScalarOrigin, 1
			);
		if (InstanceOrigins.size() > MaximumTerrainInstancesPerChunk
			|| InstanceOrigins.size() > std::numeric_limits<uint32>::max())
			return false;
		const FMatrix& LocalToWorld = Draw.SceneInfo->GetTransform();
		const FMatrix LocalToClip = SceneView.ViewProjectionMatrix * LocalToWorld;
		std::vector<FTerrainInstanceData> Instances;
		Instances.reserve(InstanceOrigins.size());
		for (const auto& Origin : InstanceOrigins)
		{
			const FVector4 LocalAnchor(
				static_cast<double>(Origin[0]) * Proxy.GetSpacingX(),
				static_cast<double>(Origin[1]) * Proxy.GetSpacingY(), 0.0, 1.0
			);
			const FVector4 WorldAnchor = LocalToWorld * LocalAnchor;
			const FVector4 ClipAnchor = SceneView.ViewProjectionMatrix * WorldAnchor;
			const FVector3 RelativeAnchor = FVector3(WorldAnchor) - SceneView.ViewLocation;
			Instances.push_back({.SampleOrigin = Origin, .AnchorClip = FVector4f(ClipAnchor), .AnchorRelativeWorld = FVector4f(FVector3f(RelativeAnchor), 0.0f)});
		}
		if (Instances.size() * sizeof(FTerrainInstanceData)
			> std::numeric_limits<uint32>::max()) return false;
		const auto DynamicBegin = std::chrono::steady_clock::now();
		const auto InstanceRange = CommandList.AllocateDynamicStorageBuffer(
			Instances.data(), static_cast<uint32>(Instances.size() * sizeof(FTerrainInstanceData))
		);
		if (OutDynamicAllocationNanoseconds)
			*OutDynamicAllocationNanoseconds = std::chrono::duration_cast<
												   std::chrono::nanoseconds>(std::chrono::steady_clock::now() - DynamicBegin)
												   .count();
		if (!InstanceRange.Buffer || InstanceRange.Size != Instances.size() * sizeof(FTerrainInstanceData))
			return false;
		FTransformUniform Transform;
		Transform.LocalToClip = ToShaderMatrix(LocalToClip);
		Transform.LocalToWorld = ToShaderMatrix(LocalToWorld);
		Transform.NormalToWorld = ToShaderMatrix(Math::Transpose(Math::Inverse(LocalToWorld)));
		Transform.TransformParams.x = glm::determinant(glm::mat3(LocalToWorld)) < 0.0 ? -1.0f : 1.0f;
		const auto TransformBuffer = CommandList.AllocateDynamicUniformBuffer(&Transform, sizeof(Transform));
		FTerrainUniform Terrain;
		Terrain.SamplePatch = {Payload->Width, Payload->Height, 0, 0};
		Terrain.SpacingHeight = FVector4f(static_cast<float>(Proxy.GetSpacingX()), static_cast<float>(Proxy.GetSpacingY()), static_cast<float>(Proxy.GetHeightScale()), static_cast<float>(Proxy.GetHeightOffset()));
		const FTerrainDistancePolicy DistancePolicy =
			ResolveTerrainDistancePolicy(SceneView);
		Terrain.DistanceTransition = FVector4f(
			static_cast<float>(DistancePolicy.FadeStart),
			static_cast<float>(DistancePolicy.RenderDistance), 0.0f, 0.0f
		);
		const auto TerrainBuffer = CommandList.AllocateDynamicUniformBuffer(&Terrain, sizeof(Terrain));
		FMaterialUniform Material;
		const auto& Binding = Draw.MaterialBinding;
		Material.BaseColor = Binding.BaseColor;
		Material.EmissiveMetallic = FVector4f(Binding.Emissive, Binding.Metallic);
		Material.NormalRoughness = FVector4f(Binding.Normal, Binding.Roughness);
		Material.SurfaceParams = FVector4f(Binding.AmbientOcclusion, Binding.OpacityMask, RenderMode == ERenderMode::Lit && Draw.Material.PipelineIdentity.ShaderMap.ShadingModel == EMaterialShadingModel::Lit ? 1.0f : 0.0f, 0.0f);
		for (size_t Role = 0; Role < 8; ++Role)
			Material.UVTransforms[Role] = FVector4f(Binding.UVScales[Role].x, Binding.UVScales[Role].y, Binding.UVOffsets[Role].x, Binding.UVOffsets[Role].y);
		Material.UVChannels0 = FVector4f(Binding.UVChannels[0], Binding.UVChannels[1], Binding.UVChannels[2], Binding.UVChannels[3]);
		Material.UVChannels1 = FVector4f(Binding.UVChannels[4], Binding.UVChannels[5], Binding.UVChannels[6], Binding.UVChannels[7]);
		Material.UVRotations0 = FVector4f(Binding.UVRotations[0], Binding.UVRotations[1], Binding.UVRotations[2], Binding.UVRotations[3]);
		Material.UVRotations1 = FVector4f(Binding.UVRotations[4], Binding.UVRotations[5], Binding.UVRotations[6], Binding.UVRotations[7]);
		const auto MaterialBuffer = CommandList.AllocateDynamicUniformBuffer(&Material, sizeof(Material));
		if (GBuffer == nullptr)
		{
			CommandList.SetGraphicsPipelineState(*Pipeline->Pipeline);
			if (bShadowDepth)
			{
				const FRHIRasterizerState Rasterizer =
					MakeShadowRasterizerState(Draw.PipelineKey.Rasterizer);
				CommandList.SetDepthBias(Rasterizer.DepthBiasConstantFactor, Rasterizer.DepthBiasClamp, Rasterizer.DepthBiasSlopeFactor);
			}
		}
		TopologyIt->second->VertexFactory.BindStreams(CommandList);
		CommandList.BindIndexBuffer(TopologyIt->second->Indices, 0);
		if (GBuffer == nullptr)
		{
			FTerrainVertexShader::FParameters VS;
			VS.Transform = TransformBuffer;
			VS.HeightTexture = HeightIt->second.Texture;
			VS.Terrain = TerrainBuffer;
			VS.TerrainPatchOrigins = InstanceRange;
			SetShaderParameters(CommandList, Pipeline->Vertex, VS);
		}
		const FRHIDrawIndexedArguments DrawArguments{
			PreparedIndexCount, static_cast<uint32>(InstanceOrigins.size()), 0, 0, 0
		};
		if (GBuffer == nullptr && bShadowDepth
			&& Draw.PipelineKey.Material.ShaderMap.BlendMode
				   != EMaterialBlendMode::Masked)
		{
			CommandList.DrawIndexed(DrawArguments);
			return true;
		}
		FTerrainFragmentShader::FParameters PS;
		PS.Lighting = Lighting;
		PS.Material = MaterialBuffer;
		auto ResolveTexture = [&](size_t Role, EDefaultTexture Fallback) { FRHITexture* T = Binding.Textures[Role] ? Binding.Textures[Role]->GetReferencedTexture_RenderThread() : nullptr; return T ? T : DefaultTextures.Get_RenderThread(Fallback); };
		PS.BaseColorTexture = ResolveTexture(0, EDefaultTexture::White);
		PS.NormalTexture = ResolveTexture(1, EDefaultTexture::FlatNormal);
		PS.MetallicTexture = ResolveTexture(2, EDefaultTexture::White);
		PS.RoughnessTexture = ResolveTexture(3, EDefaultTexture::White);
		PS.AmbientOcclusionTexture = ResolveTexture(4, EDefaultTexture::White);
		PS.EmissiveTexture = ResolveTexture(5, EDefaultTexture::Black);
		PS.OpacityTexture = ResolveTexture(6, EDefaultTexture::White);
		PS.OpacityMaskTexture = ResolveTexture(7, EDefaultTexture::White);
		std::array<FRHISampler*, 8> Samplers{};
		for (size_t Role = 0; Role < 8; ++Role)
			Samplers[Role] = State->Samplers.at(GetSamplerKey(Binding.Samplers[Role]));
		PS.BaseColorSampler = Samplers[0];
		PS.NormalSampler = Samplers[1];
		PS.MetallicSampler = Samplers[2];
		PS.RoughnessSampler = Samplers[3];
		PS.AmbientOcclusionSampler = Samplers[4];
		PS.EmissiveSampler = Samplers[5];
		PS.OpacitySampler = Samplers[6];
		PS.OpacityMaskSampler = Samplers[7];
		if (GBuffer != nullptr)
		{
			const FVertexDeclarationRHIRef VertexDeclaration(
				TopologyIt->second->VertexFactory.GetDeclaration()
			);
			FGBufferRenderer::FPipeline* GBufferPipeline =
				GBuffer->EnsurePipeline_RenderThread({.Material = Draw.PipelineKey.Material, .Rasterizer = Draw.PipelineKey.Rasterizer, .Depth = Draw.PipelineKey.Depth, .VertexDeclaration = VertexDeclaration, .VertexDomain = EGBufferVertexDomain::Terrain});
			if (GBufferPipeline == nullptr) return false;
			const FGBufferRenderer::FVertexParameters VertexParameters{
				.Transform = TransformBuffer,
				.HeightTexture = HeightIt->second.Texture,
				.Terrain = TerrainBuffer,
				.TerrainPatchOrigins = InstanceRange
			};
			FGBufferRenderer::FFragmentParameters FragmentParameters;
			FragmentParameters.Material = MaterialBuffer;
			FragmentParameters.Textures = {
				PS.BaseColorTexture,
				PS.NormalTexture,
				PS.MetallicTexture,
				PS.RoughnessTexture,
				PS.AmbientOcclusionTexture,
				PS.EmissiveTexture,
				PS.OpacityTexture,
				PS.OpacityMaskTexture
			};
			FragmentParameters.Samplers = Samplers;
			if (!GBuffer->BindPipeline_RenderThread(CommandList, *GBufferPipeline, VertexParameters, FragmentParameters))
			{
				return false;
			}
			CommandList.DrawIndexed(DrawArguments);
			return true;
		}
		if (bShadowDepth
			&& Draw.PipelineKey.Material.ShaderMap.BlendMode
				   == EMaterialBlendMode::Masked)
		{
			FTerrainShadowFragmentShader::FParameters ShadowParameters;
			ShadowParameters.Material = MaterialBuffer;
			ShadowParameters.OpacityMaskTexture = PS.OpacityMaskTexture;
			ShadowParameters.OpacityMaskSampler = Samplers[7];
			SetShaderParameters(CommandList, Pipeline->ShadowFragment, ShadowParameters);
			CommandList.DrawIndexed(DrawArguments);
			return true;
		}
		FRHITexture* Irradiance = EnvironmentLighting.GetIrradiance_RenderThread();
		FRHITexture* Prefiltered = EnvironmentLighting.GetPrefiltered_RenderThread();
		FRHITexture* Brdf = EnvironmentLighting.GetBrdfLut_RenderThread();
		FRHISampler* EnvironmentSampler = EnvironmentLighting.GetSampler_RenderThread();
		const bool Complete = Irradiance && Prefiltered && Brdf && EnvironmentSampler;
		PS.EnvironmentIrradiance = Complete ? Irradiance : DefaultTextures.GetCube_RenderThread();
		PS.EnvironmentPrefiltered = Complete ? Prefiltered : DefaultTextures.GetCube_RenderThread();
		PS.EnvironmentBrdfLut = Complete ? Brdf : DefaultTextures.Get_RenderThread(EDefaultTexture::Black);
		PS.EnvironmentSampler = Complete ? EnvironmentSampler : Samplers[0];
		PS.DirectionalShadowTexture = Draw.DirectionalShadowTexture != nullptr ? Draw.DirectionalShadowTexture : DefaultTextures.GetArray_RenderThread();
		PS.DirectionalShadowSampler = Draw.DirectionalShadowSampler != nullptr ? Draw.DirectionalShadowSampler : Samplers[0];
		SetShaderParameters(CommandList, Pipeline->Fragment, PS);
		CommandList.DrawIndexed(DrawArguments);
		return true;
	}

	auto FTerrainRenderer::DrawBatch_RenderThread(
		FRHICommandListImmediate& CommandList, const FSceneView& SceneView, const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode, const std::vector<FPreparedTerrainDraw>& Draws, const FPreparedTerrainBatch& Batch, bool bShadowDepth, uint64* OutDynamicAllocationNanoseconds, FGBufferRenderer* GBuffer
	) -> bool
	{
		if (!Batch.bResourcesReady || Batch.DrawIndices.empty()
			|| Batch.DrawIndices.size() > MaximumTerrainInstancesPerChunk)
			return false;
		const FPreparedTerrainDraw& First = Draws[Batch.DrawIndices.front()];
		std::vector<std::array<uint32, 2>> Origins;
		Origins.reserve(Batch.DrawIndices.size());
		for (uint32 DrawIndex : Batch.DrawIndices)
		{
			if (DrawIndex >= Draws.size()
				|| !AreTerrainDrawsBatchCompatible(First, Draws[DrawIndex])) return false;
			Origins.push_back({Draws[DrawIndex].Patch->OriginX, Draws[DrawIndex].Patch->OriginY});
		}
		return Draw_RenderThread(CommandList, SceneView, Lighting, RenderMode, First, bShadowDepth, Origins, OutDynamicAllocationNanoseconds, GBuffer);
	}

	auto FTerrainRenderer::ExecutePreparedDraw_RenderThread(
		FRHICommandListImmediate& CommandList, const FSceneView& SceneView, const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode, const FPreparedTerrainDraw& Draw, FPreparedTerrainView& View, bool bHybridRetained
	) -> void
	{
		const auto Begin = std::chrono::steady_clock::now();
		++View.AttemptedDraws;
		++View.ScalarTranslucentDraws;
		++View.InstanceAllocations;
		++View.InstanceCount;
		View.InstanceBytes += sizeof(FTerrainInstanceData);
		uint64 DynamicNanoseconds = 0;
		if (Draw_RenderThread(CommandList, SceneView, Lighting, RenderMode, Draw, false, {}, &DynamicNanoseconds, nullptr, bHybridRetained))
		{
			++View.SuccessfulDraws;
			++View.SubmittedLogicalPatches;
		}
		else
			++View.RejectedDraws;
		View.DynamicAllocationNanoseconds += DynamicNanoseconds;
		View.CommandRecordingNanoseconds += std::chrono::duration_cast<
												std::chrono::nanoseconds>(std::chrono::steady_clock::now() - Begin)
												.count();
	}

	auto FTerrainRenderer::ExecutePass_RenderThread(
		FRHICommandListImmediate& CommandList, const FSceneView& SceneView, const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode, EStaticMeshBasePass Pass, FPreparedTerrainView& View
	) -> void
	{
		const auto& Draws = Pass == EStaticMeshBasePass::Opaque ? View.Opaque : View.Masked;
		const auto& Batches = Pass == EStaticMeshBasePass::Opaque ? View.OpaqueBatches : View.MaskedBatches;
		for (const auto& Batch : Batches)
		{
			const auto Begin = std::chrono::steady_clock::now();
			++View.AttemptedDraws;
			++View.InstanceAllocations;
			View.InstanceBytes += Batch.DrawIndices.size() * sizeof(FTerrainInstanceData);
			uint64 DynamicNanoseconds = 0;
			if (DrawBatch_RenderThread(CommandList, SceneView, Lighting, RenderMode, Draws, Batch, false, &DynamicNanoseconds))
			{
				++View.SuccessfulDraws;
				View.SubmittedLogicalPatches += Batch.DrawIndices.size();
			}
			else
				++View.RejectedDraws;
			View.DynamicAllocationNanoseconds += DynamicNanoseconds;
			View.CommandRecordingNanoseconds += std::chrono::duration_cast<
													std::chrono::nanoseconds>(std::chrono::steady_clock::now() - Begin)
													.count();
		}
	}

	auto FTerrainRenderer::ExecuteGBuffer_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneView& SceneView,
		FGBufferRenderer& GBuffer,
		FPreparedTerrainView& View
	) -> void
	{
		check(CommandList.IsInsideRenderPass());
		check(View.Phase == EPreparedTerrainPhase::ResourcesPrepared);
		View.GBufferSkippedDraws += View.Translucent.size();
		for (auto [Draws, Batches] : {
				 std::pair{&View.Opaque, &View.OpaqueBatches},
				 std::pair{&View.Masked, &View.MaskedBatches}
			 })
		{
			for (const FPreparedTerrainBatch& Batch : *Batches)
			{
				if (Batch.DrawIndices.empty()
					|| Batch.DrawIndices.front() >= Draws->size())
				{
					++View.GBufferAttemptedDraws;
					++View.GBufferRejectedDraws;
					continue;
				}
				const FPreparedTerrainDraw& Draw =
					(*Draws)[Batch.DrawIndices.front()];
				if (Draw.Material.PipelineIdentity.ShaderMap.ShadingModel
					!= EMaterialShadingModel::Lit)
				{
					++View.GBufferSkippedDraws;
					continue;
				}
				++View.GBufferAttemptedDraws;
				if (DrawBatch_RenderThread(
						CommandList,
						SceneView,
						{},
						ERenderMode::Lit,
						*Draws,
						Batch,
						false,
						nullptr,
						&GBuffer
					))
				{
					++View.GBufferSuccessfulDraws;
				}
				else
				{
					++View.GBufferRejectedDraws;
				}
			}
		}
		check(View.GBufferAttemptedDraws == View.GBufferSuccessfulDraws + View.GBufferRejectedDraws);
	}

	auto FTerrainRenderer::FinalizeExecution_RenderThread(FPreparedTerrainView& View) -> void
	{
		View.Phase = EPreparedTerrainPhase::Executed;
		check(View.AttemptedDraws == View.SuccessfulDraws + View.RejectedDraws);
		check(View.AttemptedDraws == View.GetNumHardwareDraws());
	}

	auto FTerrainRenderer::ReleaseResources_RenderThread() -> void
	{
		for (auto& [Key, Topology] : State->Topologies)
			if (Topology->VertexFactory.IsInitialized()) Topology->VertexFactory.ReleaseResource();
		State->Topologies.clear();
		State->Heights.clear();
		State->Samplers.clear();
		State->Shaders.Reset();
		State->ShadowShaders.Reset();
		State->Pipelines.Reset();
		State->ShadowPipelines.Reset();
	}
} // namespace Durin
