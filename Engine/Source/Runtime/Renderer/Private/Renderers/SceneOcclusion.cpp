#include "Renderers/SceneOcclusion.h"

#include "Renderers/GBufferRendering.h"
#include "StaticMesh/StaticMeshResources.h"
#include "Rendering/StaticMeshSceneProxy.h"
#include "Scene.h"
#include "SceneInfo.h"
#include "Shader/GlobalShader.h"
#include "DynamicRHI.h"
#include "RHICapabilities.h"
#include "PipelineStateCache.h"
#include "RHICommandList.h"

namespace Durin
{
	namespace
	{
		constexpr uint32 TileSize = 16;
		constexpr uint32 MaxExtent = 4096;
		constexpr uint32 MaxCandidates = 4096;
		constexpr size_t MaxMaterialRecords = 65536;
		constexpr uint64 MaxHistoryAge = 2;

		auto TileCount(uint32 Extent) -> uint32 { return (Extent + TileSize - 1) / TileSize; }

		class FHZBReduceDepthCS final : public FGlobalShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FHZBReduceDepthCS)
				DURIN_SHADER_PARAMETER_GRAPH_TEXTURE(SceneDepth, "Texture2D");
				DURIN_SHADER_PARAMETER_GRAPH_STORAGE_BUFFER(DepthTiles);
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_GLOBAL_SHADER(FHZBReduceDepthCS, FGlobalShader, "/Engine/SceneOcclusion", EShaderFrequency::Compute, "ReduceDepth");
		};
		DURIN_IMPLEMENT_GLOBAL_SHADER(FHZBReduceDepthCS);
		class FHZBTestBoundsCS final : public FGlobalShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FHZBTestBoundsCS)
				DURIN_SHADER_PARAMETER_GRAPH_STORAGE_BUFFER(TestDepthTiles);
				DURIN_SHADER_PARAMETER_GRAPH_STORAGE_BUFFER(TestBounds);
				DURIN_SHADER_PARAMETER_GRAPH_STORAGE_BUFFER(Occluded);
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_GLOBAL_SHADER(FHZBTestBoundsCS, FGlobalShader, "/Engine/SceneOcclusionBounds", EShaderFrequency::Compute, "TestBoundsMain");
		};
		DURIN_IMPLEMENT_GLOBAL_SHADER(FHZBTestBoundsCS);
		const FGlobalShaderSetRegistration GHZBShaders("Renderer", "SceneOcclusion",
			EShaderRequestEligibility::GameAndEditor, {&FHZBReduceDepthCS::StaticType()});

		const FGlobalShaderSetRegistration GHZBBoundsShaders("Renderer", "SceneOcclusionBounds",
			EShaderRequestEligibility::GameAndEditor, {&FHZBTestBoundsCS::StaticType()});

		struct FReduceParameters
		{
			FRDGTextureParameter SceneDepth;
			FRDGBufferParameter DepthTiles;
			static auto GetRDGParametersMetadata() -> const FRDGParametersMetadata*
			{
				using P = FReduceParameters;
				static const std::array Members{
					WithRDGShaderBinding(MakeRDGTextureReadMetadata<P, decltype(SceneDepth), ERDGPassType::Compute>(
						"SceneDepth", offsetof(P, SceneDepth)), ERHIBindingType::Texture),
					WithRDGShaderBinding(MakeRDGResourceParameterMemberMetadata<P, decltype(DepthTiles), FRDGBufferParameter>(
						"DepthTiles", offsetof(P, DepthTiles), ERDGParameterMemberKind::Buffer, ERDGResourceKind::Buffer,
						ERDGParameterRangeKind::BufferBytes, ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite), ERHIBindingType::StorageBuffer)};
				static const auto Metadata = MakeInlineRDGParametersMetadata<P>("SceneOcclusion.Reduce", Members);
				return &Metadata;
			}
		};
		struct FTestBoundsParameters
		{
			FRDGBufferParameter TestDepthTiles, TestBounds, Occluded;
			static auto GetRDGParametersMetadata() -> const FRDGParametersMetadata*
			{
				using P = FTestBoundsParameters;
				auto Buffer = [](const char* Name, size_t Offset, ERDGUse Use, ERHIAccess Access)
				{
					return WithRDGShaderBinding(MakeRDGResourceParameterMemberMetadata<P, FRDGBufferParameter, FRDGBufferParameter>(
						Name, Offset, ERDGParameterMemberKind::Buffer, ERDGResourceKind::Buffer,
						ERDGParameterRangeKind::BufferBytes, Use, Access), ERHIBindingType::StorageBuffer);
				};
				static const std::array Members{
					Buffer("TestDepthTiles", offsetof(P, TestDepthTiles), ERDGUse::ReadWrite, ERHIAccess::ComputeShaderReadWrite),
					Buffer("TestBounds", offsetof(P, TestBounds), ERDGUse::Read, ERHIAccess::ComputeShaderRead),
					Buffer("Occluded", offsetof(P, Occluded), ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite)};
				static const auto Metadata = MakeInlineRDGParametersMetadata<P>("SceneOcclusion.TestBounds", Members);
				return &Metadata;
			}
		};
		auto CaptureOcclusionPrimitive(const FPrimitiveSceneInfo& Info, FStaticMeshBatchCaptureContext& Capture,
			std::vector<uint64>& Materials) -> std::optional<FOcclusionPrimitive>
		{
			const auto& Proxy = Info.GetProxy();
			if (typeid(Proxy) != typeid(FStaticMeshSceneProxy)) return {};
			const auto& Mesh = static_cast<const FStaticMeshSceneProxy&>(Proxy);
			if (!Mesh.GetRenderData()) return {};
			const auto& Geometry = Capture.CaptureGeometry(*Mesh.GetRenderData());
			const auto& Slots = Geometry->ReferencedMaterialSlots;
			const auto Bindings = Mesh.GetUsedMaterials();
			if (!Geometry->bValidLayout || (!Slots.empty() && Slots.back() >= Bindings.size())
				|| Slots.size() > MaxMaterialRecords - Materials.size()) return {};
			FOcclusionPrimitive Primitive{.Handle = Info.GPUSceneHandle,
				.Geometry = Geometry,
				.Instances = Mesh.GetInstanceSceneDataBuffers(), .MaterialCount = uint32(Slots.size()), .Transform = Mesh.GetLocalToWorld(), .Bounds = Info.GetWorldBounds()};
			for (const uint32 Slot : Slots)
			{
				const auto& Binding = Bindings[Slot];
				const auto Record = Binding ? Binding->GetCachedOpaqueDepthMaterialId_RenderThread() : std::nullopt;
				if (!Record) return {};
				Materials.push_back(*Record);
			}
			return Primitive;
		}
	}

	auto FOcclusionPrimitive::operator==(const FOcclusionPrimitive& Other) const -> bool
	{
		return Handle == Other.Handle && !Geometry.owner_before(Other.Geometry) && !Other.Geometry.owner_before(Geometry)
			&& !Instances.owner_before(Other.Instances) && !Other.Instances.owner_before(Instances)
			&& MaterialCount == Other.MaterialCount && Transform == Other.Transform
			&& Bounds.bIsValid == Other.Bounds.bIsValid && Bounds.Min == Other.Bounds.Min && Bounds.Max == Other.Bounds.Max;
	}
	auto FOcclusionViewKey::CanReuseResults(const FOcclusionViewKey& Previous) const -> bool
	{
		if (Scene.owner_before(Previous.Scene) || Previous.Scene.owner_before(Scene)
			|| Generation != Previous.Generation
			|| Projection != Previous.Projection || FeatureLevel != Previous.FeatureLevel || FadeRange != Previous.FadeRange
			|| Viewport != Previous.Viewport || Width != Previous.Width || Height != Previous.Height
			|| Depth != Previous.Depth || LODMode != Previous.LODMode) return false;
		if (View == Previous.View) return Origin == Previous.Origin && ViewProjection == Previous.ViewProjection;

		// UE IsLargeCameraMovement, using rows for Durin's column-vector world-to-view convention.
		constexpr double TranslationThreshold = 10000.0;
		constexpr float CosRotationThreshold = 0.70710678f; // 45 degrees
		const auto Delta = Origin - Previous.Origin;
		if (!Math::IsFinite(Delta) || glm::dot(Delta, Delta) > TranslationThreshold * TranslationThreshold) return false;
		for (int Axis = 0; Axis < 3; ++Axis)
		{
			const FVector3 Current(View[0][Axis], View[1][Axis], View[2][Axis]);
			const FVector3 Prior(Previous.View[0][Axis], Previous.View[1][Axis], Previous.View[2][Axis]);
			if (!(glm::dot(Current, Prior) >= CosRotationThreshold)) return false;
		}
		return true;
	}

	auto FHZBBoundsBatch::MapCandidates(std::span<const FPrimitiveSceneInfo* const> Candidates, std::vector<uint32>& Order) const -> bool
	{
		if (Candidates.size() != TestIndices.size()) return false;
		Order.assign(Candidates.size(), INDEX_NONE_U32);
		for (uint32 Index = 0; Index < Candidates.size(); ++Index)
		{
			const auto It = TestIndices.find(Candidates[Index]->GetId());
			if (It == TestIndices.end() || Order[It->second] != INDEX_NONE_U32) return false;
			Order[It->second] = Index;
		}
		return true;
	}

	static auto ProjectBounds(const FSceneView& View, const FMatrix& Matrix, const FBox& Bounds, uint32 Width, uint32 Height) -> FHZBProjectedBounds
	{
		if (!Width || !Height || Width > MaxExtent || Height > MaxExtent || !View.ViewportWidth || !View.ViewportHeight
			|| uint64(View.ViewportX) + View.ViewportWidth > Width || uint64(View.ViewportY) + View.ViewportHeight > Height
			|| !Bounds.bIsValid
			|| !Math::IsFinite(Bounds.Min) || !Math::IsFinite(Bounds.Max) || glm::any(glm::greaterThan(Bounds.Min, Bounds.Max))) return {};
		const bool bReversed = View.DepthConvention == ESceneDepthConvention::ReversedZ;
		FVector2 Min(1.0), Max(-1.0);
		double Nearest = bReversed ? 0.0 : 1.0;
		for (uint32 Corner = 0; Corner < 8; ++Corner)
		{
			const FVector4 Position(FVector3(Corner & 1 ? Bounds.Max.x : Bounds.Min.x,
				Corner & 2 ? Bounds.Max.y : Bounds.Min.y, Corner & 4 ? Bounds.Max.z : Bounds.Min.z) - View.ViewLocation, 1.0);
			const FVector4 Clip = Matrix * Position;
			if (!Math::IsFinite(Clip) || Clip.w <= 1.e-5) return {};
			// Reject ill-conditioned projections before using a finite pixel/depth guard band.
			const FVector4 Magnitudes = glm::abs(Matrix[0] * Position.x) + glm::abs(Matrix[1] * Position.y)
				+ glm::abs(Matrix[2] * Position.z) + glm::abs(Matrix[3]);
			if (glm::any(glm::greaterThan(Magnitudes, FVector4(16.0 * Clip.w)))) return {};
			const FVector3 Ndc = FVector3(Clip) / Clip.w;
			if (std::abs(Ndc.x) >= 1 || std::abs(Ndc.y) >= 1 || Ndc.z <= 0 || Ndc.z >= 1) return {};
			Min = glm::min(Min, FVector2(Ndc));
			Max = glm::max(Max, FVector2(Ndc));
			Nearest = bReversed ? std::max(Nearest, Ndc.z) : std::min(Nearest, Ndc.z);
		}
		const FVector2 Extent(View.ViewportWidth, View.ViewportHeight), Origin(View.ViewportX, View.ViewportY);
		const auto First = glm::floor(Origin + (Min * 0.5 + 0.5) * Extent) - 1.0;
		const auto Last = glm::ceil(Origin + (Max * 0.5 + 0.5) * Extent) + 1.0;
		if (glm::any(glm::lessThan(First, Origin)) || glm::any(glm::greaterThanEqual(Last, Origin + Extent))) return {};
		return {uint32(First.x) / TileSize, uint32(First.y) / TileSize, uint32(Last.x) / TileSize,
			uint32(Last.y) / TileSize, TileCount(Width), uint32(bReversed), std::nextafter(float(Nearest), bReversed ? 1.f : 0.f)};
	}

	auto FHZBBoundsBatch::AddBounds(const FSceneView& View, const FMatrix& WorldToClip, const FBox& Box) -> void
	{
		Bounds.push_back(ProjectBounds(View, WorldToClip, Box, Key.Width, Key.Height));
	}

	auto IsOccludedByDepthTiles(const FSceneView& View, const FBox& Bounds, uint32 Width, uint32 Height,
		std::span<const FVector2f> Tiles) -> bool
	{
		const auto B = ProjectBounds(View, View.GetTranslatedWorldToClip(), Bounds, Width, Height);
		if (!B.Stride || Tiles.size() != size_t(B.Stride) * TileCount(Height)) return false;
		for (uint32 Y = B.FirstY; Y <= B.LastY; ++Y)
			for (uint32 X = B.FirstX; X <= B.LastX; ++X)
			{
				const auto Z = Tiles[size_t(Y) * TileCount(Width) + X];
				if (!std::isfinite(Z.x) || !std::isfinite(Z.y) || Z.x < 0 || Z.y > 1 || Z.x > Z.y
					|| (B.Reversed ? B.Nearest >= Z.x - 1.e-4f : B.Nearest <= Z.y + 1.e-4f)) return false;
			}
		return true;
	}

	auto FHZBOcclusionTester::Invalidate() -> void
	{
		CurrentBatch.reset();
		for (auto& Slot : Slots)
		{
			if (!Slot) continue;
			if (Slot->Readback.IsInFlight()) Slot->bValid = false;
			else Slot.reset();
		}
	}

	auto FHZBOcclusionTester::MapResults(uint64 Frame, const FOcclusionViewKey& Key) -> std::optional<FMappedResults>
	{
		FOcclusionFeedback* Newest = nullptr;
		for (const auto& Slot : Slots)
		{
			if (!Slot || !Slot->bValid) continue;
			if (Frame <= Slot->Frame || Frame - Slot->Frame > MaxHistoryAge)
			{
				Slot->bValid = false;
				continue;
			}
			if ((!Newest || Slot->Frame > Newest->Frame) && Slot->Readback.IsReady()
				&& Key.CanReuseResults(Slot->Batch->Key)) Newest = Slot.get();
		}
		const uint32 Bytes = uint32(CurrentBatch->Bounds.size()) * sizeof(uint32);
		if (Newest && Newest->Readback.GetDataSize() == Bytes)
			if (const auto* Values = static_cast<const uint32*>(Newest->Readback.Lock(Bytes)))
				return FMappedResults{Newest->Readback, Values};
		return {};
	}

	auto FHZBOcclusionTester::Reserve(uint64 Frame) -> std::shared_ptr<FOcclusionFeedback>
	{
		auto* Oldest = &Slots.front();
		for (auto& Slot : Slots)
		{
			if (!Slot) { Oldest = &Slot; break; }
			if (!Slot->Readback.IsInFlight() && (Slot->Frame < (*Oldest)->Frame || (*Oldest)->Readback.IsInFlight())) Oldest = &Slot;
		}
		if (*Oldest && (*Oldest)->Readback.IsInFlight()) return {};
		auto Feedback = std::make_shared<FOcclusionFeedback>();
		Feedback->Batch = CurrentBatch;
		Feedback->Frame = Frame;
		*Oldest = Feedback;
		return Feedback;
	}

	auto FHZBOcclusionTester::Prepare(FRHICommandListImmediate& Commands, const FSceneView& View, const FScene& Scene,
		const FRenderResourceGeneration& Generation, uint32 Width, uint32 Height, uint64 Frame, double RealTimeSeconds,
		std::vector<const FPrimitiveSceneInfo*>& Candidates, FStaticMeshBatchCaptureContext& Capture) -> std::shared_ptr<FOcclusionFeedback>
	{
		if (!Width || !Height || Width > MaxExtent || Height > MaxExtent || Candidates.empty() || Candidates.size() > MaxCandidates
			|| View.Settings.Mode.RenderMode != ERenderMode::Lit || View.Settings.Mode.RasterMode != ERasterMode::Solid
			|| View.Settings.Mode.VisibilityMode != EViewVisibilityMode::Normal) { Invalidate(); return {}; }
		constexpr double ProbablyVisibleSeconds = 8.0;
		if (LastFrame && (Frame <= *LastFrame || RealTimeSeconds < LastRenderTime - 0.0001
			|| RealTimeSeconds - LastRenderTime > ProbablyVisibleSeconds)) Invalidate();
		LastRenderTime = RealTimeSeconds;
		LastFrame = Frame;
		const FOcclusionViewKey Key{Scene.GPUScene, Generation, View.ViewMatrix,
			View.ProjectionMatrix, View.ViewProjectionMatrix, View.FeatureLevel, {View.ViewFadeStart, View.ViewRenderDistance}, View.ViewLocation, {View.ViewportX, View.ViewportY, View.ViewportWidth, View.ViewportHeight},
			Width, Height, View.DepthConvention, View.Settings.Mode.LODMode};
		const bool SameMembers = CurrentBatch && CurrentBatch->MapCandidates(Candidates, CandidateOrder);
		if (!SameMembers)
		{
			CandidateOrder.resize(Candidates.size());
			for (uint32 Index = 0; Index < Candidates.size(); ++Index) CandidateOrder[Index] = Index;
		}
		Scratch.Primitives.clear();
		Scratch.MaterialRecords.clear();
		Scratch.Primitives.reserve(Candidates.size());
		for (const uint32 Index : CandidateOrder)
		{
			auto Primitive = CaptureOcclusionPrimitive(*Candidates[Index], Capture, Scratch.MaterialRecords);
			if (!Primitive) { Invalidate(); return {}; }
			Scratch.Primitives.push_back(std::move(*Primitive));
		}
		if (!SameMembers || !Key.CanReuseResults(CurrentBatch->Key) || CurrentBatch->Inputs != Scratch)
			Invalidate();
		// Even a sub-threshold camera move needs fresh projected bounds for this frame's GPU test.
		if (!CurrentBatch || CurrentBatch->Key.View != Key.View || CurrentBatch->Key.Origin != Key.Origin
			|| CurrentBatch->Key.ViewProjection != Key.ViewProjection)
		{
			auto Batch = std::make_shared<FHZBBoundsBatch>();
			Batch->Key = Key;
			std::swap(Batch->Inputs, Scratch);
			Batch->Bounds.reserve(Candidates.size());
			Batch->TestIndices.reserve(Candidates.size());
			const auto Matrix = View.GetTranslatedWorldToClip();
			for (const uint32 Index : CandidateOrder)
			{
				const auto* Info = Candidates[Index];
				if (!Info->GetId().IsValid() || !Batch->TestIndices.emplace(Info->GetId(), uint32(Batch->Bounds.size())).second)
				{
					Invalidate();
					return {};
				}
				Batch->AddBounds(View, Matrix, Info->GetWorldBounds());
			}
			auto Desc = FRHIBufferCreateDesc::Create("Scene.Occlusion.Bounds", uint32(Batch->Bounds.size() * sizeof(FHZBProjectedBounds)),
				sizeof(FHZBProjectedBounds), EBufferUsageFlags::StructuredBuffer);
			Desc.InitialData = {Batch->Bounds.data(), Desc.Size};
			Batch->BoundsBuffer = GDynamicRHI->RHICreateBuffer(Commands, Desc);
			if (!Batch->BoundsBuffer) { Invalidate(); return {}; }
			CurrentBatch = std::move(Batch);
		}
		if (auto Results = MapResults(Frame, Key))
		{
			for (size_t TestIndex = 0; TestIndex < CandidateOrder.size(); ++TestIndex)
				if (!Results->IsVisible(TestIndex)) Candidates[CandidateOrder[TestIndex]] = nullptr;
			std::erase(Candidates, nullptr);
			Results->UnmapResults();
		}
		return Reserve(Frame);
	}

	auto FHZBOcclusionTester::Submit(FRDGBuilder& Graph, const FGBufferGraphOutput& GBuffer,
		const std::shared_ptr<FOcclusionFeedback>& Feedback) -> void
	{
		if (!Feedback || !GBuffer.Completion) return;
		const auto X = TileCount(Feedback->Batch->Key.Width), Y = TileCount(Feedback->Batch->Key.Height);
		const uint32 Bytes = X * Y * sizeof(FVector2f);
		const auto* Caps = GDynamicRHI ? GDynamicRHI->RHIGetCapabilities() : nullptr;
		if (!Caps || Caps->MaxComputeWorkGroupCount[0] < X || Caps->MaxComputeWorkGroupCount[1] < Y || Caps->MaxStorageBufferRange < Bytes) return;
		const auto Shader = GetGlobalShaderMap().GetShader<FHZBReduceDepthCS>();
		auto* RHIShader = Shader.GetRHIShader(false);
		if (!RHIShader) return;
		FComputePipelineStateInitializer Init;
		Init.ComputeShader = RHIShader; Init.PipelineLayout = Shader.GetPipelineLayout();
		auto& Pipeline = Feedback->ReducePipeline;
		Pipeline = PipelineStateCache::PrecacheComputePipelineState(Init, "SceneOcclusion");
		if (!Pipeline || Pipeline->GetState() != ERHIPipelineState::Ready) return;
		const auto TestShader = GetGlobalShaderMap().GetShader<FHZBTestBoundsCS>();
		FComputePipelineStateInitializer TestInit;
		TestInit.ComputeShader = TestShader.GetRHIShader(false);
		if (!TestInit.ComputeShader) return;
		TestInit.PipelineLayout = TestShader.GetPipelineLayout();
		auto& TestPipeline = Feedback->TestPipeline;
		TestPipeline = PipelineStateCache::PrecacheComputePipelineState(TestInit, "SceneOcclusion.TestBounds");
		if (!TestPipeline || TestPipeline->GetState() != ERHIPipelineState::Ready) return;
		static_assert(sizeof(FHZBProjectedBounds) == 7 * sizeof(uint32));
		const uint32 Count = uint32(Feedback->Batch->Bounds.size());
		const uint32 BoundsBytes = Count * sizeof(FHZBProjectedBounds);
		const uint32 ResultBytes = Count * sizeof(uint32);
		if (Count > Caps->MaxComputeWorkGroupCount[0] || BoundsBytes > Caps->MaxStorageBufferRange) return;
		const auto Buffer = Graph.CreateBuffer({.Buffer = FRHIBufferDesc(Bytes, sizeof(FVector2f),
			EBufferUsageFlags::StructuredBuffer | EBufferUsageFlags::UnorderedAccess)}, "Scene.Occlusion.DepthTiles");
		auto Reduce = Graph.AllocParameters<FReduceParameters>();
		Reduce->SceneDepth = {GBuffer.Depth, {ERHITextureAspect::Depth, 0, 1, 0, 1}};
		Reduce->DepthTiles = {Buffer, 0, Bytes};
		Graph.AddPass("Scene.Occlusion.Reduce", ERDGPassType::Compute, std::move(Reduce),
			[Shader, Init, Pipeline, X, Y](FRHICommandListImmediate& Commands, const FReduceParameters& Params, const FRDGParameterResolver& Resolver) {
				Commands.SwitchPipeline(ERHIPipeline::Compute);
				SetComputePipelineState(Commands, Init);
				FHZBReduceDepthCS::FParameters Bindings;
				SetShaderParameters(Commands, Shader, Resolver.GetShaderParameters(Params), Bindings);
				Commands.Dispatch(X, Y, 1);
				Commands.SwitchPipeline(ERHIPipeline::Graphics);
			});
		const auto Bounds = Graph.RegisterExternalBuffer(Feedback->Batch->BoundsBuffer, "Scene.Occlusion.Bounds",
			ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		const auto Results = Graph.CreateBuffer({.Buffer = FRHIBufferDesc(ResultBytes, sizeof(uint32),
			EBufferUsageFlags::StructuredBuffer | EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::SourceCopy)}, "Scene.Occlusion.Results");
		auto Test = Graph.AllocParameters<FTestBoundsParameters>();
		Test->TestDepthTiles = {Buffer, 0, Bytes};
		Test->TestBounds = {Bounds, 0, BoundsBytes};
		Test->Occluded = {Results, 0, ResultBytes};
		Graph.AddPass("Scene.Occlusion.TestBounds", ERDGPassType::Compute, std::move(Test),
			[TestShader, TestInit, TestPipeline, Count](FRHICommandListImmediate& Commands, const FTestBoundsParameters& Params, const FRDGParameterResolver& Resolver) {
				Commands.SwitchPipeline(ERHIPipeline::Compute);
				SetComputePipelineState(Commands, TestInit);
				FHZBTestBoundsCS::FParameters Bindings;
				SetShaderParameters(Commands, TestShader, Resolver.GetShaderParameters(Params), Bindings);
				Commands.Dispatch(Count, 1, 1);
				Commands.SwitchPipeline(ERHIPipeline::Graphics);
			});
		using P = RendererPrivate::TMeshDrawCommandReadbackParameters<FGBufferPassResult>;
		auto Copy = Graph.AllocParameters<P>();
		Copy->Arguments = {Results, 0, ResultBytes}; Copy->Completion = {*GBuffer.Completion};
		const auto Pass = Graph.AddPass("Scene.Occlusion.Readback", ERDGPassType::Copy, std::move(Copy),
			[Feedback, ResultBytes](FRHICommandListImmediate& Commands, const P& Params, const FRDGParameterResolver& Resolver) {
				const auto Result = Resolver.ReadValue(Params.Completion);
				if (Result.IsComplete() && Result.bRenderedGeometry)
					Feedback->Readback.EnqueueCopy(Commands, Resolver.GetBuffer(Params.Arguments), ResultBytes);
			});
		Graph.MarkPassRoot(Pass, "Persistent view occlusion feedback");
	}
}
