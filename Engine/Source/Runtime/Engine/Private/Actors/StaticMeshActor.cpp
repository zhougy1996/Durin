#include "Actors/StaticMeshActor.h"

#include "Components/StaticMeshComponent.h"

namespace Durin
{
	AStaticMeshActor::AStaticMeshActor(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		StaticMeshComponent = CreateDefaultComponent<DStaticMeshComponent>("DStaticMeshComponent");
		StaticMeshComponent->SetPhysicsBodyMotionType(EPhysicsBodyMotionType::Static);
		verify(StaticMeshComponent->SetCollisionProfileName(CollisionProfile::WorldStatic));
		SetRootComponent(StaticMeshComponent);
	}

	auto AStaticMeshActor::GetStaticMeshComponent() const -> DStaticMeshComponent*
	{
		return StaticMeshComponent.Get();
	}
} // namespace Durin
