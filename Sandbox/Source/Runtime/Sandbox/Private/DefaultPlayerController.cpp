#include "DefaultPlayerController.h"

#include "Input/GameInputState.h"
#include "Input/InputCoreTypes.h"
#include "SandboxGameplayTuning.h"

namespace Durin::Sandbox
{
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
		Intent.Look = {
			Input.GetMouseDelta().x * GameplayTuning::MouseIntentPerPixel,
			-Input.GetMouseDelta().y * GameplayTuning::MouseIntentPerPixel};
		return Intent;
	}
}
