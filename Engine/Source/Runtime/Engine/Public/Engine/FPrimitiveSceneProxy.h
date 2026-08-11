#pragma once

#include "EngineAPI.h"
#include "Animation/SkeletalAnimation.h"
#include "Math/Box.h"
#include "Materials/MaterialRenderProxy.h"
#include "RHIResources.h"

namespace Durin
{
	class FRHICommandListBase;
	struct FStaticMeshRenderData;
	struct FSkeletalMeshRenderData;

	// Stores renderer-owned state detached from the game-thread primitive component.
	enum class EPrimitiveSceneProxyKind : uint8
	{
		StaticMesh,
		SkeletalMesh
	};

	class FPrimitiveSceneProxy
	{
	public:
		ENGINE_API virtual ~FPrimitiveSceneProxy() = default;
		virtual auto GetKind() const -> EPrimitiveSceneProxyKind = 0;
		virtual auto GetLocalBounds() const -> FBox = 0;
		virtual auto UpdateMaterialBinding_RenderThread(
			const FMaterialRenderProxyBindingUpdate&) -> bool { return false; }
	};

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

	// Couples static-mesh render resources with revisioned per-slot material bindings.
	class FStaticMeshSceneProxy : public FPrimitiveSceneProxy
	{
	public:
		ENGINE_API explicit FStaticMeshSceneProxy(
			const FStaticMeshRenderData* InRenderData,
			std::vector<FMaterialRenderProxyRef> InMaterialProxies,
			uint64 InMaterialComponentRevision);

		ENGINE_API auto GetRenderData() const -> const FStaticMeshRenderData*;
		auto GetKind() const -> EPrimitiveSceneProxyKind override { return EPrimitiveSceneProxyKind::StaticMesh; }
		ENGINE_API auto GetLocalBounds() const -> FBox override;
		ENGINE_API auto ResolveMaterialRenderData_RenderThread(
			uint32 SlotIndex) const -> const FMaterialRenderData&;
		auto GetNumMaterials() const -> uint32 { return static_cast<uint32>(Materials.size()); }
		auto ResolveMaterialRenderData_RenderThread() const
			-> const FMaterialRenderData&
		{
			return ResolveMaterialRenderData_RenderThread(0);
		}
		auto GetMaterialComponentRevision() const -> uint64 { return MaterialComponentRevision; }
		ENGINE_API auto GetMaterialRenderProxy(uint32 SlotIndex) const
			-> const FMaterialRenderProxyRef&;
		ENGINE_API auto UpdateMaterialRenderProxyBinding(
			const FMaterialRenderProxyBindingUpdate& Update) -> void;
		ENGINE_API auto UpdateMaterialBinding_RenderThread(
			const FMaterialRenderProxyBindingUpdate& Update) -> bool override;

	private:
		// Non-owning borrow bounded by the component render-state lifetime. The
		// component removes this proxy before the asset retires the render data.
		const FStaticMeshRenderData* RenderData = nullptr;
		std::vector<FMaterialRenderProxyRef> Materials;
		uint64 MaterialComponentRevision = 0;
	};
}
