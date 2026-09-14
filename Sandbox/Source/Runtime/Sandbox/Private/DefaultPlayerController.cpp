#include "DefaultPlayerController.h"

#include "Input/InputCoreTypes.h"
#include "Misc/Paths.h"
#include "Logging/LogMacros.h"
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
		auto& Actions = GetInputActions();
		require(Actions.DefineAction("Move", EInputActionType::Axis2D));
		require(Actions.DefineAction("Look", EInputActionType::Axis2D));
		require(Actions.DefineAction("Jump", EInputActionType::Button));
		require(Actions.DefineAction("Interact", EInputActionType::Button));
		FInputMappingContext Gameplay;
		Gameplay.Name = "Sandbox.Gameplay";
		Gameplay.Bindings = {
			{"MoveForward", "Move", FInputSource::Key(EKey::W), {0.0, 1.0}},
			{"MoveBackward", "Move", FInputSource::Key(EKey::S), {0.0, -1.0}},
			{"MoveLeft", "Move", FInputSource::Key(EKey::A), {-1.0, 0.0}},
			{"MoveRight", "Move", FInputSource::Key(EKey::D), {1.0, 0.0}},
			{"LookX", "Look", {EInputSourceKind::MouseX}, {1.0, 0.0}},
			{"LookY", "Look", {EInputSourceKind::MouseY}, {0.0, 1.0}},
			{"Jump", "Jump", FInputSource::Key(EKey::Space)},
			{"Interact", "Interact", FInputSource::Key(EKey::E)}};
		require(Actions.AddContext(std::move(Gameplay)));
	}

	auto ADefaultPlayerController::GetControlBindingsPath() -> std::filesystem::path
	{
		return std::filesystem::path(FPaths::LaunchSavedDir()) / "Configs" / "SandboxInput.bindings";
	}

	auto ADefaultPlayerController::BeginPlay() -> void
	{
		const auto Path = GetControlBindingsPath();
		std::error_code FileError;
		if (std::filesystem::exists(Path, FileError))
		{
			std::string Error;
			auto Candidate = GetInputActions();
			bool bValid = Candidate.LoadOverrides(Path, Error);
			if (bValid)
				for (const std::string_view Slot : {"MoveForward", "MoveBackward", "MoveLeft", "MoveRight", "LookX", "LookY", "Jump", "Interact"})
					if (Candidate.GetBindingSource("Sandbox.Gameplay", Slot) == FInputSource::Key(EKey::Escape))
					{ bValid = false; Error = "Escape is reserved for releasing mouse capture."; break; }
			if (bValid) GetInputActions() = std::move(Candidate);
			else DURIN_WARN("Sandbox input overrides: {}", Error);
		}
		else if (FileError) DURIN_WARN("Sandbox input overrides: {}", FileError.message());
		Super::BeginPlay();
	}

	auto ADefaultPlayerController::RebindControl(std::string_view Slot, FInputSource Source, std::string& Error) -> bool
	{
		// Escape belongs to the host capture policy and never reaches action mapping.
		if (Source == FInputSource::Key(EKey::Escape)) { Error = "Escape is reserved for releasing mouse capture."; return false; }
		auto Candidate = GetInputActions();
		if (!Candidate.Rebind("Sandbox.Gameplay", Slot, Source, Error) || !Candidate.SaveOverrides(GetControlBindingsPath(), Error)) return false;
		GetInputActions() = std::move(Candidate);
		CancelPlayerInput();
		return true;
	}

	auto ADefaultPlayerController::ResetControlBindings(std::string& Error) -> bool
	{
		auto Candidate = GetInputActions();
		if (!Candidate.ResetBindings(Error) || !Candidate.SaveOverrides(GetControlBindingsPath(), Error)) return false;
		GetInputActions() = std::move(Candidate);
		CancelPlayerInput();
		return true;
	}

	auto ADefaultPlayerController::BuildControlIntent(const FInputActionSnapshot& Input) const -> FPawnControlIntent
	{
		FPawnControlIntent Intent;
		Intent.Move = GameplayTuning::DigitalMoveScale * Input.Get("Move").Value;
		const auto& Jump = Input.Get("Jump");
		Intent.bJumpHeld = Jump.bActive;
		Intent.bJumpPressed = Jump.bStarted;
		Intent.bJumpReleased = Jump.bCompleted || Jump.bCancelled;
		const FVector2d MouseDelta = SuppressMouseAxisCrosstalk(Input.Get("Look").Value);
		Intent.Look = {
			MouseDelta.x * GameplayTuning::MouseIntentPerPixel,
			-MouseDelta.y * GameplayTuning::MouseIntentPerPixel};
		return Intent;
	}
}
