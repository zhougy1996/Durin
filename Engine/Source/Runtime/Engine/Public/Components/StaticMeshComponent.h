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
		ENGINE_API auto SetStaticMesh(std::shared_ptr<DStaticMesh> InStaticMesh) -> void;
		ENGINE_API auto GetStaticMesh() const -> const std::shared_ptr<DStaticMesh>&;
		ENGINE_API auto CreateSceneProxy() -> std::unique_ptr<PrimitiveSceneProxy> override;

	private:

		std::shared_ptr<DStaticMesh> StaticMesh;
	};
}
