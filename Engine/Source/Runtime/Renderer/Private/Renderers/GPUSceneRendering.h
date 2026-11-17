#pragma once

#include "CoreMinimal.h"
#include "RendererAPI.h"
#include "GPUSceneParameters.h"
#include "GPUScene.h"
#include "PrimitiveSceneShaderData.h"
#include "InstanceUniformShaderParameters.h"
#include "RDG/RDG.h"

namespace Durin
{
	class FGPUSceneDynamicContext;
	class FGPUScenePrimitiveCollector;
	struct FGPUSceneBufferSet
	{
		std::array<FBufferRHIRef, 3> SceneData;
		FBufferRHIRef InstanceIds;
		std::shared_ptr<FGPUSceneDynamicContext> DynamicContext;
		bool bStandaloneScene = false;
		uint32 NumAllocatedPrimitives = 0, NumAllocatedInstances = 0;
		uint64 ResourceGeneration = 0;
		RENDERER_API auto GetShaderParameters() const -> FGPUSceneShaderParameters;
		RENDERER_API auto GetShaderParameters(FRDGBuilder& Graph) const -> FGPUSceneResourceParameters;
	};
	struct FGPUScenePrimitiveData
	{
		std::shared_ptr<const FGPUSceneBufferSet> Buffers;
		uint32 InstanceId = INDEX_NONE_U32;
		uint32 InstanceIdOffset = 0;
	};
	// Immutable logical record identity. Sharing this record explicitly reuses its allocation.
	struct FGPUSceneDynamicPrimitiveData
	{
		FMatrix LocalToWorld{1.0};
		FBox WorldBounds;
		FPrimitiveCustomData CustomPrimitiveData, CustomInstanceData;
	};
	// Generic submission inputs. Provider filtering and prepared-view projection stay outside.
	class FGPUScenePrimitiveCollector
	{
	public:
		explicit FGPUScenePrimitiveCollector(std::shared_ptr<FGPUSceneDynamicContext> InContext) : Context(std::move(InContext)) {}
		FGPUScenePrimitiveCollector(const FGPUScenePrimitiveCollector&) = delete;
		auto operator=(const FGPUScenePrimitiveCollector&) -> FGPUScenePrimitiveCollector& = delete;
		RENDERER_API auto Add(std::shared_ptr<const FGPUSceneDynamicPrimitiveData> MeshBatchData, uint32 NumInstances = 1) -> uint32;
		RENDERER_API auto Add(FPersistentPrimitiveIndex PrimitiveIndex, FPrimitiveInstanceId InstanceId, uint32 NumInstances = 1) -> uint32;
		RENDERER_API auto Commit() -> bool;
		RENDERER_API auto GetPrimitiveData(uint32 PrimitiveIndex) const -> const FGPUScenePrimitiveData&;
		auto Num() const -> size_t { return Primitives.size(); }
	private:
		friend class FGPUSceneDynamicContext;
		struct FPrimitiveData
		{
			std::shared_ptr<const FGPUSceneDynamicPrimitiveData> DynamicData;
			FPrimitiveSceneInfoRef Source;
			FPrimitiveInstanceId InstanceId;
			uint32 NumInstances = 1;
		};
		std::shared_ptr<FGPUSceneDynamicContext> Context;
		std::vector<FPrimitiveData> Primitives;
		std::vector<FGPUScenePrimitiveData> Results;
		bool bCommitted = false;
	};
	// A submission-scoped upload owner for dynamic/unpublished mesh facts. Scene
	// members reuse persistent attributes; ID lists belong to this submission.
	class FGPUSceneDynamicContext : public std::enable_shared_from_this<FGPUSceneDynamicContext>
	{
	public:
		RENDERER_API explicit FGPUSceneDynamicContext(std::shared_ptr<FGPUScene> InGPUScene = {});
		RENDERER_API ~FGPUSceneDynamicContext();
		FGPUSceneDynamicContext(const FGPUSceneDynamicContext&) = delete;
		auto operator=(const FGPUSceneDynamicContext&) -> FGPUSceneDynamicContext& = delete;
		RENDERER_API auto RegisterGraphUse(const FRDGBuilder& Graph) -> void;
		RENDERER_API auto UploadInstanceIds(FRHICommandListImmediate& Commands, std::span<const uint32> Ids) -> FBufferRHIRef;
		auto GetGPUScene() const -> FGPUScene& { return *GPUScene; }
		auto IsUsingPersistentGPUScene() const -> bool { return !bStandaloneScene; }
		RENDERER_API auto Upload(FRHICommandListImmediate& Commands, FRDGAllocator& Allocator) -> bool;

	private:
		friend class FGPUScenePrimitiveCollector;
		RENDERER_API auto CommitPrimitiveCollector(FGPUScenePrimitiveCollector& Collector) -> bool;
		std::shared_ptr<FGPUScene> GPUScene;
		std::vector<FGPUSceneDynamicAllocation> Allocations;
		bool bStandaloneScene = false;
		std::map<std::shared_ptr<const FGPUSceneDynamicPrimitiveData>, uint32, std::owner_less<std::shared_ptr<const FGPUSceneDynamicPrimitiveData>>> DynamicInstances;
		std::vector<std::weak_ptr<FGPUSceneBufferSet>> PendingBufferSets;
		std::vector<uint32> InstanceIds;
		std::vector<std::shared_ptr<FGPUSceneInstanceIdBuffer>> InstanceIdUploads;
	};

}
