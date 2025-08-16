#pragma once

#include "Components/MeshComponent.h"

class DStaticMesh;

class DStaticMeshComponent : public DMeshComponent
{
public:
	ENGINE_API DStaticMeshComponent(AActor* OwnerActor);

	ENGINE_API auto GetDefaultName() const -> FStringName override { return "StaticMeshComponent"; }

private:

	TSharedPtr<DStaticMesh> StaticMesh_;
};