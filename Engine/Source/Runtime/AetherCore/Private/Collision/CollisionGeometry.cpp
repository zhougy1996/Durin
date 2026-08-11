#include "Collision/CollisionGeometry.h"
#include "Physics/PhysicsTypes.h"

namespace Durin
{
	class FCollisionGeometry
	{
	public:
		uint64 Identity = 0;
		std::vector<FCollisionGeometryChild> Children;
		FVector3 LocalMin{0.0};
		FVector3 LocalMax{0.0};
		uint64 RetainedBytes = 0;
	};

	namespace
	{
		std::atomic<uint64> GNextCollisionGeometryIdentity = 1;

		auto BuildGeometryChildBounds(
			const FCollisionGeometryChild& Child, FVector3& OutMin, FVector3& OutMax) -> bool
		{
			if (!Child.Shape.IsValid() || !IsValidPhysicsTransform(Child.LocalTransform)) return false;
			FVector3 LocalExtent;
			switch (Child.Shape.GetType())
			{
			case ECollisionShapeType::Box:
				LocalExtent = Child.Shape.GetBoxHalfExtent() * Child.LocalTransform.Scale3D;
				break;
			case ECollisionShapeType::Sphere:
			{
				const double Radius = Child.Shape.GetSphereRadius() * std::max({
					Child.LocalTransform.Scale3D.x,
					Child.LocalTransform.Scale3D.y,
					Child.LocalTransform.Scale3D.z});
				LocalExtent = FVector3(Radius);
				break;
			}
			case ECollisionShapeType::Capsule:
			{
				const double Radius = Child.Shape.GetCapsuleRadius()
					* std::max(Child.LocalTransform.Scale3D.x, Child.LocalTransform.Scale3D.y);
				const double HalfHeight = std::max(
					Radius, Child.Shape.GetCapsuleHalfHeight() * Child.LocalTransform.Scale3D.z);
				LocalExtent = FVector3(Radius, Radius, HalfHeight);
				break;
			}
			}
			FQuat Rotation;
			if (!Math::TryNormalize(Child.LocalTransform.Rotation, Rotation)) return false;
			const FVector3 X = Math::Abs(Math::RotateVector(Rotation, FVectorConstants::Forward));
			const FVector3 Y = Math::Abs(Math::RotateVector(Rotation, FVectorConstants::Right));
			const FVector3 Z = Math::Abs(Math::RotateVector(Rotation, FVectorConstants::Up));
			const FVector3 WorldExtent = X * LocalExtent.x + Y * LocalExtent.y + Z * LocalExtent.z;
			OutMin = Child.LocalTransform.Translation - WorldExtent;
			OutMax = Child.LocalTransform.Translation + WorldExtent;
			return Math::IsFinite(OutMin) && Math::IsFinite(OutMax);
		}
	}

	auto FCollisionGeometryRef::MakePrimitive(const FCollisionShape& Shape) -> FCollisionGeometryRef
	{
		if (!Shape.IsValid()) return {};
		const FCollisionGeometryChild Child{.Shape = Shape, .LocalTransform = FTransform()};
		return MakeCompound(std::span(&Child, 1));
	}

	auto FCollisionGeometryRef::MakeCompound(std::span<const FCollisionGeometryChild> Children)
		-> FCollisionGeometryRef
	{
		if (Children.empty() || Children.size() > 64) return {};
		auto Payload = std::make_shared<FCollisionGeometry>();
		Payload->Children.reserve(Children.size());
		for (const FCollisionGeometryChild& Child : Children)
		{
			FVector3 ChildMin;
			FVector3 ChildMax;
			if (!BuildGeometryChildBounds(Child, ChildMin, ChildMax)) return {};
			if (Payload->Children.empty())
			{
				Payload->LocalMin = ChildMin;
				Payload->LocalMax = ChildMax;
			}
			else
			{
				Payload->LocalMin = Math::Min(Payload->LocalMin, ChildMin);
				Payload->LocalMax = Math::Max(Payload->LocalMax, ChildMax);
			}
			Payload->Children.push_back(Child);
		}
		uint64 Identity = GNextCollisionGeometryIdentity.fetch_add(1, std::memory_order_relaxed);
		if (Identity == 0) Identity = GNextCollisionGeometryIdentity.fetch_add(1, std::memory_order_relaxed);
		Payload->Identity = Identity;
		Payload->RetainedBytes = sizeof(FCollisionGeometry)
			+ Payload->Children.capacity() * sizeof(FCollisionGeometryChild);
		return FCollisionGeometryRef(std::move(Payload));
	}

	auto FCollisionGeometryRef::GetIdentity() const -> uint64
	{
		return Payload ? Payload->Identity : 0;
	}

	auto FCollisionGeometryRef::GetChildCount() const -> uint32
	{
		return Payload ? static_cast<uint32>(Payload->Children.size()) : 0;
	}

	auto FCollisionGeometryRef::GetChild(uint32 Index) const -> const FCollisionGeometryChild*
	{
		return Payload && Index < Payload->Children.size() ? &Payload->Children[Index] : nullptr;
	}

	auto FCollisionGeometryRef::GetLocalBounds(FVector3& OutMin, FVector3& OutMax) const -> bool
	{
		if (!Payload) return false;
		OutMin = Payload->LocalMin;
		OutMax = Payload->LocalMax;
		return true;
	}

	auto FCollisionGeometryRef::GetRetainedBytes() const -> uint64
	{
		return Payload ? Payload->RetainedBytes : 0;
	}
}

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

		auto ExactSegmentBoxDistance(
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
			const FVector3 Direction = SegmentEnd - SegmentStart;
			std::array<double, 8> Breakpoints{};
			uint32 BreakpointCount = 2;
			Breakpoints[0] = 0.0;
			Breakpoints[1] = 1.0;
			for (uint32 Axis = 0; Axis < 3; ++Axis)
			{
				if (std::abs(Direction[Axis]) <= ContactTolerance) continue;
				for (double Plane : {-Extent[Axis], Extent[Axis]})
				{
					const double Alpha = (Plane - SegmentStart[Axis]) / Direction[Axis];
					if (Alpha > 0.0 && Alpha < 1.0) Breakpoints[BreakpointCount++] = Alpha;
				}
			}
			std::ranges::sort(Breakpoints.begin(), Breakpoints.begin() + BreakpointCount);
			FCapsuleBoxDistance Result = Evaluate(0.0);
			uint64 EvaluationCount = 1;
			for (uint32 Interval = 0; Interval + 1 < BreakpointCount; ++Interval)
			{
				const double Low = Breakpoints[Interval];
				const double High = Breakpoints[Interval + 1];
				const double Middle = (Low + High) * 0.5;
				const FVector3 MiddlePoint = SegmentStart + Direction * Middle;
				double A = 0.0;
				double B = 0.0;
				for (uint32 Axis = 0; Axis < 3; ++Axis)
				{
					double Plane = MiddlePoint[Axis];
					if (MiddlePoint[Axis] < -Extent[Axis]) Plane = -Extent[Axis];
					else if (MiddlePoint[Axis] > Extent[Axis]) Plane = Extent[Axis];
					else continue;
					A += Direction[Axis] * Direction[Axis];
					B += Direction[Axis] * (SegmentStart[Axis] - Plane);
				}
				const double CandidateAlpha = A > 0.0 ? std::clamp(-B / A, Low, High) : Low;
				for (double Alpha : {Low, CandidateAlpha, High})
				{
					const FCapsuleBoxDistance Candidate = Evaluate(Alpha);
					++EvaluationCount;
					if (Candidate.SquaredDistance < Result.SquaredDistance) Result = Candidate;
				}
			}
			RecordGeometryWork(Counters, EvaluationCount, 0);
			return Result;
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

	namespace
	{
		struct FLeafDistance
		{
			double Separation = 0.0;
			FVector3 Normal{0.0};
			FVector3 TargetPoint{0.0};
			FVector3 QueryPoint{0.0};
		};

		auto GetSphere(
			const FCollisionShape& Shape, const FTransform& Transform,
			FVector3& OutCenter, double& OutRadius) -> bool
		{
			if (Shape.GetType() != ECollisionShapeType::Sphere || !Shape.IsValid()
				|| !IsValidPhysicsTransform(Transform)) return false;
			OutCenter = Transform.Translation;
			OutRadius = Shape.GetSphereRadius() * std::max({
				Transform.Scale3D.x, Transform.Scale3D.y, Transform.Scale3D.z});
			return std::isfinite(OutRadius);
		}

		auto ClosestPointOnSegment(const FVector3& Point, const FVector3& Start, const FVector3& End)
			-> FVector3
		{
			const FVector3 Delta = End - Start;
			const double LengthSquared = Math::LengthSquared(Delta);
			const double Alpha = LengthSquared > 0.0
				? std::clamp(Math::Dot(Point - Start, Delta) / LengthSquared, 0.0, 1.0) : 0.0;
			return Start + Delta * Alpha;
		}

		auto ClosestSegmentPoints(
			const FVector3& FirstStart, const FVector3& FirstEnd,
			const FVector3& SecondStart, const FVector3& SecondEnd,
			FVector3& OutFirst, FVector3& OutSecond) -> void
		{
			const FVector3 D1 = FirstEnd - FirstStart;
			const FVector3 D2 = SecondEnd - SecondStart;
			const FVector3 R = FirstStart - SecondStart;
			const double A = Math::Dot(D1, D1);
			const double E = Math::Dot(D2, D2);
			const double F = Math::Dot(D2, R);
			double S = 0.0;
			double T = 0.0;
			if (A <= ContactTolerance && E <= ContactTolerance)
			{
				OutFirst = FirstStart;
				OutSecond = SecondStart;
				return;
			}
			if (A <= ContactTolerance) T = std::clamp(F / E, 0.0, 1.0);
			else
			{
				const double C = Math::Dot(D1, R);
				if (E <= ContactTolerance) S = std::clamp(-C / A, 0.0, 1.0);
				else
				{
					const double B = Math::Dot(D1, D2);
					const double Denominator = A * E - B * B;
					if (Denominator != 0.0) S = std::clamp((B * F - C * E) / Denominator, 0.0, 1.0);
					T = (B * S + F) / E;
					if (T < 0.0)
					{
						T = 0.0;
						S = std::clamp(-C / A, 0.0, 1.0);
					}
					else if (T > 1.0)
					{
						T = 1.0;
						S = std::clamp((B - C) / A, 0.0, 1.0);
					}
				}
			}
			OutFirst = FirstStart + D1 * S;
			OutSecond = SecondStart + D2 * T;
		}

		auto SupportPoint(
			const FCollisionShape& Shape, const FTransform& Transform, const FVector3& Direction,
			FVector3& OutPoint, FCollisionGeometryCounters* Counters) -> bool
		{
			if (Counters) AddCounter(Counters->SupportEvaluations, 1, Counters->bOverflowed);
			const FVector3 Normal = Math::NormalizeOr(Direction, FVectorConstants::Forward);
			if (Shape.GetType() == ECollisionShapeType::Sphere)
			{
				FVector3 Center;
				double Radius = 0.0;
				if (!GetSphere(Shape, Transform, Center, Radius)) return false;
				OutPoint = Center + Normal * Radius;
				return true;
			}
			if (Shape.GetType() == ECollisionShapeType::Capsule)
			{
				FVector3 Start;
				FVector3 End;
				double Radius = 0.0;
				if (!GetCapsuleSegment(Shape, Transform, Start, End, Radius)) return false;
				OutPoint = (Math::Dot(Start, Normal) >= Math::Dot(End, Normal) ? Start : End)
					+ Normal * Radius;
				return true;
			}
			if (Shape.GetType() != ECollisionShapeType::Box || !Shape.IsValid()
				|| !IsValidPhysicsTransform(Transform)) return false;
			FQuat Rotation;
			if (!Math::TryNormalize(Transform.Rotation, Rotation)) return false;
			const std::array<FVector3, 3> Axes{
				Math::RotateVector(Rotation, FVectorConstants::Forward),
				Math::RotateVector(Rotation, FVectorConstants::Right),
				Math::RotateVector(Rotation, FVectorConstants::Up)};
			const FVector3 Extent = Shape.GetBoxHalfExtent() * Transform.Scale3D;
			OutPoint = Transform.Translation;
			for (uint32 Index = 0; Index < 3; ++Index)
				OutPoint += Axes[Index] * Extent[Index] * (Math::Dot(Normal, Axes[Index]) >= 0.0 ? 1.0 : -1.0);
			return true;
		}

		auto PointBoxDistance(
			const FVector3& Point, const FCollisionShape& Box, const FTransform& BoxTransform,
			FLeafDistance& OutDistance) -> bool
		{
			if (Box.GetType() != ECollisionShapeType::Box || !Box.IsValid()
				|| !IsValidPhysicsTransform(BoxTransform)) return false;
			const FVector3 LocalPoint = ToBoxSpace(Point, BoxTransform);
			const FVector3 Extent = Box.GetBoxHalfExtent() * BoxTransform.Scale3D;
			FVector3 LocalClosest = ClosestPointOnBox(LocalPoint, Extent);
			const FVector3 Delta = LocalPoint - LocalClosest;
			const double Distance = Math::Length(Delta);
			FVector3 LocalNormal = Math::NormalizeOr(Delta, FVectorConstants::Forward);
			if (Distance <= ContactTolerance)
			{
				double BestDepth = std::numeric_limits<double>::max();
				for (uint32 Axis = 0; Axis < 3; ++Axis)
				{
					const double Depth = Extent[Axis] - std::abs(LocalPoint[Axis]);
					if (Depth < BestDepth)
					{
						BestDepth = Depth;
						LocalNormal = FVector3(0.0);
						LocalNormal[Axis] = LocalPoint[Axis] >= 0.0 ? 1.0 : -1.0;
						LocalClosest[Axis] = LocalNormal[Axis] * Extent[Axis];
					}
				}
				OutDistance.Separation = -BestDepth;
			}
			else OutDistance.Separation = Distance;
			OutDistance.Normal = Math::NormalizeOr(ToWorldDirection(LocalNormal, BoxTransform), FVectorConstants::Forward);
			OutDistance.TargetPoint = BoxTransform.Translation + ToWorldDirection(LocalClosest, BoxTransform);
			OutDistance.QueryPoint = Point;
			return true;
		}

		auto PrimitiveDistance(
			const FCollisionShape& Query, const FTransform& QueryTransform,
			const FCollisionShape& Target, const FTransform& TargetTransform,
			FLeafDistance& OutDistance, FCollisionGeometryCounters* Counters) -> bool
		{
			RecordGeometryWork(Counters, 1, 0);
			if (!Query.IsValid() || !Target.IsValid() || !IsValidPhysicsTransform(QueryTransform)
				|| !IsValidPhysicsTransform(TargetTransform)) return false;
			if (Query.GetType() == ECollisionShapeType::Sphere)
			{
				FVector3 QueryCenter;
				double QueryRadius = 0.0;
				if (!GetSphere(Query, QueryTransform, QueryCenter, QueryRadius)) return false;
				if (Target.GetType() == ECollisionShapeType::Sphere)
				{
					FVector3 TargetCenter;
					double TargetRadius = 0.0;
					if (!GetSphere(Target, TargetTransform, TargetCenter, TargetRadius)) return false;
					const FVector3 Delta = QueryCenter - TargetCenter;
					const double CenterDistance = Math::Length(Delta);
					OutDistance.Normal = Math::NormalizeOr(Delta, FVectorConstants::Forward);
					OutDistance.Separation = CenterDistance - QueryRadius - TargetRadius;
					OutDistance.TargetPoint = TargetCenter + OutDistance.Normal * TargetRadius;
					OutDistance.QueryPoint = QueryCenter - OutDistance.Normal * QueryRadius;
					return true;
				}
				if (Target.GetType() == ECollisionShapeType::Box)
				{
					if (!PointBoxDistance(QueryCenter, Target, TargetTransform, OutDistance)) return false;
					OutDistance.Separation -= QueryRadius;
					OutDistance.QueryPoint = QueryCenter - OutDistance.Normal * QueryRadius;
					return true;
				}
				FVector3 TargetStart;
				FVector3 TargetEnd;
				double TargetRadius = 0.0;
				if (!GetCapsuleSegment(Target, TargetTransform, TargetStart, TargetEnd, TargetRadius)) return false;
				const FVector3 TargetAxisPoint = ClosestPointOnSegment(QueryCenter, TargetStart, TargetEnd);
				const FVector3 Delta = QueryCenter - TargetAxisPoint;
				const double CenterDistance = Math::Length(Delta);
				OutDistance.Normal = Math::NormalizeOr(Delta, FVectorConstants::Forward);
				OutDistance.Separation = CenterDistance - QueryRadius - TargetRadius;
				OutDistance.TargetPoint = TargetAxisPoint + OutDistance.Normal * TargetRadius;
				OutDistance.QueryPoint = QueryCenter - OutDistance.Normal * QueryRadius;
				return true;
			}
			if (Query.GetType() == ECollisionShapeType::Capsule
				&& Target.GetType() == ECollisionShapeType::Capsule)
			{
				FVector3 QueryStart;
				FVector3 QueryEnd;
				FVector3 TargetStart;
				FVector3 TargetEnd;
				double QueryRadius = 0.0;
				double TargetRadius = 0.0;
				if (!GetCapsuleSegment(Query, QueryTransform, QueryStart, QueryEnd, QueryRadius)
					|| !GetCapsuleSegment(Target, TargetTransform, TargetStart, TargetEnd, TargetRadius)) return false;
				FVector3 QueryPoint;
				FVector3 TargetPoint;
				ClosestSegmentPoints(QueryStart, QueryEnd, TargetStart, TargetEnd, QueryPoint, TargetPoint);
				const FVector3 Delta = QueryPoint - TargetPoint;
				const double AxisDistance = Math::Length(Delta);
				OutDistance.Normal = Math::NormalizeOr(Delta, FVectorConstants::Forward);
				OutDistance.Separation = AxisDistance - QueryRadius - TargetRadius;
				OutDistance.TargetPoint = TargetPoint + OutDistance.Normal * TargetRadius;
				OutDistance.QueryPoint = QueryPoint - OutDistance.Normal * QueryRadius;
				return true;
			}
			if (Query.GetType() == ECollisionShapeType::Capsule
				&& Target.GetType() == ECollisionShapeType::Box)
			{
				FVector3 QueryStart;
				FVector3 QueryEnd;
				double QueryRadius = 0.0;
				if (!GetCapsuleSegment(Query, QueryTransform, QueryStart, QueryEnd, QueryRadius)) return false;
				const FVector3 Extent = Target.GetBoxHalfExtent() * TargetTransform.Scale3D;
				const FCapsuleBoxDistance Distance = ExactSegmentBoxDistance(
					ToBoxSpace(QueryStart, TargetTransform), ToBoxSpace(QueryEnd, TargetTransform), Extent, Counters);
				const FVector3 LocalDelta = Distance.SegmentPoint - Distance.BoxPoint;
				OutDistance.Normal = Math::NormalizeOr(
					ToWorldDirection(LocalDelta, TargetTransform), FVectorConstants::Forward);
				OutDistance.Separation = std::sqrt(std::max(0.0, Distance.SquaredDistance)) - QueryRadius;
				OutDistance.TargetPoint = TargetTransform.Translation
					+ ToWorldDirection(Distance.BoxPoint, TargetTransform);
				OutDistance.QueryPoint = TargetTransform.Translation
					+ ToWorldDirection(Distance.SegmentPoint, TargetTransform)
					- OutDistance.Normal * QueryRadius;
				return true;
			}
			if (Target.GetType() == ECollisionShapeType::Sphere
				|| (Query.GetType() == ECollisionShapeType::Box
					&& Target.GetType() == ECollisionShapeType::Capsule))
			{
				FLeafDistance Reverse;
				if (!PrimitiveDistance(Target, TargetTransform, Query, QueryTransform, Reverse, Counters)) return false;
				OutDistance.Separation = Reverse.Separation;
				OutDistance.Normal = -Reverse.Normal;
				OutDistance.TargetPoint = Reverse.QueryPoint;
				OutDistance.QueryPoint = Reverse.TargetPoint;
				return true;
			}
			// Box/Box uses all face and edge-cross separating axes. The largest gap is a conservative cast step.
			if (Query.GetType() == ECollisionShapeType::Box && Target.GetType() == ECollisionShapeType::Box)
			{
				FQuat QueryRotation;
				FQuat TargetRotation;
				if (!Math::TryNormalize(QueryTransform.Rotation, QueryRotation)
					|| !Math::TryNormalize(TargetTransform.Rotation, TargetRotation)) return false;
				const std::array<FVector3, 3> QueryAxes{
					Math::RotateVector(QueryRotation, FVectorConstants::Forward),
					Math::RotateVector(QueryRotation, FVectorConstants::Right),
					Math::RotateVector(QueryRotation, FVectorConstants::Up)};
				const std::array<FVector3, 3> TargetAxes{
					Math::RotateVector(TargetRotation, FVectorConstants::Forward),
					Math::RotateVector(TargetRotation, FVectorConstants::Right),
					Math::RotateVector(TargetRotation, FVectorConstants::Up)};
				const FVector3 QueryExtent = Query.GetBoxHalfExtent() * QueryTransform.Scale3D;
				const FVector3 TargetExtent = Target.GetBoxHalfExtent() * TargetTransform.Scale3D;
				const FVector3 Centers = QueryTransform.Translation - TargetTransform.Translation;
				double MaximumGap = -std::numeric_limits<double>::max();
				FVector3 BestAxis = FVectorConstants::Forward;
				auto TestAxis = [&](const FVector3& Candidate) {
					FVector3 Axis;
					if (!Math::TryNormalize(Candidate, Axis)) return;
					double QueryRadius = 0.0;
					double TargetRadius = 0.0;
					for (uint32 Index = 0; Index < 3; ++Index)
					{
						QueryRadius += std::abs(Math::Dot(Axis, QueryAxes[Index])) * QueryExtent[Index];
						TargetRadius += std::abs(Math::Dot(Axis, TargetAxes[Index])) * TargetExtent[Index];
					}
					const double SignedCenter = Math::Dot(Centers, Axis);
					const double Gap = std::abs(SignedCenter) - QueryRadius - TargetRadius;
					if (Gap > MaximumGap)
					{
						MaximumGap = Gap;
						BestAxis = SignedCenter >= 0.0 ? Axis : -Axis;
					}
				};
				for (const FVector3& Axis : QueryAxes) TestAxis(Axis);
				for (const FVector3& Axis : TargetAxes) TestAxis(Axis);
				for (const FVector3& QueryAxis : QueryAxes)
					for (const FVector3& TargetAxis : TargetAxes) TestAxis(Math::Cross(QueryAxis, TargetAxis));
				OutDistance.Separation = MaximumGap;
				OutDistance.Normal = BestAxis;
				return SupportPoint(Target, TargetTransform, BestAxis, OutDistance.TargetPoint, Counters)
					&& SupportPoint(Query, QueryTransform, -BestAxis, OutDistance.QueryPoint, Counters);
			}
			return false;
		}

		auto RaycastSphereLeaf(
			const FVector3& Start, const FVector3& End,
			const FCollisionShape& Sphere, const FTransform& Transform, FPhysicsQueryHit& OutHit) -> bool
		{
			FVector3 Center;
			double Radius = 0.0;
			if (!GetSphere(Sphere, Transform, Center, Radius)) return false;
			const FVector3 Delta = End - Start;
			const FVector3 Offset = Start - Center;
			const double C = Math::Dot(Offset, Offset) - Radius * Radius;
			if (C < -ContactTolerance)
			{
				OutHit.Time = 0.0;
				OutHit.Location = Start;
				OutHit.bStartPenetrating = true;
				const double CenterDistance = Math::Length(Offset);
				OutHit.ImpactNormal = Math::NormalizeOr(Offset, FVectorConstants::Forward);
				OutHit.ImpactPoint = Center + OutHit.ImpactNormal * Radius;
				OutHit.PenetrationDepth = Radius - CenterDistance;
				return true;
			}
			const double A = Math::Dot(Delta, Delta);
			const double B = Math::Dot(Offset, Delta);
			const double Discriminant = B * B - A * C;
			if (A <= ContactTolerance || Discriminant < 0.0) return false;
			const double Time = (-B - std::sqrt(std::max(0.0, Discriminant))) / A;
			if (Time < 0.0 || Time > 1.0) return false;
			OutHit.Time = Time;
			OutHit.Location = Start + Delta * Time;
			OutHit.ImpactNormal = Math::NormalizeOr(OutHit.Location - Center, FVectorConstants::Forward);
			OutHit.ImpactPoint = OutHit.Location;
			return true;
		}

		auto RaycastCapsuleLeaf(
			const FVector3& Start, const FVector3& End,
			const FCollisionShape& Capsule, const FTransform& Transform, FPhysicsQueryHit& OutHit) -> bool
		{
			FVector3 SegmentStart;
			FVector3 SegmentEnd;
			double Radius = 0.0;
			if (!GetCapsuleSegment(Capsule, Transform, SegmentStart, SegmentEnd, Radius)) return false;
			const FVector3 Closest = ClosestPointOnSegment(Start, SegmentStart, SegmentEnd);
			const FVector3 InitialDelta = Start - Closest;
			const double InitialDistance = Math::Length(InitialDelta);
			if (InitialDistance < Radius - ContactTolerance)
			{
				OutHit.Time = 0.0;
				OutHit.Location = Start;
				OutHit.ImpactNormal = Math::NormalizeOr(InitialDelta, FVectorConstants::Forward);
				OutHit.ImpactPoint = Closest + OutHit.ImpactNormal * Radius;
				OutHit.PenetrationDepth = Radius - InitialDistance;
				OutHit.bStartPenetrating = true;
				return true;
			}
			const FVector3 Delta = End - Start;
			const double Length = Math::Length(Delta);
			if (Length <= ContactTolerance) return false;
			const FVector3 Direction = Delta / Length;
			const FVector3 Axis = SegmentEnd - SegmentStart;
			const FVector3 Offset = Start - SegmentStart;
			const double AxisSquared = Math::Dot(Axis, Axis);
			const double AxisDirection = Math::Dot(Axis, Direction);
			const double AxisOffset = Math::Dot(Axis, Offset);
			const double DirectionOffset = Math::Dot(Direction, Offset);
			const double OffsetSquared = Math::Dot(Offset, Offset);
			const double A = AxisSquared - AxisDirection * AxisDirection;
			const double B = AxisSquared * DirectionOffset - AxisOffset * AxisDirection;
			const double C = AxisSquared * OffsetSquared - AxisOffset * AxisOffset
				- Radius * Radius * AxisSquared;
			double Distance = std::numeric_limits<double>::max();
			const double Discriminant = B * B - A * C;
			if (std::abs(A) > ContactTolerance && Discriminant >= 0.0)
			{
				const double Candidate = (-B - std::sqrt(std::max(0.0, Discriminant))) / A;
				const double AxisTime = AxisOffset + Candidate * AxisDirection;
				if (Candidate >= 0.0 && AxisTime >= 0.0 && AxisTime <= AxisSquared) Distance = Candidate;
			}
			for (const FVector3& CapCenter : {SegmentStart, SegmentEnd})
			{
				FPhysicsQueryHit CapHit;
				FTransform SphereTransform;
				SphereTransform.Translation = CapCenter;
				if (!RaycastSphereLeaf(Start, End, FCollisionShape::MakeSphere(Radius), SphereTransform, CapHit)) continue;
				Distance = std::min(Distance, CapHit.Time * Length);
			}
			if (!std::isfinite(Distance) || Distance < 0.0 || Distance > Length) return false;
			OutHit.Time = Distance / Length;
			OutHit.Location = Start + Delta * OutHit.Time;
			const FVector3 AxisPoint = ClosestPointOnSegment(OutHit.Location, SegmentStart, SegmentEnd);
			OutHit.ImpactNormal = Math::NormalizeOr(OutHit.Location - AxisPoint, -Direction);
			OutHit.ImpactPoint = OutHit.Location;
			return true;
		}

		auto OverlapLeaf(
			const FCollisionShape& Query, const FTransform& QueryTransform,
			const FCollisionShape& Target, const FTransform& TargetTransform,
			FPhysicsQueryHit& OutHit, FCollisionGeometryCounters* Counters) -> bool
		{
			FLeafDistance Distance;
			if (!PrimitiveDistance(Query, QueryTransform, Target, TargetTransform, Distance, Counters)
				|| Distance.Separation >= -ContactTolerance) return false;
			OutHit = {};
			OutHit.Time = 0.0;
			OutHit.Location = QueryTransform.Translation;
			OutHit.ImpactPoint = Distance.TargetPoint;
			OutHit.ImpactNormal = Math::NormalizeOr(Distance.Normal, FVectorConstants::Forward);
			OutHit.PenetrationDepth = -Distance.Separation;
			OutHit.bStartPenetrating = true;
			return true;
		}

		auto SweepLeaf(
			const FCollisionShape& Query, const FTransform& QueryTransform, const FVector3& Delta,
			const FCollisionShape& Target, const FTransform& TargetTransform,
			FPhysicsQueryHit& OutHit, FCollisionGeometryCounters* Counters) -> ECollisionQueryStatus
		{
			if (OverlapLeaf(Query, QueryTransform, Target, TargetTransform, OutHit, Counters)) return ECollisionQueryStatus::Hit;
			if (Math::LengthSquared(Delta) <= ContactTolerance * ContactTolerance) return ECollisionQueryStatus::Miss;
			double Time = 0.0;
			for (uint32 Iteration = 0; Iteration < 32; ++Iteration)
			{
				RecordGeometryWork(Counters, 0, 1);
				FTransform Moved = QueryTransform;
				Moved.Translation += Delta * Time;
				FLeafDistance Distance;
				if (!PrimitiveDistance(Query, Moved, Target, TargetTransform, Distance, Counters))
					return ECollisionQueryStatus::Unsupported;
				if (Distance.Separation <= ContactTolerance)
				{
					if (Distance.Separation >= -ContactTolerance
						&& Math::Dot(Delta, Distance.Normal) >= -ContactTolerance)
						return ECollisionQueryStatus::Miss;
					OutHit = {};
					OutHit.Time = std::clamp(Time, 0.0, 1.0);
					OutHit.Location = Moved.Translation;
					OutHit.ImpactPoint = Distance.TargetPoint;
					OutHit.ImpactNormal = Math::NormalizeOr(Distance.Normal, -Delta);
					return ECollisionQueryStatus::Hit;
				}
				const double ClosingSpeed = -Math::Dot(Delta, Distance.Normal);
				if (ClosingSpeed <= ContactTolerance) return ECollisionQueryStatus::Miss;
				const double Step = Distance.Separation / ClosingSpeed;
				if (!std::isfinite(Step) || Step <= 1.0e-12) return ECollisionQueryStatus::NonConverged;
				Time += Step;
				if (Time > 1.0 + ContactTolerance) return ECollisionQueryStatus::Miss;
			}
			return ECollisionQueryStatus::NonConverged;
		}

		auto AddSimpleCounter(uint64& Value, FCollisionGeometryCounters* Counters) -> void
		{
			if (!Counters) return;
			AddCounter(Value, 1, Counters->bOverflowed);
		}
	}

	auto Raycast(
		const FVector3& Start, const FVector3& End,
		const FCollisionGeometryRef& Target, const FTransform& TargetTransform,
		ECollisionQueryAlgorithm, FPhysicsQueryHit& OutHit,
		FCollisionGeometryCounters* Counters) -> ECollisionQueryStatus
	{
		OutHit = {};
		if (!Math::IsFinite(Start) || !Math::IsFinite(End) || !Target.IsValid()
			|| !IsValidPhysicsTransform(TargetTransform)) return ECollisionQueryStatus::Invalid;
		bool bFound = false;
		uint32 Winner = 0;
		for (uint32 Index = 0; Index < Target.GetChildCount(); ++Index)
		{
			const FCollisionGeometryChild* Child = Target.GetChild(Index);
			if (!Child) return ECollisionQueryStatus::Invalid;
			if (Counters)
			{
				AddSimpleCounter(Counters->LeafTests, Counters);
				if (Target.GetChildCount() > 1) AddSimpleCounter(Counters->CompoundChildren, Counters);
				AddSimpleCounter(Counters->AnalyticDispatches, Counters);
			}
			FPhysicsQueryHit Candidate;
			const FTransform ChildTransform = FTransform::Combine(TargetTransform, Child->LocalTransform);
			bool bHit = false;
			switch (Child->Shape.GetType())
			{
			case ECollisionShapeType::Box:
				bHit = RaycastBox(Start, End, Child->Shape, ChildTransform, Candidate, Counters);
				break;
			case ECollisionShapeType::Sphere:
				bHit = RaycastSphereLeaf(Start, End, Child->Shape, ChildTransform, Candidate);
				break;
			case ECollisionShapeType::Capsule:
				bHit = RaycastCapsuleLeaf(Start, End, Child->Shape, ChildTransform, Candidate);
				break;
			}
			if (!bHit || (bFound && (Candidate.Time > OutHit.Time
				|| (Candidate.Time == OutHit.Time && Index >= Winner)))) continue;
			OutHit = Candidate;
			Winner = Index;
			bFound = true;
		}
		return bFound ? ECollisionQueryStatus::Hit : ECollisionQueryStatus::Miss;
	}

	auto Sweep(
		const FCollisionShape& Query, const FTransform& QueryTransform, const FVector3& Delta,
		const FCollisionGeometryRef& Target, const FTransform& TargetTransform,
		ECollisionQueryAlgorithm Algorithm, FPhysicsQueryHit& OutHit,
		FCollisionGeometryCounters* Counters) -> ECollisionQueryStatus
	{
		OutHit = {};
		if (!Query.IsValid() || !IsValidPhysicsTransform(QueryTransform) || !Math::IsFinite(Delta)
			|| !Target.IsValid() || !IsValidPhysicsTransform(TargetTransform)) return ECollisionQueryStatus::Invalid;
		ECollisionQueryStatus FinalStatus = ECollisionQueryStatus::Miss;
		uint32 Winner = 0;
		bool bFound = false;
		for (uint32 Index = 0; Index < Target.GetChildCount(); ++Index)
		{
			const FCollisionGeometryChild* Child = Target.GetChild(Index);
			if (!Child) return ECollisionQueryStatus::Invalid;
			if (Counters)
			{
				AddSimpleCounter(Counters->LeafTests, Counters);
				if (Target.GetChildCount() > 1) AddSimpleCounter(Counters->CompoundChildren, Counters);
				AddSimpleCounter(Counters->GenericDispatches, Counters);
			}
			FPhysicsQueryHit Candidate;
			const FTransform ChildTransform = FTransform::Combine(TargetTransform, Child->LocalTransform);
			ECollisionQueryStatus Status = SweepLeaf(
				Query, QueryTransform, Delta, Child->Shape, ChildTransform, Candidate, Counters);
			if ((Status == ECollisionQueryStatus::NonConverged
				|| Status == ECollisionQueryStatus::Unsupported)
				&& Algorithm == ECollisionQueryAlgorithm::Production
				&& Query.GetType() == ECollisionShapeType::Capsule
				&& Child->Shape.GetType() == ECollisionShapeType::Box)
			{
				if (Counters) AddSimpleCounter(Counters->ReferenceFallbacks, Counters);
				Status = SweepCapsuleBox(Query, QueryTransform, Delta, Child->Shape, ChildTransform,
					Candidate, Counters) ? ECollisionQueryStatus::Hit : ECollisionQueryStatus::Miss;
			}
			if (Status == ECollisionQueryStatus::NonConverged)
			{
				FinalStatus = Status;
				if (Counters) AddSimpleCounter(Counters->NonConverged, Counters);
				continue;
			}
			if (Status == ECollisionQueryStatus::Unsupported)
			{
				FinalStatus = Status;
				if (Counters) AddSimpleCounter(Counters->Unsupported, Counters);
				continue;
			}
			if (Status != ECollisionQueryStatus::Hit || (bFound && (Candidate.Time > OutHit.Time
				|| (Candidate.Time == OutHit.Time && Index >= Winner)))) continue;
			OutHit = Candidate;
			Winner = Index;
			bFound = true;
			FinalStatus = ECollisionQueryStatus::Hit;
		}
		return FinalStatus;
	}

	auto Overlap(
		const FCollisionShape& Query, const FTransform& QueryTransform,
		const FCollisionGeometryRef& Target, const FTransform& TargetTransform,
		ECollisionQueryAlgorithm Algorithm, FPhysicsQueryHit& OutHit,
		FCollisionGeometryCounters* Counters) -> ECollisionQueryStatus
	{
		OutHit = {};
		if (!Query.IsValid() || !IsValidPhysicsTransform(QueryTransform)
			|| !Target.IsValid() || !IsValidPhysicsTransform(TargetTransform)) return ECollisionQueryStatus::Invalid;
		for (uint32 Index = 0; Index < Target.GetChildCount(); ++Index)
		{
			const FCollisionGeometryChild* Child = Target.GetChild(Index);
			if (!Child) return ECollisionQueryStatus::Invalid;
			if (Counters)
			{
				AddSimpleCounter(Counters->LeafTests, Counters);
				if (Target.GetChildCount() > 1) AddSimpleCounter(Counters->CompoundChildren, Counters);
				AddSimpleCounter(Counters->AnalyticDispatches, Counters);
			}
			const FTransform ChildTransform = FTransform::Combine(TargetTransform, Child->LocalTransform);
			const bool bHit = OverlapLeaf(
				Query, QueryTransform, Child->Shape, ChildTransform, OutHit, Counters);
			if (bHit)
				return ECollisionQueryStatus::Hit;
		}
		return ECollisionQueryStatus::Miss;
	}
}
