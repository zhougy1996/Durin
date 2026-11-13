#pragma once

#include "CoreMinimal.h"
#include "RendererAPI.h"
#include "InstanceUniformShaderParameters.h"
#include "RenderResource.h"
#include "RDG/RDG.h"

namespace Durin
{
	class FPrimitiveSceneInfo;

	enum class EGPUSceneDirtyState : uint8
	{
		None = 0, Added = 1, Transform = 2, PrimitiveCustomData = 4,
		InstanceData = 8, Bounds = 16, Removed = 32, All = 31
	};

	struct FGPUScenePrimitiveHandle
	{
		FPersistentPrimitiveIndex PersistentPrimitiveIndex;
		uint64 Generation = 0;
		auto operator==(const FGPUScenePrimitiveHandle&) const -> bool = default;
	};

	class FGPUScene;
	// Non-owning scene membership identity; resolving never retains SceneInfo storage.
	struct FPrimitiveSceneInfoRef
	{
		std::weak_ptr<FGPUScene> GPUScene;
		FGPUScenePrimitiveHandle Handle;
		auto IsSceneBound() const -> bool { return Handle.PersistentPrimitiveIndex.IsValid(); }
		RENDERER_API auto Resolve() const -> FPrimitiveSceneInfo*;
	};

	struct FGPUSceneResourceParameters
	{
		FRDGBufferParameter GPUScenePrimitiveSceneData;
		FRDGBufferParameter GPUSceneInstanceSceneData;
		FRDGBufferParameter GPUSceneInstancePayloadData;
		uint32 GPUSceneNumAllocatedPrimitives = 0;
		uint32 GPUSceneNumAllocatedInstances = 0;
		uint64 GPUSceneResourceGeneration = 0;
	};

    // Cumulative admitted uploads plus current native capacity. Failed/abandoned
    // transactions do not increase committed counters; capacity is not traffic.
    struct FGPUSceneStatistics
    {
        uint64 UploadBytes = 0;
        uint64 UploadRanges = 0;
        uint64 BufferRebuilds = 0;
        uint64 CapacityBytes = 0;
        uint64 MirrorCapacityBytes = 0;
    };

	struct FGPUSceneDynamicAllocation
	{
		uint64 Id = 0;
		uint32 PrimitiveOffset = 0, InstanceOffset = 0, Count = 0;
	};

	struct FGPUSceneUpdate;
	struct FGPUSceneUpdateResult
	{
		FGPUSceneResourceParameters Parameters;
		uint64 Revision = 0;
		uint64 UploadBytes = 0;
		uint32 UploadRanges = 0;
		uint32 BufferRebuilds = 0;
	private:
		friend class FGPUScene;
		std::shared_ptr<FGPUSceneUpdate> Update;
	};

	// Scene-owned source state survives ReleaseRHI; only committed GPU state is reset.
	class FGPUScene final : public FRenderResource, public std::enable_shared_from_this<FGPUScene>
	{
	public:
		RENDERER_API FGPUScene();
		RENDERER_API ~FGPUScene() override;
		RENDERER_API auto AllocateDynamicPrimitiveRange(uint32 Count) -> FGPUSceneDynamicAllocation;
		RENDERER_API auto WriteDynamicPrimitive(const FGPUSceneDynamicAllocation& Allocation, uint32 Index,
			const FMatrix& Transform, const FBox& Bounds, const FPrimitiveCustomData& PrimitiveData, const FPrimitiveCustomData& InstanceData) -> bool;
		RENDERER_API auto ReleaseDynamicPrimitiveRange(const FGPUSceneDynamicAllocation& Allocation) -> void;
		RENDERER_API auto ResolvePrimitive(FGPUScenePrimitiveHandle Handle) const -> FPrimitiveSceneInfo*;
		RENDERER_API auto AddPrimitive(FPrimitiveSceneInfo& Info) -> void;
		RENDERER_API auto RemovePrimitive(FPrimitiveSceneInfo& Info) -> void;
		RENDERER_API auto RequestUpdate(FGPUScenePrimitiveHandle Handle, EGPUSceneDirtyState Dirty) -> bool;
		RENDERER_API auto UpdateCPUData() -> void;
		RENDERER_API auto Update(FRDGBuilder& GraphBuilder) -> FGPUSceneUpdateResult;
		RENDERER_API auto GetShaderParameters(const FRDGBuilder& GraphBuilder) const -> FGPUSceneResourceParameters;
		// Must be called after this exact graph executes; failed/unexecuted graphs
		// never acknowledge dirty data. Dropping a result also leaves it pending.
		RENDERER_API auto CompleteUpdate(const FRDGBuilder& GraphBuilder, const FGPUSceneUpdateResult& Result) -> bool;
		RENDERER_API auto ReleaseRHI() -> void override;
        RENDERER_API auto GetStatistics() const -> FGPUSceneStatistics;
		auto GetFriendlyName() const -> std::string override { return "GPUScene"; }
		RENDERER_API auto GetPrimitiveSceneData() const -> std::span<const FPrimitiveSceneShaderData>;
		RENDERER_API auto GetInstanceSceneData() const -> std::span<const FInstanceSceneShaderData>;
		RENDERER_API auto GetInstancePayloadData() const -> std::span<const FPrimitiveCustomData>;
		RENDERER_API auto GetResourceBuffers() const -> std::array<FBufferRHIRef, 3>;
		RENDERER_API auto FindInstanceId(FPersistentPrimitiveIndex Primitive, FPrimitiveInstanceId StableInstanceId) const -> std::optional<uint32>;
		RENDERER_API auto IsDirty() const -> bool;
		RENDERER_API auto RegisterGraphUse(const FRDGBuilder& Graph) -> void;
	private:
		struct FState;
		std::unique_ptr<FState> State;
	};
}
