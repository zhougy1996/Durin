#pragma once

#include "Components/MeshComponent.h"

#include "StaticMeshComponent.gen.h"

namespace Durin
{
	class DStaticMesh;

	DCLASS()
	class DStaticMeshComponent : public DMeshComponent
	{
		GENERATED_BODY()
	public:
		ENGINE_API auto SetStaticMesh(DStaticMesh* InStaticMesh) -> void;
		ENGINE_API auto GetStaticMesh() const -> DStaticMesh*;
		ENGINE_API auto CreateSceneProxy() -> std::unique_ptr<PrimitiveSceneProxy> override;

	private:

		DPROPERTY(Edit)
		TObjectPtr<DStaticMesh> StaticMesh;
	};
}
