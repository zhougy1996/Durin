#include "Components/PhysicsComponent.h"

#include "Components/SceneComponent.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Engine/World.h"

namespace Durin
{
	namespace
	{
		constexpr FReal GravityAcceleration = -9.81;
	}

	DPhysicsComponent::DPhysicsComponent(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		SetComponentTickGroup(ETickingGroup::Physics);
		SetComponentTickEnabled(true);
	}

	auto DPhysicsComponent::TickComponent(float DeltaSeconds) -> void
	{
		Super::TickComponent(DeltaSeconds);
		AActor* Owner = GetOwner();
		DSceneComponent* Root = Owner ? Owner->GetRootComponent() : nullptr;
		DLevel* Level = Owner ? Cast<DLevel>(Owner->GetOuter()) : nullptr;
		DWorld* World = Level ? Level->GetWorld() : nullptr;
		if (!bSimulatePhysics || !Root || !World || !World->IsPhysicsSimulationEnabled() || DeltaSeconds <= 0.0f) return;

		LinearVelocity.z += GravityAcceleration * static_cast<FReal>(GravityScale * DeltaSeconds);
		FVector3 Location = Root->GetWorldLocation() + LinearVelocity * static_cast<FReal>(DeltaSeconds);
		if (Location.z < GroundHeight)
		{
			Location.z = GroundHeight;
			LinearVelocity.z = std::abs(LinearVelocity.z) > 0.05
				? -LinearVelocity.z * static_cast<FReal>(std::clamp(Restitution, 0.0f, 1.0f))
				: 0.0;
		}
		Root->SetWorldLocation(Location);
	}
}
