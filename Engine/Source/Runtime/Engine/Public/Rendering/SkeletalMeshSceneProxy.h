#pragma once

#include "Animation/SkeletalAnimation.h"
#include "Rendering/PrimitiveSceneProxy.h"

namespace Durin
{
	struct FSkeletalMeshRenderData;

	// Retains skeletal render resources and the latest immutable pose for render-thread use.
	class FSkeletalMeshSceneProxy final : public FPrimitiveSceneProxy
	{
	public:
		ENGINE_API FSkeletalMeshSceneProxy(
			const FSkeletalMeshRenderData* InRenderData,
			std::vector<FMaterialRenderProxyRef> InMaterialProxies,
			uint64 InMaterialComponentRevision,
			std::shared_ptr<const FSkeletalPosePalette> InPose);
		auto GetKind() const -> EPrimitiveSceneProxyKind override
		{
			return EPrimitiveSceneProxyKind::SkeletalMesh;
		}
		auto GetRenderData() const -> const FSkeletalMeshRenderData* { return RenderData; }
		ENGINE_API auto GetLocalBounds() const -> FBox override;
		auto GetPose() const -> const std::shared_ptr<const FSkeletalPosePalette>& { return Pose; }
		ENGINE_API auto ResolveMaterialRenderData_RenderThread(uint32 SlotIndex) const
			-> const FMaterialRenderData&;
		ENGINE_API auto GetMaterialRenderProxy(uint32 SlotIndex) const
			-> const FMaterialRenderProxyRef&;
		ENGINE_API auto UpdateMaterialBinding_RenderThread(
			const FMaterialRenderProxyBindingUpdate& Update) -> bool override;
		ENGINE_API auto UpdateDynamicData_RenderThread(
			std::shared_ptr<const FSkeletalPosePalette> InPose) -> bool;

	private:
		const FSkeletalMeshRenderData* RenderData = nullptr;
		std::vector<FMaterialRenderProxyRef> Materials;
		uint64 MaterialComponentRevision = 0;
		std::shared_ptr<const FSkeletalPosePalette> Pose;
	};
}
