#pragma once

#include "EngineAPI.h"
#include "Materials/MaterialTypes.h"

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
		ENGINE_API explicit FStaticMeshSceneProxy(FStaticMeshRenderData* InRenderData, std::vector<FMaterialRenderData> InMaterials);

		ENGINE_API auto GetRenderData() const -> FStaticMeshRenderData*;
		ENGINE_API auto GetMaterialRenderData(uint32 SlotIndex) const -> const FMaterialRenderData&;
		auto GetNumMaterials() const -> uint32 { return static_cast<uint32>(Materials.size()); }

	private:
		FStaticMeshRenderData* RenderData = nullptr;
		std::vector<FMaterialRenderData> Materials;
	};
}
