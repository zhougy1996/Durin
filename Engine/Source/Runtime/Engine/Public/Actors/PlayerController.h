#pragma once

#include "Actors/Controller.h"
#include "Input/InputActions.h"

#include "PlayerController.gen.h"

namespace Durin
{
	class FGameInputState;

	// Categorizes controller view-target assignment failures.
	enum class EViewTargetError : uint8
	{
		None,
		ControllerUnavailable,
		TargetUnavailable,
		InvalidMembership,
		WorldEnding
	};

	struct FViewTargetResult
	{
		EViewTargetError Error = EViewTargetError::None;
		std::string Message;

		explicit operator bool() const { return Error == EViewTargetError::None; }
	};

	// Translates raw local-player input and owns the local controller's transient camera target.
	DCLASS()
	class APlayerController : public AController
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit APlayerController(const FObjectInitializer& ObjectInitializer);
		ENGINE_API auto SetViewTarget(AActor* Target) -> FViewTargetResult;
		auto GetViewTarget() const -> AActor* { return ViewTarget.Get(); }
		auto GetInputActions() -> FInputActionEvaluator& { return InputActions; }
		auto GetInputActions() const -> const FInputActionEvaluator& { return InputActions; }
		// Host and World lifecycle boundaries cancel immediately, including while paused.
		ENGINE_API auto CancelPlayerInput() -> void;

	protected:
		// Derived controllers translate logical actions into source-neutral Pawn intent.
		ENGINE_API virtual auto BuildControlIntent(const FInputActionSnapshot& Input) const -> FPawnControlIntent;
		ENGINE_API auto OnPossessedPawnChanged(APawn* PreviousPawn, APawn* NewPawn) -> void override;
		ENGINE_API auto EndPlay() -> void override;
		ENGINE_API auto OnActorDestroyed() -> void override;

	private:
		FInputActionEvaluator InputActions;
		auto PreparePlayerInput(const FGameInputState& Input) -> void;
		auto HandleViewTargetDestroyed(AActor* Target) -> void;

		DPROPERTY(Transient)
		TObjectPtr<AActor> ViewTarget;

		friend class DWorld;
	};
}
