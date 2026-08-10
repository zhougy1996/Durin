#include "Actors/SkeletalMeshActor.h"

#include "Components/SkeletalMeshComponent.h"

namespace Durin
{
	ASkeletalMeshActor::ASkeletalMeshActor(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		SkeletalMeshComponent = CreateDefaultComponent<DSkeletalMeshComponent>("DSkeletalMeshComponent");
		SetRootComponent(SkeletalMeshComponent);
	}

	auto ASkeletalMeshActor::GetSkeletalMeshComponent() const -> DSkeletalMeshComponent*
	{
		return SkeletalMeshComponent.Get();
	}
} // namespace Durin
