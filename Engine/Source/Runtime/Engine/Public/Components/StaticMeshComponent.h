#pragma once

#include "CoreMinimal.h"

#include "Components/MeshComponent.h"

#include "StaticMeshComponent.gen.h"

namespace Durin
{
	class DStaticMesh;
	class DMaterialInterface;
	class DBodySetup;

	// Binds a static mesh and per-slot materials to a render-scene primitive.
	DCLASS()
	class DStaticMeshComponent : public DMeshComponent
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DStaticMeshComponent(const FObjectInitializer& ObjectInitializer);
		ENGINE_API auto SetStaticMesh(DStaticMesh* InStaticMesh) -> void;
		ENGINE_API auto GetStaticMesh() const -> DStaticMesh*;
		ENGINE_API auto SetRenderInstances(std::span<const FStaticMeshRenderInstance> Instances) -> bool;
		ENGINE_API auto SetInstanceCustomData(uint32 InstanceId, uint32 First, std::span<const float> Values) -> bool;
		ENGINE_API auto RemoveRenderInstance(uint32 InstanceId) -> bool;
		ENGINE_API auto ClearRenderInstances() -> void;
		auto GetRenderInstances() const -> FStaticMeshRenderInstances { return RenderInstances; }
		// Refreshes render and physics consumers after transactional reference replacement.
		ENGINE_API auto RefreshReloadedAssetBindings() -> void;
		ENGINE_API auto GetBodySetup() const -> DBodySetup*;
		ENGINE_API auto BuildCollisionShape(FCollisionShape& OutShape, FTransform& OutWorldTransform) const -> bool override;
		ENGINE_API auto BuildCollisionGeometry(
			FCollisionGeometryRef& OutGeometry, FTransform& OutWorldTransform) const -> bool override;
		ENGINE_API auto GetNumMaterials() const -> uint32 override;
		ENGINE_API auto GetMaterialIndex(FName SlotName) const -> std::optional<uint32> override;
		ENGINE_API auto GetDefaultMaterial(uint32 SlotIndex) const -> DMaterialInterface* override;
		ENGINE_API auto CreateSceneProxy() -> std::unique_ptr<FPrimitiveSceneProxy> override;
		ENGINE_API auto OnRegister() -> void override;
		ENGINE_API auto PreEditChangeProperty(FPropertyEditProposal& Proposal) -> std::expected<void, FObjectValidationError> override;
		ENGINE_API auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;

	private:
		friend class FStaticMeshRenderStateRecreateContext;

		ENGINE_API auto GetCollisionStateRevision() const -> uint64 override;
		auto HandleStaticMeshRenderDataChanged(DStaticMesh* ChangedMesh) -> void;
		auto HandleStaticMeshCollisionDataChanged(DStaticMesh* ChangedMesh) -> void;

		DPROPERTY(Category = "Static Mesh", Edit)
		TObjectPtr<DStaticMesh> StaticMesh;
		FStaticMeshRenderInstances RenderInstances;

		friend class DStaticMesh;
	};
}
