#pragma once

#include "Components/MeshComponent.h"

#include "StaticMeshComponent.gen.h"

namespace Doge
{
	class DStaticMesh;

	DCLASS()
	class DStaticMeshComponent : public DMeshComponent
	{
		GENERATED_BODY()
	public:

	private:

		TSharedPtr<DStaticMesh> StaticMesh;
	};
}