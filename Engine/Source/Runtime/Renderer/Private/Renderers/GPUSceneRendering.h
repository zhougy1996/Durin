#pragma once

#include "CoreMinimal.h"
#include "RendererAPI.h"
#include "GPUSceneParameters.h"
#include "GPUScene.h"
#include "RDG/RDG.h"

namespace Durin
{
	class FGPUSceneDynamicContext;
	struct FPreparedStaticMeshView;
	struct FResolvedStaticMeshView;
	struct FGPUSceneBufferSet
	{
		std::array<FBufferRHIRef, 3> SceneData;
		FBufferRHIRef InstanceIds;
		std::shared_ptr<FGPUSceneDynamicContext> DynamicContext;
		bool bStandaloneScene = false;
		RENDERER_API auto GetShaderParameters() const -> FGPUSceneShaderParameters;
	};
	struct FGPUScenePrimitiveData
	{
		std::shared_ptr<const FGPUSceneBufferSet> Buffers;
		uint32 InstanceId = INDEX_NONE_U32;
		uint32 InstanceIdOffset = 0;
	};
	// A submission-scoped upload owner for dynamic/unpublished mesh facts. Scene
	// members reuse persistent attributes; ID lists belong to this submission.
	class FGPUSceneDynamicContext
	{
	public:
		RENDERER_API explicit FGPUSceneDynamicContext(std::shared_ptr<FGPUScene> InGPUScene);
		RENDERER_API ~FGPUSceneDynamicContext();
		FGPUSceneDynamicContext(const FGPUSceneDynamicContext&) = delete;
		auto operator=(const FGPUSceneDynamicContext&) -> FGPUSceneDynamicContext& = delete;
		RENDERER_API auto RegisterGraphUse(const FRDGBuilder& Graph) -> void;
		RENDERER_API static auto UploadInstanceIds(FRHICommandListImmediate& Commands, std::span<const uint32> Ids) -> FBufferRHIRef;
		RENDERER_API static auto Prepare(FRHICommandListImmediate& Commands, FRDGAllocator& Allocator,
			const FPreparedStaticMeshView& Prepared, FGPUScene* Scene = nullptr)
			-> std::vector<FGPUScenePrimitiveData>;
	private:
		std::shared_ptr<FGPUScene> GPUScene;
		std::vector<FGPUSceneDynamicAllocation> Allocations;
	};
	RENDERER_API auto RegisterGPUSceneGraphUse(const FRDGBuilder& Graph, const FResolvedStaticMeshView& View) -> void;

	struct FGPUSceneReadParameters
	{
		// Receiver plus four cascades: one attribute set, direct IDs and merged IDs each.
		std::array<std::optional<FRDGBufferAccess>, 25> Buffers;
		RENDERER_API static auto GetRDGParametersMetadata() -> const FRDGParametersMetadata*;
		RENDERER_API auto Add(FRDGBuilder& Graph, const FResolvedStaticMeshView& View) -> void;
	};
}
