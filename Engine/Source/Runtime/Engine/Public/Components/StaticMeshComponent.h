#pragma once

#include "Components/MeshComponent.h"

class DStaticMeshComponent : public DMeshComponent
{
public:
	ENGINE_API DStaticMeshComponent(AActor* OwnerActor);

	ENGINE_API auto GetDefaultName() const -> FName override { return "StaticMeshComponent"; }
};