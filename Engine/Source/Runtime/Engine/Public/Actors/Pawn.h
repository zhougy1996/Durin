#pragma once

#include "Engine/Actor.h"
#include "Gameplay/PawnControlIntent.h"

#include "Pawn.gen.h"

namespace Durin
{
	class AController;
	class DPawnMovementComponent;

	// Defines a possessable actor that consumes at most one semantic control sample per tick.
	DCLASS()
	class APawn : public AActor
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit APawn(const FObjectInitializer& ObjectInitializer);
		ENGINE_API auto Tick(float DeltaSeconds) -> void override;
		ENGINE_API auto EndPlay() -> void override;

		auto GetController() const -> AController* { return Controller.Get(); }
		auto GetMovementComponent() const -> DPawnMovementComponent* { return MovementComponent.Get(); }
		auto HasPendingControlIntent() const -> bool { return bHasPendingControlIntent; }

	protected:
		// Selects the single pawn-owned component allowed to perform semantic movement updates.
		ENGINE_API auto SetMovementComponent(DPawnMovementComponent* Component) -> bool;
		ENGINE_API auto OnActorDestroyed() -> void override;

	private:
		auto AdmitControlIntent(const FPawnControlIntent& Intent) -> bool;
		auto ClearPendingControlIntent() -> void;

		DPROPERTY(Transient)
		TObjectPtr<DPawnMovementComponent> MovementComponent;

		DPROPERTY(Transient)
		TObjectPtr<AController> Controller;

		FPawnControlIntent PendingControlIntent;
		bool bHasPendingControlIntent = false;

		friend class AController;
		friend class DWorld;
	};
}
