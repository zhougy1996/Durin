#pragma once

#include "CoreMinimal.h"
#include "RendererAPI.h"
#include "GPUSceneParameters.h"
#include "RDG/RDG.h"

namespace Durin
{
	class FGPUScene;
	struct FPreparedStaticMeshView;
	struct FResolvedStaticMeshView;
	struct FGPUSceneBufferSet
	{
		std::array<FBufferRHIRef, 3> SceneData;
		FBufferRHIRef InstanceIds;
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
		RENDERER_API static auto UploadInstanceIds(FRHICommandListImmediate& Commands, std::span<const uint32> Ids) -> FBufferRHIRef;
		RENDERER_API static auto Prepare(FRHICommandListImmediate& Commands,
			const FPreparedStaticMeshView& Prepared, FGPUScene* Scene = nullptr)
			-> std::vector<FGPUScenePrimitiveData>;
	};

	struct FGPUSceneReadParameters
	{
		// Receiver plus four cascades: two scene-data sets and one merged ID list each.
		std::array<std::optional<FRDGBufferAccess>, 45> Buffers;
		RENDERER_API static auto GetRDGParametersMetadata() -> const FRDGParametersMetadata*;
		RENDERER_API auto Add(FRDGBuilder& Graph, const FResolvedStaticMeshView& View) -> void;
	};
}
