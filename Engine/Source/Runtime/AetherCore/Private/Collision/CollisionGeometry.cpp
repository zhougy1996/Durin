#include "Collision/CollisionGeometry.h"

namespace Durin::CollisionGeometry
{
	namespace
	{
		constexpr double ContactTolerance = 1.0e-8;
		constexpr uint32 SearchIterations = 28;

		struct FCapsuleBoxDistance
		{
			double SquaredDistance = 0.0;
			FVector3 SegmentPoint{0.0};
			FVector3 BoxPoint{0.0};
		};

		auto AddCounter(uint64& Counter, uint64 Delta, bool& bOverflowed) -> void
		{
			if (Counter > std::numeric_limits<uint64>::max() - Delta)
			{
				Counter = std::numeric_limits<uint64>::max();
				bOverflowed = true;
				return;
			}
			Counter += Delta;
		}

		auto RecordGeometryWork(
			FCollisionGeometryCounters* Counters,
			uint64 DistanceEvaluations,
			uint64 SearchIterations) -> void
		{
			if (!Counters) return;
			AddCounter(Counters->DistanceEvaluations, DistanceEvaluations, Counters->bOverflowed);
			AddCounter(Counters->SearchIterations, SearchIterations, Counters->bOverflowed);
		}

		auto NormalizedRotation(const FQuat& Rotation) -> FQuat
		{
			FQuat Result;
			return Math::TryNormalize(Rotation, Result) ? Result : FQuatConstants::Identity;
		}

		auto ToBoxSpace(const FVector3& Point, const FTransform& BoxTransform) -> FVector3
		{
			return Math::RotateVector(
				Math::Inverse(NormalizedRotation(BoxTransform.Rotation)),
				Point - BoxTransform.Translation);
		}

		auto ToWorldDirection(const FVector3& Direction, const FTransform& BoxTransform) -> FVector3
		{
			return Math::RotateVector(NormalizedRotation(BoxTransform.Rotation), Direction);
		}

		auto ClosestPointOnBox(const FVector3& Point, const FVector3& Extent) -> FVector3
		{
			return Math::Clamp(Point, -Extent, Extent);
		}

		auto SegmentBoxDistance(
			const FVector3& SegmentStart,
			const FVector3& SegmentEnd,
			const FVector3& Extent,
			FCollisionGeometryCounters* Counters) -> FCapsuleBoxDistance
		{
			auto Evaluate = [&](double Alpha) {
				const FVector3 SegmentPoint = SegmentStart + (SegmentEnd - SegmentStart) * Alpha;
				const FVector3 BoxPoint = ClosestPointOnBox(SegmentPoint, Extent);
				return FCapsuleBoxDistance{
					.SquaredDistance = Math::LengthSquared(SegmentPoint - BoxPoint),
					.SegmentPoint = SegmentPoint,
					.BoxPoint = BoxPoint};
			};
			double Low = 0.0;
			double High = 1.0;
			for (uint32 Iteration = 0; Iteration < SearchIterations; ++Iteration)
			{
				const double First = (Low * 2.0 + High) / 3.0;
				const double Second = (Low + High * 2.0) / 3.0;
				if (Evaluate(First).SquaredDistance <= Evaluate(Second).SquaredDistance) High = Second;
				else Low = First;
			}
			FCapsuleBoxDistance Result = Evaluate((Low + High) * 0.5);
			const FCapsuleBoxDistance StartResult = Evaluate(0.0);
			const FCapsuleBoxDistance EndResult = Evaluate(1.0);
			if (StartResult.SquaredDistance < Result.SquaredDistance) Result = StartResult;
			if (EndResult.SquaredDistance < Result.SquaredDistance) Result = EndResult;
			RecordGeometryWork(Counters, SearchIterations * 2u + 3u, SearchIterations);
			return Result;
		}

		auto GetCapsuleSegment(
			const FCollisionShape& Capsule,
			const FTransform& CapsuleTransform,
			FVector3& OutStart,
			FVector3& OutEnd,
			double& OutRadius) -> bool
		{
			if (Capsule.GetType() != ECollisionShapeType::Capsule
				|| !Capsule.IsValid() || !IsValidPhysicsTransform(CapsuleTransform)) return false;
			const double RadialScale = std::max(CapsuleTransform.Scale3D.x, CapsuleTransform.Scale3D.y);
			OutRadius = Capsule.GetCapsuleRadius() * RadialScale;
			const double HalfHeight = std::max(
				OutRadius, Capsule.GetCapsuleHalfHeight() * CapsuleTransform.Scale3D.z);
			const FVector3 Axis = Math::RotateVector(
				NormalizedRotation(CapsuleTransform.Rotation), FVectorConstants::Up);
			const FVector3 Offset = Axis * (HalfHeight - OutRadius);
			OutStart = CapsuleTransform.Translation - Offset;
			OutEnd = CapsuleTransform.Translation + Offset;
			return Math::IsFinite(OutStart) && Math::IsFinite(OutEnd) && std::isfinite(OutRadius);
		}

		auto MakeCapsuleContact(
			const FCapsuleBoxDistance& Distance,
			double Radius,
			const FTransform& BoxTransform,
			const FVector3& FallbackDirection,
			FPhysicsQueryHit& OutHit) -> void
		{
			FVector3 LocalNormal;
			if (!Math::TryNormalize(Distance.SegmentPoint - Distance.BoxPoint, LocalNormal))
			{
				const FVector3 Direction = Math::NormalizeOr(FallbackDirection, FVectorConstants::Up);
				LocalNormal = Math::RotateVector(
					Math::Inverse(NormalizedRotation(BoxTransform.Rotation)), -Direction);
				const FVector3 AbsNormal = Math::Abs(LocalNormal);
				const uint32 Axis = AbsNormal.y > AbsNormal.x
					? (AbsNormal.z > AbsNormal.y ? 2u : 1u)
					: (AbsNormal.z > AbsNormal.x ? 2u : 0u);
				LocalNormal = FVector3(0.0);
				LocalNormal[Axis] = Direction[Axis] >= 0.0 ? -1.0 : 1.0;
			}
			OutHit.ImpactNormal = Math::NormalizeOr(ToWorldDirection(LocalNormal, BoxTransform), FVectorConstants::Up);
			OutHit.ImpactPoint = BoxTransform.Translation + ToWorldDirection(Distance.BoxPoint, BoxTransform);
			OutHit.PenetrationDepth = std::max(0.0, Radius - std::sqrt(std::max(0.0, Distance.SquaredDistance)));
		}
	}

	auto RaycastBox(
		const FVector3& Start,
		const FVector3& End,
		const FCollisionShape& Box,
		const FTransform& BoxTransform,
		FPhysicsQueryHit& OutHit,
		FCollisionGeometryCounters*) -> bool
	{
		OutHit = {};
		if (Box.GetType() != ECollisionShapeType::Box || !Box.IsValid()
			|| !IsValidPhysicsTransform(BoxTransform)
			|| !Math::IsFinite(Start) || !Math::IsFinite(End)) return false;
		const FVector3 LocalStart = ToBoxSpace(Start, BoxTransform);
		const FVector3 LocalEnd = ToBoxSpace(End, BoxTransform);
		const FVector3 Delta = LocalEnd - LocalStart;
		const FVector3 Extent = Box.GetBoxHalfExtent() * BoxTransform.Scale3D;
		double Enter = 0.0;
		double Exit = 1.0;
		FVector3 EnterNormal(0.0);
		const bool bInside = std::abs(LocalStart.x) <= Extent.x
			&& std::abs(LocalStart.y) <= Extent.y
			&& std::abs(LocalStart.z) <= Extent.z;
		for (uint32 Axis = 0; Axis < 3; ++Axis)
		{
			if (std::abs(Delta[Axis]) <= ContactTolerance)
			{
				if (LocalStart[Axis] < -Extent[Axis] || LocalStart[Axis] > Extent[Axis]) return false;
				continue;
			}
			double Near = (-Extent[Axis] - LocalStart[Axis]) / Delta[Axis];
			double Far = (Extent[Axis] - LocalStart[Axis]) / Delta[Axis];
			double Sign = -1.0;
			if (Near > Far)
			{
				std::swap(Near, Far);
				Sign = 1.0;
			}
			if (Near > Enter)
			{
				Enter = Near;
				EnterNormal = FVector3(0.0);
				EnterNormal[Axis] = Sign;
			}
			Exit = std::min(Exit, Far);
			if (Enter > Exit) return false;
		}
		if (Exit < 0.0 || Enter > 1.0) return false;
		OutHit.Time = bInside ? 0.0 : std::clamp(Enter, 0.0, 1.0);
		OutHit.Location = Start + (End - Start) * OutHit.Time;
		OutHit.ImpactPoint = OutHit.Location;
		OutHit.bStartPenetrating = bInside;
		if (bInside)
		{
			double BestDepth = std::numeric_limits<double>::max();
			for (uint32 Axis = 0; Axis < 3; ++Axis)
			{
				const double Depth = Extent[Axis] - std::abs(LocalStart[Axis]);
				if (Depth < BestDepth)
				{
					BestDepth = Depth;
					EnterNormal = FVector3(0.0);
					EnterNormal[Axis] = LocalStart[Axis] >= 0.0 ? 1.0 : -1.0;
				}
			}
			OutHit.PenetrationDepth = BestDepth;
		}
		OutHit.ImpactNormal = Math::NormalizeOr(ToWorldDirection(EnterNormal, BoxTransform), FVectorConstants::Up);
		return true;
	}

	auto OverlapCapsuleBox(
		const FCollisionShape& Capsule,
		const FTransform& CapsuleTransform,
		const FCollisionShape& Box,
		const FTransform& BoxTransform,
		FPhysicsQueryHit& OutHit,
		FCollisionGeometryCounters* Counters) -> bool
	{
		OutHit = {};
		if (Box.GetType() != ECollisionShapeType::Box || !Box.IsValid()
			|| !IsValidPhysicsTransform(BoxTransform)) return false;
		FVector3 SegmentStart;
		FVector3 SegmentEnd;
		double Radius = 0.0;
		if (!GetCapsuleSegment(Capsule, CapsuleTransform, SegmentStart, SegmentEnd, Radius)) return false;
		const FVector3 Extent = Box.GetBoxHalfExtent() * BoxTransform.Scale3D;
		const FCapsuleBoxDistance Distance = SegmentBoxDistance(
			ToBoxSpace(SegmentStart, BoxTransform), ToBoxSpace(SegmentEnd, BoxTransform), Extent, Counters);
		if (Distance.SquaredDistance >= Radius * Radius - ContactTolerance) return false;
		OutHit.Time = 0.0;
		OutHit.Location = CapsuleTransform.Translation;
		OutHit.bStartPenetrating = true;
		MakeCapsuleContact(Distance, Radius, BoxTransform, FVectorConstants::Up, OutHit);
		return true;
	}

	auto SweepCapsuleBox(
		const FCollisionShape& Capsule,
		const FTransform& CapsuleTransform,
		const FVector3& Delta,
		const FCollisionShape& Box,
		const FTransform& BoxTransform,
		FPhysicsQueryHit& OutHit,
		FCollisionGeometryCounters* Counters) -> bool
	{
		OutHit = {};
		if (!Math::IsFinite(Delta)) return false;
		if (OverlapCapsuleBox(Capsule, CapsuleTransform, Box, BoxTransform, OutHit, Counters)) return true;
		FVector3 SegmentStart;
		FVector3 SegmentEnd;
		double Radius = 0.0;
		if (!GetCapsuleSegment(Capsule, CapsuleTransform, SegmentStart, SegmentEnd, Radius)
			|| Box.GetType() != ECollisionShapeType::Box || !Box.IsValid()
			|| !IsValidPhysicsTransform(BoxTransform)) return false;
		const FVector3 LocalStart = ToBoxSpace(SegmentStart, BoxTransform);
		const FVector3 LocalEnd = ToBoxSpace(SegmentEnd, BoxTransform);
		const FVector3 LocalDelta = Math::RotateVector(
			Math::Inverse(NormalizedRotation(BoxTransform.Rotation)), Delta);
		const FVector3 Extent = Box.GetBoxHalfExtent() * BoxTransform.Scale3D;
		auto Evaluate = [&](double Time) {
			return SegmentBoxDistance(
				LocalStart + LocalDelta * Time, LocalEnd + LocalDelta * Time, Extent, Counters);
		};
		double Low = 0.0;
		double High = 1.0;
		for (uint32 Iteration = 0; Iteration < SearchIterations; ++Iteration)
		{
			const double First = (Low * 2.0 + High) / 3.0;
			const double Second = (Low + High * 2.0) / 3.0;
			if (Evaluate(First).SquaredDistance <= Evaluate(Second).SquaredDistance) High = Second;
			else Low = First;
		}
		RecordGeometryWork(Counters, 0, SearchIterations);
		const double MinimumTime = (Low + High) * 0.5;
		if (Evaluate(MinimumTime).SquaredDistance > Radius * Radius + ContactTolerance) return false;
		Low = 0.0;
		High = MinimumTime;
		for (uint32 Iteration = 0; Iteration < SearchIterations; ++Iteration)
		{
			const double Middle = (Low + High) * 0.5;
			if (Evaluate(Middle).SquaredDistance <= Radius * Radius + ContactTolerance) High = Middle;
			else Low = Middle;
		}
		RecordGeometryWork(Counters, 0, SearchIterations);
		OutHit.Time = std::clamp(High, 0.0, 1.0);
		OutHit.Location = CapsuleTransform.Translation + Delta * OutHit.Time;
		MakeCapsuleContact(Evaluate(OutHit.Time), Radius, BoxTransform, Delta, OutHit);
		if (Math::Dot(Delta, OutHit.ImpactNormal) >= -ContactTolerance)
		{
			OutHit = {};
			return false;
		}
		return true;
	}
}
