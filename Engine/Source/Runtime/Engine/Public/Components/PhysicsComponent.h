#pragma once

#include "Components/ActorComponent.h"

#include "PhysicsComponent.gen.h"

namespace Durin
{
	// Lightweight rigid-body integration used until a full collision/physics backend is introduced.
	// It deliberately provides deterministic gravity and a ground plane instead of pretending to
	// support arbitrary mesh collision.
	DCLASS()
	class DPhysicsComponent : public DActorComponent
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DPhysicsComponent(const FObjectInitializer& ObjectInitializer);
		ENGINE_API auto TickComponent(float DeltaSeconds) -> void override;

		auto IsSimulatingPhysics() const -> bool { return bSimulatePhysics; }
		auto SetSimulatePhysics(bool bEnabled) -> void { bSimulatePhysics = bEnabled; }
		auto GetLinearVelocity() const -> const FVector3& { return LinearVelocity; }
		auto SetLinearVelocity(const FVector3& Velocity) -> void { LinearVelocity = Velocity; }

	private:
		DPROPERTY(Edit)
		bool bSimulatePhysics = true;

		DPROPERTY(Edit)
		FVector3 LinearVelocity{0.0};

		DPROPERTY(Edit)
		float GravityScale = 1.0f;

		DPROPERTY(Edit)
		float Restitution = 0.25f;

		DPROPERTY(Edit)
		float GroundHeight = 0.0f;
	};
}
