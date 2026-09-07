#pragma once

#include "Rendering/SplineMeshSceneProxy.h"
#include "VertexFactory.h"

namespace Durin
{
	// Retains vertex input resources independently of the source asset containers.
	class FStaticMeshBatchBinding : public FVertexFactoryInputBinding
	{
	public:
		auto GetFactoryKey() const -> FXxHash64 override
		{
			return FXxHash64::HashBuffer("LocalVertexFactory");
		}
		auto GetLayoutKey() const -> FXxHash64 override
		{
			return FXxHash64::HashBuffer("LocalMeshBinding.v1");
		}

	};

	class FSplineMeshBatchBinding final : public FStaticMeshBatchBinding
	{
	public:
		auto GetFactoryKey() const -> FXxHash64 override
		{
			return FXxHash64::HashBuffer("SplineVertexFactory");
		}
		auto GetLayoutKey() const -> FXxHash64 override
		{
			return FXxHash64::HashBuffer("SplineMeshBinding.v1");
		}
		FSplineMeshRenderDynamicData DynamicData;
		uint64 AcceptedDynamicUpdates = 0;
	};
}
