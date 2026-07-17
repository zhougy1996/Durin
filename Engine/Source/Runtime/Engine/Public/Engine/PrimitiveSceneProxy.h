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
		ENGINE_API explicit FStaticMeshSceneProxy(FStaticMeshRenderData* InRenderData, std::vector<FMaterialRenderUpdate> InMaterials);

		ENGINE_API auto GetRenderData() const -> FStaticMeshRenderData*;
		ENGINE_API auto GetMaterialRenderData(uint32 SlotIndex) const -> const FMaterialRenderData&;
		auto GetNumMaterials() const -> uint32 { return static_cast<uint32>(Materials.size()); }
		auto GetMaterialRenderData() const -> const FMaterialRenderData& { return GetMaterialRenderData(0); }
		auto GetMaterialComponentRevision() const -> uint64 { return MaterialComponentRevision; }
		auto GetMaterialVersion(uint32 SlotIndex = 0) const -> uint64 { return SlotIndex < MaterialVersions.size() ? MaterialVersions[SlotIndex] : 0; }
		auto GetLastMaterialDirtyFlags(uint32 SlotIndex = 0) const -> EMaterialRenderDirtyFlags { return SlotIndex < LastMaterialDirtyFlags.size() ? LastMaterialDirtyFlags[SlotIndex] : EMaterialRenderDirtyFlags::None; }
		ENGINE_API auto UpdateMaterialRenderData(const FMaterialRenderUpdate& Update) -> void;

	private:
		FStaticMeshRenderData* RenderData = nullptr;
		std::vector<FMaterialRenderData> Materials;
		std::vector<uint64> MaterialVersions;
		std::vector<EMaterialRenderDirtyFlags> LastMaterialDirtyFlags;
		uint64 MaterialComponentRevision = 0;
	};
}
