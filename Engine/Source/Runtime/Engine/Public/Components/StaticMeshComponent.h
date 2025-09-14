#pragma once

#include "Components/MeshComponent.h"

class DStaticMesh;

class DStaticMeshComponent : public DMeshComponent
{
public:

private:

	TSharedPtr<DStaticMesh> StaticMesh_;
};