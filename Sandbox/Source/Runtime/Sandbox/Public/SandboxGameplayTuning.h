#pragma once

#include "Math/Vector.h"

namespace Durin::Sandbox::GameplayTuning
{
	inline constexpr double DigitalMoveScale = 1.0;
	inline constexpr double MouseIntentPerPixel = 0.1;
	inline constexpr double LookDegreesPerIntent = 4.0;
	inline constexpr double MinimumPitchDegrees = -80.0;
	inline constexpr double MaximumPitchDegrees = 80.0;
	inline constexpr double GroundHeight = 0.0;
	inline constexpr double MaximumHorizontalSpeed = 6.0;
	inline constexpr double HorizontalAcceleration = 24.0;
	inline constexpr double HorizontalDeceleration = 32.0;
	inline constexpr double Gravity = -20.0;
	inline constexpr double JumpImpulse = 8.0;
	inline constexpr float MaximumDeltaSeconds = 0.05f;
	inline constexpr FVector3 VisualOffset{0.0, 0.0, 1.0};
	inline constexpr FVector3 VisualScale{0.5, 0.5, 1.0};
	inline constexpr FVector3 CameraOffset{-6.0, 0.0, 3.0};
	inline constexpr std::string_view GrayboxMeshPath = "/Game/Models/GrayboxPawn";
}
