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
	struct FPreparedStaticMeshView;
	struct FResolvedStaticMeshView;
	struct FGPUSceneBufferSet
	{
		std::array<FBufferRHIRef, 3> SceneData;
		FBufferRHIRef InstanceIds;
		std::shared_ptr<FGPUScenePrimitiveCollector> PrimitiveCollector;
		bool bStandaloneScene = false;
		RENDERER_API auto GetShaderParameters() const -> FGPUSceneShaderParameters;
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
	class FGPUScenePrimitiveCollector : public std::enable_shared_from_this<FGPUScenePrimitiveCollector>
	{
	public:
		explicit FGPUScenePrimitiveCollector(std::shared_ptr<FGPUSceneDynamicContext> InContext) : Context(std::move(InContext)) {}
		RENDERER_API auto Commit(const FPreparedStaticMeshView& Prepared) -> std::vector<FGPUScenePrimitiveData>;
		auto GetDynamicContext() const -> const std::shared_ptr<FGPUSceneDynamicContext>& { return Context; }
	private:
		friend class FGPUSceneDynamicContext;
		std::shared_ptr<FGPUSceneDynamicContext> Context;
		std::weak_ptr<FGPUSceneBufferSet> Buffers;
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
		auto CreatePrimitiveCollector() -> std::shared_ptr<FGPUScenePrimitiveCollector> { return std::make_shared<FGPUScenePrimitiveCollector>(shared_from_this()); }
		RENDERER_API auto Upload(FRHICommandListImmediate& Commands, FRDGAllocator& Allocator) -> bool;
		RENDERER_API auto Prepare(FRHICommandListImmediate& Commands, FRDGAllocator& Allocator,
			const FPreparedStaticMeshView& Prepared) -> std::vector<FGPUScenePrimitiveData>;

	private:
		friend class FGPUScenePrimitiveCollector;
		RENDERER_API auto CommitPrimitiveCollector(FGPUScenePrimitiveCollector& Collector, const FPreparedStaticMeshView& Prepared) -> std::vector<FGPUScenePrimitiveData>;
		std::shared_ptr<FGPUScene> GPUScene;
		std::vector<FGPUSceneDynamicAllocation> Allocations;
		bool bStandaloneScene = false;
		std::map<std::shared_ptr<const FGPUSceneDynamicPrimitiveData>, uint32, std::owner_less<std::shared_ptr<const FGPUSceneDynamicPrimitiveData>>> DynamicInstances;
		std::vector<std::weak_ptr<FGPUScenePrimitiveCollector>> PendingCollectors;
		std::vector<uint32> InstanceIds;
		std::vector<std::shared_ptr<FGPUSceneInstanceIdBuffer>> InstanceIdUploads;
	};

	struct FGPUSceneReadParameters
	{
		// Receiver plus four cascades: one attribute set, direct IDs and merged IDs each.
		std::array<std::optional<FRDGBufferAccess>, 25> Buffers;
		RENDERER_API static auto GetRDGParametersMetadata() -> const FRDGParametersMetadata*;
		RENDERER_API auto Add(FRDGBuilder& Graph, const FResolvedStaticMeshView& View) -> void;
	};
}
