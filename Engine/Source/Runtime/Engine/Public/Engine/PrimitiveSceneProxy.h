#pragma once

#include "EngineAPI.h"

namespace Durin
{
	class FRHICommandListBase;
	struct FStaticMeshRenderData;

	class PrimitiveSceneProxy
	{
	public:
		ENGINE_API virtual ~PrimitiveSceneProxy() = default;

		ENGINE_API auto SetTransform(FRHICommandListBase& RHICmdList, const FMatrix& InLocalToWorld, FVector3 InActorPosition) -> void;
		ENGINE_API auto GetLocalToWorld() const -> const FMatrix&;

	protected:
		FMatrix LocalToWorld_{1.0};

		FVector3 ActorPosition_;
	};

	class FStaticMeshSceneProxy : public PrimitiveSceneProxy
	{
	public:
		ENGINE_API explicit FStaticMeshSceneProxy(FStaticMeshRenderData* InRenderData);

		ENGINE_API auto GetRenderData() const -> FStaticMeshRenderData*;

	private:
		FStaticMeshRenderData* RenderData = nullptr;
	};
}
