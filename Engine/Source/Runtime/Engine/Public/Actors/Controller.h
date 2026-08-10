#pragma once

#include "Engine/Actor.h"
#include "Gameplay/PawnControlIntent.h"

#include "Controller.gen.h"

namespace Durin
{
	class APawn;

	// Categorizes every observable possession rejection without relying on log output.
	enum class EPossessionError : uint8
	{
		None,
		NullPawn,
		ControllerUnavailable,
		PawnUnavailable,
		InvalidMembership,
		WorldEnding
	};

	struct FPossessionResult
	{
		EPossessionError Error = EPossessionError::None;
		std::string Message;

		explicit operator bool() const { return Error == EPossessionError::None; }
	};

	// Owns the authoritative mutation boundary for reciprocal controller/pawn possession.
	DCLASS()
	class AController : public AActor
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit AController(const FObjectInitializer& ObjectInitializer);
		ENGINE_API auto Possess(APawn* Pawn) -> FPossessionResult;
		ENGINE_API auto UnPossess() -> void;
		ENGINE_API auto EndPlay() -> void override;

		auto GetPawn() const -> APawn* { return Pawn.Get(); }

	protected:
		// Submits the same bounded semantic value regardless of whether the producer is input, AI, or replay.
		ENGINE_API auto SubmitControlIntent(const FPawnControlIntent& Intent) -> bool;
		ENGINE_API virtual auto OnPossessedPawnChanged(APawn* PreviousPawn, APawn* NewPawn) -> void;
		ENGINE_API auto OnActorDestroyed() -> void override;

	private:
		static auto DetachPair(AController* Controller, APawn* Pawn) -> void;

		DPROPERTY(Transient)
		TObjectPtr<APawn> Pawn;

		friend class APawn;
	};
}
