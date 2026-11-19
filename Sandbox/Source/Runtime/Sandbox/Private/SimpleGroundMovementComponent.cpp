#include "SimpleGroundMovementComponent.h"

#include "Components/ShapeComponent.h"
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
			if (Distance <= DoubleSmallNumber || Rate <= 0.0)
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

		auto FindFloor(const DCapsuleComponent& Capsule, FHitResult& OutHit) -> bool
		{
			return Capsule.SweepForBlockingHit({}, {0.0, 0.0, -GameplayTuning::FloorProbeDistance}, OutHit)
				&& OutHit.ImpactNormal.z >= GameplayTuning::WalkableFloorZ;
		}

		auto TryStep(DCapsuleComponent& Capsule, const FVector3& HorizontalDelta) -> bool
		{
			// Plan the full step first. A rejected ledge must never generate touch events.
			FHitResult Hit;
			const FVector3 Up(0.0, 0.0, GameplayTuning::MaximumStepHeight);
			if (Capsule.SweepForBlockingHit({}, Up, Hit)) return false;
			FVector3 Forward = HorizontalDelta;
			if (Capsule.SweepForBlockingHit(Up, Forward, Hit))
			{
				if (Hit.Time <= 0.0) return false;
				Forward *= Hit.Time;
			}
			const double DownDistance = GameplayTuning::MaximumStepHeight + GameplayTuning::FloorProbeDistance;
			if (!Capsule.SweepForBlockingHit(Up + Forward, {0.0, 0.0, -DownDistance}, Hit)
				|| Hit.ImpactNormal.z <= 0.0) return false;
			const FVector3 Down(0.0, 0.0, -DownDistance * Hit.Time + GameplayTuning::CollisionSkinWidth);
			const FQuat Rotation = Capsule.GetWorldRotation();
			Capsule.MoveComponent(Up, Rotation, true);
			Capsule.MoveComponent(Forward, Rotation, true);
			Capsule.MoveComponent(Down, Rotation, true);
			return true;
		}
	}

	DSimpleGroundMovementComponent::DSimpleGroundMovementComponent(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	auto DSimpleGroundMovementComponent::IsGrounded() const -> bool
	{
		const auto* Pawn = Cast<APlayerPawn>(GetPawnOwner());
		FHitResult Floor;
		return Pawn && Pawn->GetCapsuleComponent() && GetVelocity().z <= 0.0
			&& FindFloor(*Pawn->GetCapsuleComponent(), Floor);
	}

	auto DSimpleGroundMovementComponent::PerformMovement(
		const FPawnControlIntent& Intent,
		float DeltaSeconds) -> void
	{
		auto* Pawn = Cast<APlayerPawn>(GetPawnOwner());
		if (!Pawn || !Pawn->GetCapsuleComponent() || !Pawn->GetCapsuleComponent()->GetPhysicsWorld()
			|| !std::isfinite(DeltaSeconds) || DeltaSeconds <= 0.0f) return;
		const double StepSeconds = std::min(DeltaSeconds, GameplayTuning::MaximumDeltaSeconds);
		const bool bYawChanged = Pawn->ConsumeLookIntent(Intent.Look);

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
		const double Rate = Math::LengthSquared(DesiredHorizontal) > DoubleSmallNumber
			? GameplayTuning::HorizontalAcceleration
			: GameplayTuning::HorizontalDeceleration;
		const FHorizontalStep Horizontal = IntegrateHorizontal(
			CurrentHorizontal, DesiredHorizontal, Rate, StepSeconds);

		const bool bWasGrounded = IsGrounded();
		double VerticalDisplacement = 0.0;
		if (bWasGrounded && !Intent.bJumpPressed)
		{
			Velocity.z = 0.0;
		}
		else
		{
			if (bWasGrounded) Velocity.z = GameplayTuning::JumpImpulse;
			VerticalDisplacement = Velocity.z * StepSeconds
				+ 0.5 * GameplayTuning::Gravity * StepSeconds * StepSeconds;
			Velocity.z += GameplayTuning::Gravity * StepSeconds;
		}
		Velocity.x = Horizontal.Velocity.x;
		Velocity.y = Horizontal.Velocity.y;

		FVector3 Remaining(Horizontal.Displacement.x, Horizontal.Displacement.y, VerticalDisplacement);
		auto& Capsule = *Pawn->GetCapsuleComponent();
		const FQuat Rotation = bYawChanged
			? Math::MakeQuaternionFromAxisAngleDegrees(Pawn->GetYawDegrees(), FVectorConstants::Up)
			: Capsule.GetWorldRotation();
		if (bYawChanged) Capsule.MoveComponent({}, Rotation, false);
		for (uint32 SweepIndex = 0; SweepIndex < GameplayTuning::MaximumMovementSweeps; ++SweepIndex)
		{
			if (Math::LengthSquared(Remaining)
				<= GameplayTuning::MinimumMovementDistance * GameplayTuning::MinimumMovementDistance) break;
			FHitResult Hit;
			Capsule.MoveComponent(Remaining, Rotation, true, &Hit);
			if (!Hit.bBlockingHit) break;
			if (Hit.bStartPenetrating)
			{
				const double Recovery = std::min(
					Hit.PenetrationDepth + GameplayTuning::CollisionSkinWidth,
					GameplayTuning::MaximumStepHeight);
				if (Recovery <= GameplayTuning::MinimumMovementDistance) break;
				if (!Capsule.MoveComponent(Hit.ImpactNormal * Recovery, Rotation, true)) break;
				continue;
			}
			FVector3 Unconsumed = Remaining * (1.0 - Hit.Time);
			const bool bWall = Hit.ImpactNormal.z < GameplayTuning::WalkableFloorZ
				&& std::abs(Hit.ImpactNormal.z) < GameplayTuning::WalkableFloorZ;
			if (bWall && bWasGrounded)
			{
				const FVector3 HorizontalRemainder(Unconsumed.x, Unconsumed.y, 0.0);
				if (TryStep(Capsule, HorizontalRemainder))
				{
					Remaining = FVector3(0.0, 0.0, Unconsumed.z);
					continue;
				}
			}
			const double IntoSurface = Math::Dot(Unconsumed, Hit.ImpactNormal);
			if (IntoSurface < 0.0) Unconsumed -= Hit.ImpactNormal * IntoSurface;
			if (Hit.ImpactNormal.z >= GameplayTuning::WalkableFloorZ && Velocity.z < 0.0) Velocity.z = 0.0;
			if (Hit.ImpactNormal.z <= -GameplayTuning::WalkableFloorZ && Velocity.z > 0.0) Velocity.z = 0.0;
			Remaining = Unconsumed;
		}

		if (Velocity.z <= 0.0)
		{
			FHitResult Floor;
			if (FindFloor(Capsule, Floor))
			{
				const double Snap = GameplayTuning::CollisionSkinWidth - GameplayTuning::FloorProbeDistance * Floor.Time;
				Capsule.MoveComponent({0.0, 0.0, Snap}, Rotation, true);
				Velocity.z = 0.0;
			}
		}

		SetVelocity(Velocity);
	}
}
