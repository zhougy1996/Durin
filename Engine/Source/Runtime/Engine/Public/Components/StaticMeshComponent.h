#pragma once

#include "Components/MeshComponent.h"

#include "StaticMeshComponent.gen.h"

namespace Durin
{
	class DStaticMesh;
	class DMaterialInterface;

	DCLASS()
	class DStaticMeshComponent : public DMeshComponent
	{
		GENERATED_BODY()
	public:
		ENGINE_API auto SetStaticMesh(DStaticMesh* InStaticMesh) -> void;
		ENGINE_API auto GetStaticMesh() const -> DStaticMesh*;
		ENGINE_API auto SetMaterial(DMaterialInterface* InMaterial) -> void;
		ENGINE_API auto GetMaterial() const -> DMaterialInterface*;
		ENGINE_API auto SetMaterial(uint32 SlotIndex, DMaterialInterface* InMaterial) -> void;
		ENGINE_API auto GetMaterial(uint32 SlotIndex) const -> DMaterialInterface*;
		ENGINE_API auto GetNumMaterials() const -> uint32;
		ENGINE_API auto CreateSceneProxy() -> std::unique_ptr<PrimitiveSceneProxy> override;
		ENGINE_API auto PostLoad(std::string& OutError) -> bool override;

	private:

		DPROPERTY(Edit)
		TObjectPtr<DStaticMesh> StaticMesh;

		// Retained as a serialized slot-zero mirror so existing component assets keep their material.
		DPROPERTY()
		TObjectPtr<DMaterialInterface> Material;

		DPROPERTY(Edit)
		std::vector<TObjectPtr<DMaterialInterface>> Materials;
	};
}
