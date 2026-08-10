#pragma once

#include "Actors/Controller.h"

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

	protected:
		// Derived player controllers are the only gameplay types that translate raw device identities.
		ENGINE_API virtual auto BuildControlIntent(const FGameInputState& Input) const -> FPawnControlIntent;
		ENGINE_API auto OnPossessedPawnChanged(APawn* PreviousPawn, APawn* NewPawn) -> void override;
		ENGINE_API auto EndPlay() -> void override;
		ENGINE_API auto OnActorDestroyed() -> void override;

	private:
		auto PreparePlayerInput(const FGameInputState& Input) -> void;
		auto HandleViewTargetDestroyed(AActor* Target) -> void;

		DPROPERTY(Transient)
		TObjectPtr<AActor> ViewTarget;

		friend class DWorld;
	};
}
