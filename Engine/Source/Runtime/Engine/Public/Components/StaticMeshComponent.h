#pragma once

#include "Components/MeshComponent.h"

class DStaticMesh;

class DStaticMeshComponent : public DMeshComponent
{
public:
	ENGINE_API DStaticMeshComponent(AActor* OwnerActor);

private:

	TSharedPtr<DStaticMesh> StaticMesh_;
};