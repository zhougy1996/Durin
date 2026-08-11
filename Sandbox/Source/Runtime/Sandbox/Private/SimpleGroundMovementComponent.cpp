#include "SimpleGroundMovementComponent.h"

#include "Components/ShapeComponent.h"
#include "Engine/Level.h"
#include "Engine/World.h"
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

		auto GetPawnWorld(const APlayerPawn& Pawn) -> DWorld*
		{
			auto* Level = Cast<DLevel>(Pawn.GetOuter());
			return Level ? Level->GetWorld() : nullptr;
		}

		auto MakePawnQuery(const APlayerPawn& Pawn) -> FCollisionQueryParams
		{
			FCollisionQueryParams Params;
			Params.AddIgnoredActor(&Pawn);
			return Params;
		}

		auto SweepPawn(
			const APlayerPawn& Pawn,
			const FVector3& RootOffset,
			const FVector3& Delta,
			FHitResult& OutHit) -> bool
		{
			DWorld* World = GetPawnWorld(Pawn);
			DCapsuleComponent* Capsule = Pawn.GetCapsuleComponent();
			if (!World || !Capsule) return false;
			FCollisionShape Shape;
			FTransform Transform;
			if (!Capsule->BuildCollisionShape(Shape, Transform)) return false;
			Transform.Translation += RootOffset;
			return World->SweepSingleByChannel(
				OutHit, Shape, Transform, Delta, ECollisionChannel::Pawn, MakePawnQuery(Pawn));
		}

		auto FindFloor(const APlayerPawn& Pawn, const FVector3& RootOffset, double Distance, FHitResult& OutHit) -> bool
		{
			return SweepPawn(Pawn, RootOffset, FVector3(0.0, 0.0, -Distance), OutHit)
				&& OutHit.ImpactNormal.z >= GameplayTuning::WalkableFloorZ;
		}

		auto TryStep(
			const APlayerPawn& Pawn,
			const FVector3& CurrentOffset,
			const FVector3& HorizontalDelta,
			FVector3& OutOffset) -> bool
		{
			FHitResult Hit;
			const FVector3 Up(0.0, 0.0, GameplayTuning::MaximumStepHeight);
			if (SweepPawn(Pawn, CurrentOffset, Up, Hit)
				&& (!Hit.bStartPenetrating
					|| Hit.PenetrationDepth > GameplayTuning::CollisionSkinWidth * 2.0)) return false;
			const FVector3 RaisedOffset = CurrentOffset + Up;
			FVector3 ForwardOffset = RaisedOffset;
			if (SweepPawn(Pawn, RaisedOffset, HorizontalDelta, Hit))
			{
				if (Hit.Time <= 0.0) return false;
				ForwardOffset += HorizontalDelta * Hit.Time;
			}
			else ForwardOffset += HorizontalDelta;
			const double StepDownDistance = GameplayTuning::MaximumStepHeight + GameplayTuning::FloorProbeDistance;
			if (!SweepPawn(Pawn, ForwardOffset, FVector3(0.0, 0.0, -StepDownDistance), Hit)
				|| Hit.ImpactNormal.z <= 0.0)
				return false;
			const double DownDistance = StepDownDistance * Hit.Time;
			OutOffset = ForwardOffset + FVector3(0.0, 0.0, -DownDistance + GameplayTuning::CollisionSkinWidth);
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
		return Pawn && GetVelocity().z <= 0.0
			&& FindFloor(*Pawn, FVector3(0.0), GameplayTuning::FloorProbeDistance, Floor);
	}

	auto DSimpleGroundMovementComponent::PerformMovement(
		const FPawnControlIntent& Intent,
		float DeltaSeconds) -> void
	{
		auto* Pawn = Cast<APlayerPawn>(GetPawnOwner());
		if (!Pawn || !GetPawnWorld(*Pawn) || !std::isfinite(DeltaSeconds) || DeltaSeconds <= 0.0f) return;
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
		const double Rate = Math::LengthSquared(DesiredHorizontal) > kDoubleSmallNumber
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
		FVector3 AcceptedOffset(0.0);
		for (uint32 SweepIndex = 0; SweepIndex < GameplayTuning::MaximumMovementSweeps; ++SweepIndex)
		{
			if (Math::LengthSquared(Remaining)
				<= GameplayTuning::MinimumMovementDistance * GameplayTuning::MinimumMovementDistance) break;
			FHitResult Hit;
			if (!SweepPawn(*Pawn, AcceptedOffset, Remaining, Hit))
			{
				AcceptedOffset += Remaining;
				break;
			}
			if (Hit.bStartPenetrating)
			{
				const double Recovery = std::min(
					Hit.PenetrationDepth + GameplayTuning::CollisionSkinWidth,
					GameplayTuning::MaximumStepHeight);
				if (Recovery <= GameplayTuning::MinimumMovementDistance) break;
				AcceptedOffset += Hit.ImpactNormal * Recovery;
				continue;
			}
			const double RemainingLength = Math::Length(Remaining);
			const double SkinTime = RemainingLength > GameplayTuning::MinimumMovementDistance
				? GameplayTuning::CollisionSkinWidth / RemainingLength
				: 0.0;
			const double TravelTime = std::max(0.0, Hit.Time - SkinTime);
			AcceptedOffset += Remaining * TravelTime;
			FVector3 Unconsumed = Remaining * (1.0 - Hit.Time);
			const bool bWall = Hit.ImpactNormal.z < GameplayTuning::WalkableFloorZ
				&& std::abs(Hit.ImpactNormal.z) < GameplayTuning::WalkableFloorZ;
			if (bWall && bWasGrounded)
			{
				FVector3 StepOffset;
				const FVector3 HorizontalRemainder(Unconsumed.x, Unconsumed.y, 0.0);
				if (TryStep(*Pawn, AcceptedOffset, HorizontalRemainder, StepOffset))
				{
					AcceptedOffset = StepOffset;
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
			if (FindFloor(*Pawn, AcceptedOffset, GameplayTuning::FloorProbeDistance, Floor))
			{
				AcceptedOffset.z -= GameplayTuning::FloorProbeDistance * Floor.Time;
				AcceptedOffset.z += GameplayTuning::CollisionSkinWidth;
				Velocity.z = 0.0;
			}
		}

		FTransform Transform = Pawn->GetActorTransform();
		Transform.Translation += AcceptedOffset;
		if (bYawChanged)
		{
			Transform.Rotation = Math::MakeQuaternionFromAxisAngleDegrees(
				Pawn->GetYawDegrees(), FVectorConstants::Up);
		}
		SetVelocity(Velocity);
		if (bYawChanged || Math::LengthSquared(AcceptedOffset) > 0.0)
		{
			verify(Pawn->SetActorTransform(Transform));
		}
	}
}
