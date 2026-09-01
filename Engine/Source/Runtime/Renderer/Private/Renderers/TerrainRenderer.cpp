#include "Renderers/TerrainRenderer.h"
#include "Renderers/MeshRenderingCommon.h"
#include "Renderers/MeshRendererShared.h"
#include "Renderers/TerrainRenderPreparation.h"
#include "Renderers/SurfaceMaterial.h"

#include "Rendering/TerrainSceneProxy.h"
#include "Math/Operations.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RendererResourceSlotCache.h"
#include "Renderers/MaterialBindingResolution.h"
#include "Renderers/RendererResourceDiagnostics.h"
#include "Renderers/GBufferRenderer.h"
#include "Renderers/ViewPreparationMath.h"
#include "Renderers/DirectionalShadowView.h"
#include "SceneViewProjection.h"
#include "RenderingThread.h"
#include "SceneInfo.h"
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

namespace Durin
{
	namespace
	{
		using namespace RendererPrivate;
		using RendererPrivate::MakeShadowPipelineKey;
		using RendererPrivate::MakeShadowRasterizerState;

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
		class FTerrainVertexShader final : public FMeshMaterialShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FTerrainVertexShader)
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Transform);
				DURIN_SHADER_PARAMETER_TEXTURE(HeightTexture);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Terrain);
				DURIN_SHADER_PARAMETER_STORAGE_BUFFER(TerrainPatchOrigins);
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_MESH_MATERIAL_SHADER(FTerrainVertexShader, FMeshMaterialShader, "/Engine/StaticMeshBasePass", EShaderFrequency::Vertex, "VertexMain");
		};

		DURIN_IMPLEMENT_MESH_MATERIAL_SHADER(FTerrainVertexShader);

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

		auto AreTerrainDrawsBatchCompatible(
			const FPreparedTerrainDraw& A, const FPreparedTerrainDraw& B
		) -> bool
		{
			return A.SceneInfo == B.SceneInfo && A.Pass == B.Pass
				   && A.PipelineKey == B.PipelineKey
				   && A.Patch && B.Patch
				   && A.Patch->CellCountX == B.Patch->CellCountX
				   && A.Patch->CellCountY == B.Patch->CellCountY
				   && A.LODStep == B.LODStep && A.StitchMask == B.StitchMask;
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
		ERasterMode RasterMode,
		ERenderPreparationMode Mode
	) -> FPreparedTerrainView
	{
		check(IsInRenderingThread());
		FPreparedTerrainView Result;
		const auto LogicalBegin = std::chrono::steady_clock::now();
		FViewFrustum Frustum;
		const bool bCull = View.Settings.Mode.VisibilityMode == EViewVisibilityMode::Normal;
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
			const double Determinant = Math::LinearDeterminant(Transform);
			if (!std::isfinite(Determinant)) continue;
			++Result.SharedPrimitiveFactBuilds;
			FPreparedTerrainDraw CommonDraw;
			CommonDraw.SceneInfo = Info;
			CommonDraw.Material = Proxy.ResolveMaterialRenderData_RenderThread();
			FMaterialRenderBinding LogicalBinding;
			if (!RendererPrivate::ResolveMaterialBinding(
					CommonDraw.Material, LogicalBinding,
					"TerrainMaterialSelection"))
				continue;
			CommonDraw.PipelineKey.Material = CommonDraw.Material.PlanningPassIdentity;
			CommonDraw.PipelineKey.Rasterizer.PolygonMode =
				RasterMode == ERasterMode::Wireframe ? ERHIPolygonMode::Line : ERHIPolygonMode::Fill;
			CommonDraw.PipelineKey.Rasterizer.CullMode =
				CommonDraw.Material.PlanningPassIdentity.bTwoSided ? ERHICullMode::None : ERHICullMode::Back;
			CommonDraw.PipelineKey.Rasterizer.FrontFace = Determinant < 0.0 ? ERHIFrontFace::CounterClockwise : ERHIFrontFace::Clockwise;
			CommonDraw.PipelineKey.Depth.bEnableTest = true;
			CommonDraw.PipelineKey.Depth.CompareOp =
				View.DepthConvention == ESceneDepthConvention::ReversedZ ? ERHIDepthCompareOp::GreaterOrEqual : ERHIDepthCompareOp::Less;
			const auto CommonBlend =
				CommonDraw.Material.PlanningPassIdentity.ShaderMap.BlendMode;
			CommonDraw.Pass = CommonBlend == EMaterialBlendMode::Masked ? EMeshBasePass::Masked : CommonBlend == EMaterialBlendMode::Translucent ? EMeshBasePass::Translucent :
																								 EMeshBasePass::Opaque;
			if (Mode == ERenderPreparationMode::ShadowDepth
				&& CommonDraw.Pass == EMeshBasePass::Translucent)
				continue;
			const auto CommonDepth =
				CommonDraw.Material.PlanningPassIdentity.DepthWritePolicy;
			CommonDraw.PipelineKey.Depth.bEnableWrite =
				CommonDepth == EMaterialDepthWritePolicy::Enabled
				|| (CommonDepth == EMaterialDepthWritePolicy::Automatic
					&& CommonDraw.Pass != EMeshBasePass::Translucent);
			if (CommonDraw.Pass == EMeshBasePass::Translucent)
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
				++Result.SharedPatchFactBuilds;
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
					++Result.PatchClassificationTests;
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
				Draw.TranslucentDistanceSquared = Math::Dot(Offset, Offset);
				Draw.SortKey.Pipeline[0] = static_cast<uint32>(Draw.Pass);
				Draw.SortKey.Pipeline[1] = Draw.Material.PlanningPassIdentity.ShaderMap.RenderLayout.Version;
				Draw.SortKey.Geometry[0] = Patch.CellCountX;
				Draw.SortKey.Geometry[1] = Patch.CellCountY;
				Draw.SortKey.Geometry[2] = Draw.LODStep;
				Draw.SortKey.Geometry[3] = Draw.StitchMask;
				Draw.SortKey.PrimitiveId = Info->GetId().Value;
				Draw.SortKey.SectionIndex = static_cast<uint32>(Result.VisiblePatches);
				const auto Bytes = Draw.Material.Representation.GetUniformPayload();
				for (std::byte Byte : Bytes)
					Draw.SortKey.MaterialUniform.push_back(Byte);
				++Result.VisiblePatches;
				Result.Triangles += Draw.TriangleCount;
				++Result.StitchMaskHistogram[Draw.StitchMask];
				auto& Bucket = Draw.Pass == EMeshBasePass::Opaque ? Result.Opaque : Draw.Pass == EMeshBasePass::Masked ? Result.Masked :
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
		BuildTerrainBatches(Result.Opaque, Result.OpaqueBatches, View.Settings.Terrain.bDisableBatching);
		BuildTerrainBatches(Result.Masked, Result.MaskedBatches, View.Settings.Terrain.bDisableBatching);
		AssignResolvedIndices(Result.Opaque, Result.Masked, Result.Translucent);
		AssignResolvedIndices(Result.OpaqueBatches, Result.MaskedBatches);
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
			FMaterialShaderMap Map;
			TMaterialShaderRef<FTerrainVertexShader> Vertex;
			TMaterialShaderRef<FSurfaceFragmentShader> Fragment;
			TMaterialShaderRef<FSurfaceOpaqueShadowFragmentShader> OpaqueShadowFragment;
			TMaterialShaderRef<FSurfaceMaskedShadowFragmentShader> ShadowFragment;
		};
		struct FPipelinePayload
		{
			FMaterialShaderMap Map;
			TMaterialShaderRef<FTerrainVertexShader> Vertex;
			TMaterialShaderRef<FSurfaceFragmentShader> Fragment;
			TMaterialShaderRef<FSurfaceOpaqueShadowFragmentShader> OpaqueShadowFragment;
			TMaterialShaderRef<FSurfaceMaskedShadowFragmentShader> ShadowFragment;
			FGraphicsPipelineStateRHIRef Pipeline;
		};
		std::unordered_map<FTerrainTopologyKey, std::unique_ptr<FTopology>> Topologies;
		std::unordered_map<const FTerrainHeightmapPayload*, FHeight> Heights;
		TRendererResourceSlotCache<FMaterialShaderMapIdentity, FShaderPayload> Shaders{
			ERenderResourceGenerationDependency::Shader
		};
		TRendererResourceSlotCache<FMaterialShaderMapIdentity, FShaderPayload>
			ShadowShaders{ERenderResourceGenerationDependency::Shader};
		TRendererResourceSlotCache<FEffectiveMeshPipelineKey, FPipelinePayload> Pipelines{
			ERenderResourceGenerationDependency::Shader | ERenderResourceGenerationDependency::Device
		};
		TRendererResourceSlotCache<FEffectiveMeshPipelineKey, FPipelinePayload> ShadowPipelines{
			ERenderResourceGenerationDependency::Shader | ERenderResourceGenerationDependency::Device
		};
	};

	FTerrainRenderer::FTerrainRenderer(FRendererResourceCoordinator& InCoordinator, RendererPrivate::FSurfaceMaterialResources& InSurfaceMaterials)
		: Coordinator(InCoordinator)
		, SurfaceMaterials(InSurfaceMaterials)
		, State(std::make_unique<FState>())
	{
	}

	FTerrainRenderer::~FTerrainRenderer() = default;

	auto FTerrainRenderer::EnsureDrawResources_RenderThread(
		FRHICommandListImmediate& CommandList, const FPreparedTerrainDraw& Draw,
		FResolvedTerrainView& ResolvedView, bool bShadowDepth,
		bool bHybridRetained, bool bPrepareForwardPipeline
	) -> bool
	{
		const FMaterialRenderBinding* MaterialBinding =
			ResolvedView.GetMaterialBinding(Draw);
		if (MaterialBinding == nullptr) return false;
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
			GDynamicRHI->RHIUpdateTexture2D(CommandList, Candidate.Texture, 0, 0,
				FUpdateTextureRegion2D(0, 0, 0, 0, Payload->Width, Payload->Height),
				Payload->Width * sizeof(uint16), std::as_bytes(std::span(Payload->Samples)));
			ResolvedView.Observations.HeightUploadBytes += Payload->GetSampleBytes();
			++ResolvedView.Observations.HeightUploads;
			HeightIt = State->Heights.emplace(Payload.get(), std::move(Candidate)).first;
		}
		else
			++ResolvedView.Observations.HeightReuses;
		ResolvedView.Observations.HeightPreparationNanoseconds += std::chrono::duration_cast<
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
			ResolvedView.Observations.TopologyBytes += TopologyData.Vertices.size() * sizeof(TopologyData.Vertices[0])
								  + TopologyData.Indices.size() * sizeof(uint16);
			++ResolvedView.Observations.TopologyCreations;
			TopologyIt = State->Topologies.emplace(TopologyKey, std::move(Candidate)).first;
		}
		else
			++ResolvedView.Observations.TopologyReuses;
		ResolvedView.Observations.TopologyPreparationNanoseconds += std::chrono::duration_cast<
												   std::chrono::nanoseconds>(
												   std::chrono::steady_clock::now() - TopologyBegin
		)
												   .count();

		if (bPrepareForwardPipeline)
		{
			const auto ShaderBegin = std::chrono::steady_clock::now();
			auto& ShaderCache = bShadowDepth ? State->ShadowShaders : State->Shaders;
			auto& ShaderEntry = ShaderCache.FindOrAddBounded(
				Draw.Material.PlanningPassIdentity.ShaderMap,
				MaterialShaderMapCacheEntryBudget
			);
			using FShaderResult = TRenderResourceCreateResult<FState::FShaderPayload>;
			bool bShaderCreated = false;
			++ResolvedView.Observations.ShaderLookups;
			auto* Shader = ShaderEntry.Slot.Resolve(Coordinator.GetGeneration_RenderThread(), [this, &Draw, bShadowDepth, &bShaderCreated]() -> FShaderResult {
				bShaderCreated = true;
				const auto& Identity = Draw.Material.PlanningPassIdentity.ShaderMap;
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
				FShaderType& FragmentType = FSurfaceFragmentShader::StaticType();
				FShaderType& ShadowFragmentType =
					FSurfaceMaskedShadowFragmentShader::StaticType();
				FShaderType& OpaqueShadowFragmentType =
					FSurfaceOpaqueShadowFragmentShader::StaticType();
				FMaterialShaderMap Map;
				std::string Error;
				const bool bOpaqueShadow = bShadowDepth
					&& Identity.BlendMode != EMaterialBlendMode::Masked;
				const FShaderType& SelectedFragmentType = bOpaqueShadow
					? OpaqueShadowFragmentType
					: bShadowDepth ? ShadowFragmentType : FragmentType;
				const bool bInitialized = RendererPrivate::InitializeMaterialShaderMap(
					VertexType, SelectedFragmentType,
					GetTerrainVertexFactoryShaderType(),
					bShadowDepth ? MaterialMeshPassShadow : MaterialMeshPassForward,
					Identity,
					Coordinator.GetGeneration_RenderThread(), bOpaqueShadow
						? nullptr : Draw.Material.CompiledProgram.get(),
					Options, Map, Error);
				if (!bInitialized)
					return FShaderResult::Failure(MakeRendererResourceCreateError(ERenderResourceCreateErrorCategory::ShaderCompile, "TerrainShaderMap", "terrain", std::move(Error), ERenderResourceGenerationDependency::Shader | ERenderResourceGenerationDependency::Manual));
				FState::FShaderPayload Candidate;
				Candidate.Map = std::move(Map);
				Candidate.Vertex =
					TMaterialShaderRef<FTerrainVertexShader>(Candidate.Map);
				if (!bShadowDepth)
					Candidate.Fragment =
						TMaterialShaderRef<FSurfaceFragmentShader>(Candidate.Map);
				if (bShadowDepth && Identity.BlendMode == EMaterialBlendMode::Masked)
					Candidate.ShadowFragment =
						TMaterialShaderRef<FSurfaceMaskedShadowFragmentShader>(Candidate.Map);
				if (bShadowDepth && Identity.BlendMode != EMaterialBlendMode::Masked)
					Candidate.OpaqueShadowFragment =
						TMaterialShaderRef<FSurfaceOpaqueShadowFragmentShader>(Candidate.Map);
				return FShaderResult::Success(std::move(Candidate)); }, ReportRendererResourceCreateDiagnostic);
			if (!Shader) return false;
			bShaderCreated ? ++ResolvedView.Observations.ShaderCreations : ++ResolvedView.Observations.ShaderReuses;
			ResolvedView.Observations.ShaderPreparationNanoseconds += std::chrono::duration_cast<
													 std::chrono::nanoseconds>(
													 std::chrono::steady_clock::now() - ShaderBegin
			)
													 .count();

			const auto PipelineBegin = std::chrono::steady_clock::now();
			FEffectiveMeshPipelineKey EffectivePipelineKey =
				bShadowDepth ? MakeShadowPipelineKey(Draw.PipelineKey) : Draw.PipelineKey;
			EffectivePipelineKey.bHybridRetained =
				!bShadowDepth && bHybridRetained;
			auto& PipelineCache = bShadowDepth ? State->ShadowPipelines : State->Pipelines;
			auto& PipelineEntry = PipelineCache.FindOrAddBounded(
				EffectivePipelineKey, MaterialPipelineCacheEntryBudget);
			using FPipelineResult = TRenderResourceCreateResult<FState::FPipelinePayload>;
			FRenderResourceGeneration Generation = Coordinator.GetGeneration_RenderThread();
			Generation.Shader = Shader->Map.GetGeneration().Shader;
			bool bPipelineCreated = false;
			++ResolvedView.Observations.PipelineLookups;
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
						? (EffectivePipelineKey.Material.ShaderMap.BlendMode
								== EMaterialBlendMode::Translucent
							? RenderTargetLayouts::MakeHybridSortedTranslucency()
							: RenderTargetLayouts::MakeHybridRetainedForward())
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
				Initializer.PipelineLayout = Candidate.Map.GetPipelineLayout();
				Candidate.Pipeline = GDynamicRHI->RHICreateGraphicsPipelineState(FName(std::format("TerrainPipeline_{}", PipelineEntry.Index)), Initializer);
				return Candidate.Pipeline ? FPipelineResult::Success(std::move(Candidate))
					: FPipelineResult::Failure(MakeRendererResourceCreateError(ERenderResourceCreateErrorCategory::GraphicsPipeline, "TerrainPipeline", "terrain", "Pipeline creation returned null.", ERenderResourceGenerationDependency::Device)); }, ReportRendererResourceCreateDiagnostic);
			if (!Pipeline) return false;
			bPipelineCreated ? ++ResolvedView.Observations.PipelineCreations : ++ResolvedView.Observations.PipelineReuses;
			ResolvedView.Observations.PipelinePreparationNanoseconds += std::chrono::duration_cast<
													   std::chrono::nanoseconds>(
													   std::chrono::steady_clock::now() - PipelineBegin
			)
													   .count();
		}
		const ESurfaceMaterialPass SurfacePass = bShadowDepth
			? (Draw.Material.PlanningPassIdentity.ShaderMap.BlendMode
					== EMaterialBlendMode::Masked
				? ESurfaceMaterialPass::MaskedShadow
				: ESurfaceMaterialPass::OpaqueShadow)
			: ESurfaceMaterialPass::Forward;
		return SurfaceMaterials.Ensure_RenderThread(
			*MaterialBinding, SurfacePass);
	}

	auto FTerrainRenderer::PrepareResources_RenderThread(
		FRHICommandListImmediate& CommandList, const FPreparedTerrainView& View,
		FResolvedTerrainView& ResolvedView, bool bPrepareLitOpaqueForward
	) -> FGeometryResolutionResult
	{
		const auto Begin = std::chrono::steady_clock::now();
		ResolvedView.Draws.resize(View.GetNumDraws());
		ResolvedView.ReadyBatches.resize(View.PreparedBatches);
		ResolvedView.Observations.PreparedBatches = View.PreparedBatches;
		ResolvedView.Observations.BatchChunks = View.BatchChunks;
		ResolvedView.Observations.InstanceCount = View.InstanceCount;
		ResolvedView.Observations.ResourceAttemptedDraws = View.GetNumDraws();
		for (auto [Draws, Batches] : {std::pair{&View.Opaque, &View.OpaqueBatches}, std::pair{&View.Masked, &View.MaskedBatches}})
			for (const auto& Batch : *Batches)
			{
				++ResolvedView.Observations.ResourceAttemptedBatches;
				if (Batch.DrawIndices.empty()) continue;
				const FPreparedTerrainDraw& FirstDraw =
					(*Draws)[Batch.DrawIndices.front()];
				FMaterialRenderBinding MaterialBinding;
				if (!RendererPrivate::ResolvePreparedMaterialBinding(
						FirstDraw.Material, MaterialBinding,
						"TerrainMaterialBinding"))
					continue;
				for (uint32 DrawIndex : Batch.DrawIndices)
				{
					ResolvedView.Draws[(*Draws)[DrawIndex].ResolvedIndex]
						.MaterialBinding = MaterialBinding;
				}
				const bool bPrepareForwardPipeline =
					bPrepareLitOpaqueForward
					|| FirstDraw.Material.PlanningPassIdentity.ShaderMap.ShadingModel
						   != EMaterialShadingModel::Lit;
				const bool bReady = EnsureDrawResources_RenderThread(
					CommandList, FirstDraw, ResolvedView, false, false,
					bPrepareForwardPipeline
				);
				if (bReady)
				{
					ResolvedView.ReadyBatches[Batch.ResolvedIndex] = true;
					for (uint32 DrawIndex : Batch.DrawIndices)
						ResolvedView.Draws[(*Draws)[DrawIndex].ResolvedIndex]
							.bReady = true;
					++ResolvedView.Observations.ResourceSuccessfulBatches;
					ResolvedView.Observations.ResourceSuccessfulDraws += Batch.DrawIndices.size();
				}
			}
		for (const auto& Draw : View.Translucent)
		{
			FMaterialRenderBinding MaterialBinding;
			if (!RendererPrivate::ResolvePreparedMaterialBinding(
					Draw.Material, MaterialBinding,
					"TerrainMaterialBinding"))
				continue;
			ResolvedView.Draws[Draw.ResolvedIndex].MaterialBinding =
				std::move(MaterialBinding);
			const bool bReady = EnsureDrawResources_RenderThread(
				CommandList, Draw, ResolvedView);
			ResolvedView.Draws[Draw.ResolvedIndex].bReady = bReady;
			ResolvedView.Observations.ResourceSuccessfulDraws += bReady ? 1u : 0u;
		}
		ResolvedView.Observations.ResourceRejectedBatches =
			ResolvedView.Observations.ResourceAttemptedBatches - ResolvedView.Observations.ResourceSuccessfulBatches;
		ResolvedView.Observations.ResourceRejectedDraws = ResolvedView.Observations.ResourceAttemptedDraws - ResolvedView.Observations.ResourceSuccessfulDraws;
		ResolvedView.Observations.ResourcePreparationNanoseconds = std::chrono::duration_cast<
												  std::chrono::nanoseconds>(std::chrono::steady_clock::now() - Begin)
												  .count();
		return {
			.Status = ResolvedView.Observations.ResourceRejectedDraws == 0
				? EGeometryResolutionStatus::Complete
				: EGeometryResolutionStatus::Partial,
			.AttemptedDraws = ResolvedView.Observations.ResourceAttemptedDraws,
			.ResolvedDraws = ResolvedView.Observations.ResourceSuccessfulDraws,
			.RejectedDraws = ResolvedView.Observations.ResourceRejectedDraws
		};
	}

	auto FTerrainRenderer::PrepareHybridRetainedResources_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FPreparedTerrainView& View,
		FResolvedTerrainView& ResolvedView
	) -> bool
	{
		bool bReady = true;
		auto PrepareBucket = [this, &CommandList, &ResolvedView, &bReady](
								 const auto& Bucket, bool bAllMaterials
							 ) {
			for (const FPreparedTerrainDraw& Draw : Bucket)
			{
				if (!bAllMaterials
					&& Draw.Material.PlanningPassIdentity.ShaderMap.ShadingModel
						   == EMaterialShadingModel::Lit)
					continue;
				bReady = EnsureDrawResources_RenderThread(
							 CommandList, Draw, ResolvedView, false, true
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
		const FPreparedTerrainView& View,
		FResolvedTerrainView& ResolvedView
	) -> FGeometryResolutionResult
	{
		const auto Begin = std::chrono::steady_clock::now();
		check(View.Translucent.empty());
		ResolvedView.Draws.resize(View.GetNumDraws());
		ResolvedView.ReadyBatches.resize(View.PreparedBatches);
		ResolvedView.Observations.PreparedBatches = View.PreparedBatches;
		ResolvedView.Observations.BatchChunks = View.BatchChunks;
		ResolvedView.Observations.InstanceCount = View.InstanceCount;
		ResolvedView.Observations.ResourceAttemptedDraws = View.GetNumDraws();
		for (auto [Draws, Batches] : {std::pair{&View.Opaque, &View.OpaqueBatches}, std::pair{&View.Masked, &View.MaskedBatches}})
			for (const auto& Batch : *Batches)
			{
				++ResolvedView.Observations.ResourceAttemptedBatches;
				if (Batch.DrawIndices.empty()) continue;
				const FPreparedTerrainDraw& FirstDraw =
					(*Draws)[Batch.DrawIndices.front()];
				FMaterialRenderBinding MaterialBinding;
				if (!RendererPrivate::ResolvePreparedMaterialBinding(
						FirstDraw.Material, MaterialBinding,
						"TerrainShadowMaterialBinding"))
					continue;
				for (uint32 DrawIndex : Batch.DrawIndices)
				{
					ResolvedView.Draws[(*Draws)[DrawIndex].ResolvedIndex]
						.MaterialBinding = MaterialBinding;
				}
				const bool bReady = EnsureDrawResources_RenderThread(
					CommandList, FirstDraw, ResolvedView, true
				);
				if (bReady)
				{
					ResolvedView.ReadyBatches[Batch.ResolvedIndex] = true;
					for (uint32 DrawIndex : Batch.DrawIndices)
						ResolvedView.Draws[(*Draws)[DrawIndex].ResolvedIndex]
							.bReady = true;
					++ResolvedView.Observations.ResourceSuccessfulBatches;
					ResolvedView.Observations.ResourceSuccessfulDraws += Batch.DrawIndices.size();
				}
			}
		ResolvedView.Observations.ResourceRejectedBatches =
			ResolvedView.Observations.ResourceAttemptedBatches - ResolvedView.Observations.ResourceSuccessfulBatches;
		ResolvedView.Observations.ResourceRejectedDraws =
			ResolvedView.Observations.ResourceAttemptedDraws - ResolvedView.Observations.ResourceSuccessfulDraws;
		ResolvedView.Observations.ResourcePreparationNanoseconds = std::chrono::duration_cast<
												  std::chrono::nanoseconds>(std::chrono::steady_clock::now() - Begin)
												  .count();
		return {
			.Status = ResolvedView.Observations.ResourceRejectedDraws == 0
				? EGeometryResolutionStatus::Complete
				: EGeometryResolutionStatus::Partial,
			.AttemptedDraws = ResolvedView.Observations.ResourceAttemptedDraws,
			.ResolvedDraws = ResolvedView.Observations.ResourceSuccessfulDraws,
			.RejectedDraws = ResolvedView.Observations.ResourceRejectedDraws
		};
	}

	auto FTerrainRenderer::ExecuteShadow_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneView& ShadowView,
		const FRHIUniformBufferRange& FallbackLighting,
		const FPreparedTerrainView& View,
		FResolvedTerrainView& ResolvedView
	) -> bool
	{
		check(CommandList.IsInsideRenderPass());
		bool bComplete = true;
		for (auto [Draws, Batches] : {std::pair{&View.Opaque, &View.OpaqueBatches}, std::pair{&View.Masked, &View.MaskedBatches}})
			for (const auto& Batch : *Batches)
			{
				const auto Begin = std::chrono::steady_clock::now();
				++ResolvedView.Observations.AttemptedDraws;
				++ResolvedView.Observations.InstanceAllocations;
				ResolvedView.Observations.InstanceBytes += Batch.DrawIndices.size()
									  * sizeof(FTerrainInstanceData);
				uint64 DynamicNanoseconds = 0;
				if (DrawBatch_RenderThread(CommandList, ShadowView,
					FallbackLighting, ERenderMode::Unlit, *Draws, Batch,
					ResolvedView, true, &DynamicNanoseconds))
				{
					++ResolvedView.Observations.SuccessfulDraws;
					ResolvedView.Observations.SubmittedLogicalPatches += Batch.DrawIndices.size();
				}
				else
				{
					bComplete = false;
					++ResolvedView.Observations.RejectedDraws;
				}
				ResolvedView.Observations.DynamicAllocationNanoseconds += DynamicNanoseconds;
				ResolvedView.Observations.CommandRecordingNanoseconds += std::chrono::duration_cast<
														std::chrono::nanoseconds>(std::chrono::steady_clock::now() - Begin)
														.count();
			}
		check(ResolvedView.Observations.AttemptedDraws == ResolvedView.Observations.SuccessfulDraws + ResolvedView.Observations.RejectedDraws);
		return bComplete;
	}

	auto FTerrainRenderer::Draw_RenderThread(FRHICommandListImmediate& CommandList, const FSceneView& SceneView, const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode, const FPreparedTerrainDraw& Draw, const FResolvedTerrainView& ResolvedView, bool bShadowDepth, std::span<const std::array<uint32, 2>> InstanceOrigins, uint64* OutDynamicAllocationNanoseconds, FGBufferRenderer* GBuffer, bool bHybridRetained) -> bool
	{
		if (!ResolvedView.IsReady(Draw) || !Draw.SceneInfo || !Draw.Patch)
			return false;
		const FMaterialRenderBinding* MaterialBinding =
			ResolvedView.GetMaterialBinding(Draw);
		if (MaterialBinding == nullptr) return false;
		const FTerrainSceneProxy& Proxy = Draw.SceneInfo->GetTerrainProxy();
		const auto Payload = Proxy.GetPayload();
		auto HeightIt = State->Heights.find(Payload.get());
		const FTerrainTopologyKey Key{
			static_cast<uint16>(Draw.Patch->CellCountX),
			static_cast<uint16>(Draw.Patch->CellCountY),
			static_cast<uint16>(Draw.LODStep), Draw.StitchMask
		};
		auto TopologyIt = State->Topologies.find(Key);
		FEffectiveMeshPipelineKey EffectivePipelineKey =
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
		Transform.LocalToClip = Math::TransposeToFloat(LocalToClip);
		Transform.LocalToWorld = Math::TransposeToFloat(LocalToWorld);
		Transform.NormalToWorld = Math::TransposeToFloat(
			Math::Transpose(Math::Inverse(LocalToWorld)));
		Transform.TransformParams.x = Math::LinearDeterminant(LocalToWorld) < 0.0 ? -1.0f : 1.0f;
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
		const bool bMaskedShadow = bShadowDepth
			&& Draw.Material.PlanningPassIdentity.ShaderMap.BlendMode
				== EMaterialBlendMode::Masked;
		const ESurfaceMaterialPass SurfacePass = GBuffer != nullptr
			? ESurfaceMaterialPass::GBuffer
			: (bMaskedShadow ? ESurfaceMaterialPass::MaskedShadow
				: ESurfaceMaterialPass::Forward);
		FResolvedSurfaceMaterial SurfaceMaterial;
		if (!SurfaceMaterials.Resolve_RenderThread(
				*MaterialBinding, SurfacePass,
				RenderMode == ERenderMode::Lit
					&& Draw.Material.PlanningPassIdentity.ShaderMap.ShadingModel
						== EMaterialShadingModel::Lit,
				SceneView.Settings.Mode.bEnableSpecularAA,
				ResolvedView.DirectionalShadowTexture,
				ResolvedView.DirectionalShadowSampler,
				SurfaceMaterial)) return false;
		const auto MaterialBuffer = CommandList.AllocateDynamicUniformBuffer(
			&SurfaceMaterial.Uniform, sizeof(SurfaceMaterial.Uniform));
		if (GBuffer != nullptr)
		{
			const FVertexDeclarationRHIRef VertexDeclaration(
				TopologyIt->second->VertexFactory.GetDeclaration()
			);
			FGBufferRenderer::FPipeline* GBufferPipeline =
				GBuffer->EnsurePipeline_RenderThread({.Material = Draw.PipelineKey.Material, .CompiledProgram = Draw.Material.CompiledProgram, .Rasterizer = Draw.PipelineKey.Rasterizer, .Depth = Draw.PipelineKey.Depth, .VertexDeclaration = VertexDeclaration, .VertexDomain = EGBufferVertexDomain::Terrain});
			if (GBufferPipeline == nullptr) return false;
			const FGBufferRenderer::FVertexParameters VertexParameters{
				.Transform = TransformBuffer,
				.HeightTexture = HeightIt->second.Texture,
				.Terrain = TerrainBuffer,
				.TerrainPatchOrigins = InstanceRange
			};
			FGBufferRenderer::FFragmentParameters FragmentParameters;
			FragmentParameters.Material = MaterialBuffer;
			FragmentParameters.Textures = SurfaceMaterial.Textures;
			FragmentParameters.Samplers = SurfaceMaterial.Samplers;
			if (!GBuffer->BindPipeline_RenderThread(CommandList, *GBufferPipeline, VertexParameters, FragmentParameters))
			{
				return false;
			}
			CommandList.DrawIndexed(DrawArguments);
			return true;
		}
		if (bMaskedShadow)
		{
			const auto ShadowParameters = MakeSurfaceMaskedShadowParameters(
				SurfaceMaterial, MaterialBuffer);
			SetShaderParameters(CommandList, Pipeline->ShadowFragment, ShadowParameters);
			CommandList.DrawIndexed(DrawArguments);
			return true;
		}
		const auto PS = MakeSurfaceForwardParameters(
			SurfaceMaterial, MaterialBuffer, Lighting);
		SetShaderParameters(CommandList, Pipeline->Fragment, PS);
		CommandList.DrawIndexed(DrawArguments);
		return true;
	}

	auto FTerrainRenderer::DrawBatch_RenderThread(
		FRHICommandListImmediate& CommandList, const FSceneView& SceneView, const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode, const std::vector<FPreparedTerrainDraw>& Draws, const FPreparedTerrainBatch& Batch, const FResolvedTerrainView& ResolvedView, bool bShadowDepth, uint64* OutDynamicAllocationNanoseconds, FGBufferRenderer* GBuffer
	) -> bool
	{
		if (!ResolvedView.IsReady(Batch) || Batch.DrawIndices.empty()
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
		return Draw_RenderThread(CommandList, SceneView, Lighting, RenderMode,
			First, ResolvedView, bShadowDepth, Origins,
			OutDynamicAllocationNanoseconds, GBuffer);
	}

	auto FTerrainRenderer::ExecutePreparedDraw_RenderThread(
		FRHICommandListImmediate& CommandList, const FSceneView& SceneView, const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode, const FPreparedTerrainDraw& Draw, const FPreparedTerrainView& View, FResolvedTerrainView& ResolvedView, bool bHybridRetained
	) -> void
	{
		const auto Begin = std::chrono::steady_clock::now();
		++ResolvedView.Observations.AttemptedDraws;
		++ResolvedView.Observations.ScalarTranslucentDraws;
		++ResolvedView.Observations.InstanceAllocations;
		++ResolvedView.Observations.InstanceCount;
		ResolvedView.Observations.InstanceBytes += sizeof(FTerrainInstanceData);
		uint64 DynamicNanoseconds = 0;
		if (Draw_RenderThread(CommandList, SceneView, Lighting, RenderMode,
			Draw, ResolvedView, false, {}, &DynamicNanoseconds, nullptr,
			bHybridRetained))
		{
			++ResolvedView.Observations.SuccessfulDraws;
			++ResolvedView.Observations.SubmittedLogicalPatches;
		}
		else
			++ResolvedView.Observations.RejectedDraws;
		ResolvedView.Observations.DynamicAllocationNanoseconds += DynamicNanoseconds;
		ResolvedView.Observations.CommandRecordingNanoseconds += std::chrono::duration_cast<
												std::chrono::nanoseconds>(std::chrono::steady_clock::now() - Begin)
												.count();
	}

	auto FTerrainRenderer::ExecutePass_RenderThread(
		FRHICommandListImmediate& CommandList, const FSceneView& SceneView, const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode, EMeshBasePass Pass, const FPreparedTerrainView& View, FResolvedTerrainView& ResolvedView
	) -> void
	{
		const auto& Draws = Pass == EMeshBasePass::Opaque ? View.Opaque : View.Masked;
		const auto& Batches = Pass == EMeshBasePass::Opaque ? View.OpaqueBatches : View.MaskedBatches;
		for (const auto& Batch : Batches)
		{
			const auto Begin = std::chrono::steady_clock::now();
			++ResolvedView.Observations.AttemptedDraws;
			++ResolvedView.Observations.InstanceAllocations;
			ResolvedView.Observations.InstanceBytes += Batch.DrawIndices.size() * sizeof(FTerrainInstanceData);
			uint64 DynamicNanoseconds = 0;
			if (DrawBatch_RenderThread(CommandList, SceneView, Lighting,
				RenderMode, Draws, Batch, ResolvedView, false,
				&DynamicNanoseconds))
			{
				++ResolvedView.Observations.SuccessfulDraws;
				ResolvedView.Observations.SubmittedLogicalPatches += Batch.DrawIndices.size();
			}
			else
				++ResolvedView.Observations.RejectedDraws;
			ResolvedView.Observations.DynamicAllocationNanoseconds += DynamicNanoseconds;
			ResolvedView.Observations.CommandRecordingNanoseconds += std::chrono::duration_cast<
													std::chrono::nanoseconds>(std::chrono::steady_clock::now() - Begin)
													.count();
		}
	}

	auto FTerrainRenderer::ExecuteGBuffer_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneView& SceneView,
		FGBufferRenderer& GBuffer,
		const FPreparedTerrainView& View,
		FResolvedTerrainView& ResolvedView
	) -> FGeometryExecutionResult
	{
		check(CommandList.IsInsideRenderPass());
		bool bComplete = true;
		bool bRenderedGeometry = false;
		ResolvedView.Observations.GBufferSkippedDraws += View.Translucent.size();
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
					bComplete = false;
					++ResolvedView.Observations.GBufferAttemptedDraws;
					++ResolvedView.Observations.GBufferRejectedDraws;
					continue;
				}
				const FPreparedTerrainDraw& Draw =
					(*Draws)[Batch.DrawIndices.front()];
				if (Draw.Material.PlanningPassIdentity.ShaderMap.ShadingModel
					!= EMaterialShadingModel::Lit)
				{
					++ResolvedView.Observations.GBufferSkippedDraws;
					continue;
				}
				++ResolvedView.Observations.GBufferAttemptedDraws;
				if (DrawBatch_RenderThread(
						CommandList,
						SceneView,
						{},
						ERenderMode::Lit,
						*Draws,
						Batch,
						ResolvedView,
						false,
						nullptr,
						&GBuffer
					))
				{
					bRenderedGeometry = true;
					++ResolvedView.Observations.GBufferSuccessfulDraws;
				}
				else
				{
					bComplete = false;
					++ResolvedView.Observations.GBufferRejectedDraws;
				}
			}
		}
		check(ResolvedView.Observations.GBufferAttemptedDraws == ResolvedView.Observations.GBufferSuccessfulDraws + ResolvedView.Observations.GBufferRejectedDraws);
		return {bComplete, bRenderedGeometry,
			ResolvedView.Observations.GBufferAttemptedDraws,
			ResolvedView.Observations.GBufferSuccessfulDraws,
			ResolvedView.Observations.GBufferRejectedDraws,
			ResolvedView.Observations.GBufferSkippedDraws};
	}

	auto FTerrainRenderer::FinalizeExecution_RenderThread(
		FResolvedTerrainView& ResolvedView) -> void
	{
		check(ResolvedView.Observations.AttemptedDraws == ResolvedView.Observations.SuccessfulDraws + ResolvedView.Observations.RejectedDraws);
	}

	auto FTerrainRenderer::ReleaseResources_RenderThread() -> void
	{
		for (auto& [Key, Topology] : State->Topologies)
			if (Topology->VertexFactory.IsInitialized()) Topology->VertexFactory.ReleaseResource();
		State->Topologies.clear();
		State->Heights.clear();
		State->Shaders.Reset();
		State->ShadowShaders.Reset();
		State->Pipelines.Reset();
		State->ShadowPipelines.Reset();
	}
} // namespace Durin
