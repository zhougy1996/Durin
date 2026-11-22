#pragma once

#include "CoreMinimal.h"
#include "RendererAPI.h"
#include "RenderResourceCreation.h"
#include "RHIGPUReadback.h"
#include "SceneView.h"
#include "GPUScene.h"
#include "Rendering/StaticMeshBatchPublication.h"
#include "Rendering/PrimitiveComponentId.h"

namespace Durin
{
	class FRDGBuilder;
	class FPrimitiveSceneInfo;
	struct FGBufferGraphOutput;
	struct FInstanceSceneDataBuffers;

	struct FOcclusionPrimitive
	{
		FGPUScenePrimitiveHandle Handle;
		std::weak_ptr<const FStaticMeshBatchGeometry> Geometry;
		std::weak_ptr<const FInstanceSceneDataBuffers> Instances;
		uint32 MaterialCount;
		FMatrix Transform;
		FBox Bounds;
		auto operator==(const FOcclusionPrimitive& Other) const -> bool;
	};

	struct FOcclusionViewKey
	{
		std::weak_ptr<FGPUScene> Scene;
		FRenderResourceGeneration Generation;
		FMatrix View, Projection, ViewProjection;
		ERHIFeatureLevel FeatureLevel;
		FVector2 FadeRange;
		FVector3 Origin;
		std::array<uint32, 4> Viewport;
		uint32 Width = 0, Height = 0;
		ESceneDepthConvention Depth{};
		EViewLODMode LODMode{};
		auto CanReuseResults(const FOcclusionViewKey& Previous) const -> bool;
	};

	struct FOcclusionSceneInputs
	{
		std::vector<FOcclusionPrimitive> Primitives;
		std::vector<uint64> MaterialRecords;
		auto operator==(const FOcclusionSceneInputs&) const -> bool = default;
	};

	// One farthest-depth level, with both depth conventions. Each word pair covers an entire 16x16 tile.
	RENDERER_API auto IsOccludedByDepthTiles(const FSceneView& View, const FBox& Bounds,
		uint32 Width, uint32 Height, std::span<const FVector2f> Tiles) -> bool;

	// Tile rectangle in the producing view. A zero stride always yields visible.
	struct FHZBProjectedBounds
	{
		uint32 FirstX, FirstY, LastX, LastY;
		uint32 Stride, Reversed;
		float Nearest;
	};

	struct FHZBBoundsBatch
	{
		FOcclusionViewKey Key;
		FOcclusionSceneInputs Inputs;
		std::unordered_map<FPrimitiveComponentId, uint32, FPrimitiveComponentIdHash> TestIndices;
		std::vector<FHZBProjectedBounds> Bounds;
		FBufferRHIRef BoundsBuffer;
		auto MapCandidates(std::span<const FPrimitiveSceneInfo* const> Candidates, std::vector<uint32>& Order) const -> bool;
		auto AddBounds(const FSceneView& View, const FMatrix& WorldToClip, const FBox& Box) -> void;
	};

	struct FOcclusionFeedback
	{
		std::shared_ptr<const FHZBBoundsBatch> Batch;
		uint64 Frame = 0;
		FRHIGPUBufferReadback Readback;
		std::shared_ptr<class FComputePipelineState> ReducePipeline, TestPipeline;
		bool bValid = true;
	};

	class FScene;
	class FHZBOcclusionTester
	{
	public:
		RENDERER_API auto Invalidate() -> void;

		RENDERER_API auto Prepare(FRHICommandListImmediate& Commands, const FSceneView& View,
			const FScene& Scene, const FRenderResourceGeneration& Generation, uint32 Width, uint32 Height,
			uint64 Frame, double RealTimeSeconds, std::vector<const FPrimitiveSceneInfo*>& Candidates,
			FStaticMeshBatchCaptureContext& Capture)
			-> std::shared_ptr<FOcclusionFeedback>;
		auto Submit(FRDGBuilder& Graph, const FGBufferGraphOutput& GBuffer,
			const std::shared_ptr<FOcclusionFeedback>& Feedback) -> void;
	private:
		struct FMappedResults
		{
			FRHIGPUBufferReadback& Readback;
			const uint32* Values;
			auto IsVisible(size_t Index) const -> bool { return Values[Index] != 1u; }
			auto UnmapResults() -> void { Readback.Unlock(); }
		};
		auto MapResults(uint64 Frame, const FOcclusionViewKey& Key) -> std::optional<FMappedResults>;
		auto Reserve(uint64 Frame) -> std::shared_ptr<FOcclusionFeedback>;
		std::array<std::shared_ptr<FOcclusionFeedback>, 3> Slots;
		std::optional<uint64> LastFrame;
		double LastRenderTime = 0;
		FOcclusionSceneInputs Scratch;
		std::vector<uint32> CandidateOrder;
		std::shared_ptr<const FHZBBoundsBatch> CurrentBatch;
	};

}
