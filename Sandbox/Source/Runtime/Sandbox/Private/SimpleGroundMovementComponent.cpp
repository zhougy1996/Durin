#include "SimpleGroundMovementComponent.h"

#include "Math/Operations.h"
#include "PlayerPawn.h"
#include "SandboxGameplayTuning.h"

namespace Durin::Sandbox
{
	namespace
	{
		struct FHorizontalStep
		{
			FVector2 Velocity{0.0};
			FVector2 Displacement{0.0};
		};

		auto IntegrateHorizontal(
			const FVector2& Current,
			const FVector2& Desired,
			double Rate,
			double DeltaSeconds) -> FHorizontalStep
		{
			const FVector2 Difference = Desired - Current;
			const double Distance = Math::Length(Difference);
			if (Distance <= kDoubleSmallNumber || Rate <= 0.0)
				return {.Velocity = Desired, .Displacement = Desired * DeltaSeconds};
			const FVector2 Direction = Difference / Distance;
			const double AccelerationTime = std::min(DeltaSeconds, Distance / Rate);
			const FVector2 ReachedVelocity = Current + Direction * Rate * AccelerationTime;
			return {
				.Velocity = AccelerationTime < DeltaSeconds ? Desired : ReachedVelocity,
				.Displacement = Current * AccelerationTime
					+ Direction * (0.5 * Rate * AccelerationTime * AccelerationTime)
					+ Desired * (DeltaSeconds - AccelerationTime)};
		}
	}

	DSimpleGroundMovementComponent::DSimpleGroundMovementComponent(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	auto DSimpleGroundMovementComponent::IsGrounded() const -> bool
	{
		const APawn* Pawn = GetPawnOwner();
		return Pawn && Pawn->GetActorTransform().Translation.z <= GameplayTuning::GroundHeight + kDoubleDelta
			&& GetVelocity().z <= 0.0;
	}

	auto DSimpleGroundMovementComponent::PerformMovement(
		const FPawnControlIntent& Intent,
		float DeltaSeconds) -> void
	{
		auto* Pawn = Cast<APlayerPawn>(GetPawnOwner());
		if (!Pawn || !std::isfinite(DeltaSeconds) || DeltaSeconds <= 0.0f) return;
		const double StepSeconds = std::min(DeltaSeconds, GameplayTuning::MaximumDeltaSeconds);
		Pawn->ApplyLookIntent(Intent.Look);

		FVector2 MoveInput = Intent.Move;
		const double InputLength = Math::Length(MoveInput);
		if (InputLength > 1.0) MoveInput /= InputLength;
		const double YawRadians = Math::DegreesToRadians(Pawn->GetYawDegrees());
		const FVector2 Forward{std::cos(YawRadians), std::sin(YawRadians)};
		const FVector2 Right{-Forward.y, Forward.x};
		const FVector2 DesiredHorizontal =
			(Forward * MoveInput.y + Right * MoveInput.x) * GameplayTuning::MaximumHorizontalSpeed;

		FVector3 Velocity = GetVelocity();
		const FVector2 CurrentHorizontal{Velocity.x, Velocity.y};
		const double Rate = Math::LengthSquared(DesiredHorizontal) > kDoubleSmallNumber
			? GameplayTuning::HorizontalAcceleration
			: GameplayTuning::HorizontalDeceleration;
		const FHorizontalStep Horizontal = IntegrateHorizontal(
			CurrentHorizontal, DesiredHorizontal, Rate, StepSeconds);

		FTransform Transform = Pawn->GetActorTransform();
		Transform.Translation.x += Horizontal.Displacement.x;
		Transform.Translation.y += Horizontal.Displacement.y;
		const bool bWasGrounded = IsGrounded();
		if (bWasGrounded)
		{
			Transform.Translation.z = GameplayTuning::GroundHeight;
			Velocity.z = Intent.bJumpPressed ? GameplayTuning::JumpImpulse : 0.0;
		}
		Transform.Translation.z += Velocity.z * StepSeconds
			+ 0.5 * GameplayTuning::Gravity * StepSeconds * StepSeconds;
		Velocity.z += GameplayTuning::Gravity * StepSeconds;
		if (Transform.Translation.z <= GameplayTuning::GroundHeight)
		{
			Transform.Translation.z = GameplayTuning::GroundHeight;
			Velocity.z = 0.0;
		}
		Velocity.x = Horizontal.Velocity.x;
		Velocity.y = Horizontal.Velocity.y;
		SetVelocity(Velocity);
		verify(Pawn->SetActorTransform(Transform));
	}
}
