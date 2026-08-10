#pragma once

#include "EngineAPI.h"
#include "Engine/Actor.h"

#include "SkeletalMeshActor.gen.h"

namespace Durin
{
	class DSkeletalMeshComponent;

	// Provides an actor-owned skeletal mesh component for animated level geometry.
	DCLASS(DisplayName = "Skeletal Mesh Actor", DefaultObjectName = "SkeletalMeshActor")
	class ASkeletalMeshActor : public AActor
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit ASkeletalMeshActor(const FObjectInitializer& ObjectInitializer);
		ENGINE_API auto GetSkeletalMeshComponent() const -> DSkeletalMeshComponent*;

	private:
		DPROPERTY()
		TObjectPtr<DSkeletalMeshComponent> SkeletalMeshComponent;
	};
} // namespace Durin
