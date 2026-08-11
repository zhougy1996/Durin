#include "DefaultPlayerController.h"

#include "Input/GameInputState.h"
#include "Input/InputCoreTypes.h"
#include "SandboxGameplayTuning.h"

namespace Durin::Sandbox
{
	namespace
	{
		auto SuppressMouseAxisCrosstalk(FVector2d Delta) -> FVector2d
		{
			const double AbsoluteX = std::abs(Delta.x);
			const double AbsoluteY = std::abs(Delta.y);
			if (AbsoluteX >= GameplayTuning::MouseDominantAxisThresholdCounts
				&& AbsoluteY <= GameplayTuning::MouseMinorAxisNoiseCounts)
			{
				Delta.y = 0.0;
			}
			else if (AbsoluteY >= GameplayTuning::MouseDominantAxisThresholdCounts
				&& AbsoluteX <= GameplayTuning::MouseMinorAxisNoiseCounts)
			{
				Delta.x = 0.0;
			}
			return Delta;
		}
	}

	ADefaultPlayerController::ADefaultPlayerController(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	auto ADefaultPlayerController::BuildControlIntent(const FGameInputState& Input) const -> FPawnControlIntent
	{
		FPawnControlIntent Intent;
		Intent.Move.x = GameplayTuning::DigitalMoveScale * (
			static_cast<double>(Input.IsKeyDown(EKey::D)) - static_cast<double>(Input.IsKeyDown(EKey::A)));
		Intent.Move.y = GameplayTuning::DigitalMoveScale * (
			static_cast<double>(Input.IsKeyDown(EKey::W)) - static_cast<double>(Input.IsKeyDown(EKey::S)));
		Intent.bJumpHeld = Input.IsKeyDown(EKey::Space);
		Intent.bJumpPressed = Input.WasKeyPressed(EKey::Space);
		Intent.bJumpReleased = Input.WasKeyReleased(EKey::Space);
		const FVector2d MouseDelta = SuppressMouseAxisCrosstalk(Input.GetMouseDelta());
		Intent.Look = {
			MouseDelta.x * GameplayTuning::MouseIntentPerPixel,
			-MouseDelta.y * GameplayTuning::MouseIntentPerPixel};
		return Intent;
	}
}
