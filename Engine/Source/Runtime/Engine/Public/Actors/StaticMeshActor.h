#pragma once

#include "EngineAPI.h"
#include "Engine/Actor.h"

#include "StaticMeshActor.gen.h"

namespace Durin
{
	class DStaticMeshComponent;

	DCLASS()
	class AStaticMeshActor : public AActor
	{
		GENERATED_BODY()
	public:
		ENGINE_API AStaticMeshActor();
		ENGINE_API auto GetStaticMeshComponent() const -> DStaticMeshComponent*;
	private:

		DPROPERTY()
		TObjectPtr<DStaticMeshComponent> StaticMeshComponent;
	};
}
