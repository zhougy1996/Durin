#pragma once

#include "EngineAPI.h"
#include "Engine/Actor.h"

#include "StaticMeshActor.gen.h"

namespace Durin
{
	class DStaticMeshComponent;

	DCLASS(DisplayName = "Static Mesh Actor", DefaultObjectName = "StaticMeshActor")
	class AStaticMeshActor : public AActor
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit AStaticMeshActor(const FObjectInitializer& ObjectInitializer);
		ENGINE_API auto GetStaticMeshComponent() const -> DStaticMeshComponent*;

	private:
		DPROPERTY()
		TObjectPtr<DStaticMeshComponent> StaticMeshComponent;
	};
} // namespace Durin
