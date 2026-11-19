#include "Engine/World.h"

#include "Components/PrimitiveComponent.h"
#include "Collision/CollisionDebugSubsystem.h"
#include "Engine/Actor.h"
#include "Physics/PhysicsObjectID.h"

namespace Durin
{
	namespace
	{
		auto MakePhysicsQueryFilter(
			ECollisionChannel TraceChannel,
			const FCollisionQueryParams& QueryParams,
			const FCollisionResponseParams& ResponseParams) -> FPhysicsQueryFilter
		{
			FPhysicsQueryFilter Result;
			Result.Complexity = QueryParams.Complexity;
			Result.QueryChannel = static_cast<uint8>(TraceChannel);
			for (uint8 Index = 0; Index < MaximumPhysicsChannels; ++Index)
				Result.Responses[Index] = ToPhysicsResponse(ResponseParams.CollisionResponse.Responses[Index]);
			Result.IgnoredSourceObjectIDs = QueryParams.IgnoredSourceObjectIDs;
			Result.IgnoredComponentIDs = QueryParams.IgnoredComponentIDs;
			return Result;
		}
	}

	auto DWorld::LineTraceSingleByChannel(
		FHitResult& OutHit,
		const FVector3& Start,
		const FVector3& End,
		ECollisionChannel TraceChannel,
		const FCollisionQueryParams& QueryParams,
		const FCollisionResponseParams& ResponseParams) const -> bool
	{
		OutHit.Reset();
		const auto Filter = MakePhysicsQueryFilter(TraceChannel, QueryParams, ResponseParams);
		FPhysicsQueryHit Hit;
		if (!PhysicsScene.LineTraceSingle(Start, End, Filter, Hit) || !OutHit.SetFromPhysicsHit(Hit)) return false;
		if (auto* Debug = GetSubsystem<DCollisionDebugSubsystem>()) Debug->RecordHit(OutHit);
		return true;
	}

	auto DWorld::SweepSingleByChannel(
		FHitResult& OutHit,
		const FCollisionShape& Shape,
		const FTransform& StartTransform,
		const FVector3& Delta,
		ECollisionChannel TraceChannel,
		const FCollisionQueryParams& QueryParams,
		const FCollisionResponseParams& ResponseParams) const -> bool
	{
		OutHit.Reset();
		const auto Filter = MakePhysicsQueryFilter(TraceChannel, QueryParams, ResponseParams);
		FPhysicsQueryHit Hit;
		if (!PhysicsScene.SweepSingle(Shape, StartTransform, Delta, Filter, Hit) || !OutHit.SetFromPhysicsHit(Hit)) return false;
		if (auto* Debug = GetSubsystem<DCollisionDebugSubsystem>()) Debug->RecordHit(OutHit);
		return true;
	}

	auto DWorld::OverlapMultiByChannel(
		std::vector<FOverlapResult>& OutOverlaps,
		const FCollisionShape& Shape,
		const FTransform& Transform,
		ECollisionChannel TraceChannel,
		const FCollisionQueryParams& QueryParams,
		const FCollisionResponseParams& ResponseParams) const -> bool
	{
		OutOverlaps.clear();
		std::vector<FPhysicsQueryHit> Hits;
		if (!PhysicsScene.OverlapMulti(
			Shape, Transform, MakePhysicsQueryFilter(TraceChannel, QueryParams, ResponseParams), Hits)) return false;
		for (const FPhysicsQueryHit& Hit : Hits)
		{
			auto* Component = Cast<DPrimitiveComponent>(PhysicsPrivate::ResolveObjectID(Hit.ComponentID));
			if (!Component) continue;
			OutOverlaps.push_back({Component->GetOwner(), Component, Hit.Response == EPhysicsQueryResponse::Block});
		}
		return !OutOverlaps.empty();
	}

	auto DWorld::DispatchOverlapNotifications() -> void
	{
		// Callback-produced transitions are delivered at the next dispatch boundary.
		const auto Notifications = std::exchange(PendingOverlapNotifications, {});
		for (const auto& Event : Notifications)
		{
			auto Notify = [&](TObjectKey<DPrimitiveComponent> Self, TObjectKey<DPrimitiveComponent> Other) {
				auto* Component = Self.ResolveObjectPtr();
				if (!Component || Component->IsBeingDestroyed() || !Component->HasBegunPlay()
					|| Component->GetPhysicsWorld() != this) return;
				auto& Delegate = Event.bBegin ? Component->OnComponentBeginOverlap : Component->OnComponentEndOverlap;
				Delegate.Broadcast(*Component, Other);
			};
			if (!CanDispatchSubsystems()) break;
			Notify(Event.First, Event.Second);
			if (!CanDispatchSubsystems()) break;
			Notify(Event.Second, Event.First);
			if (Event.FirstActor.IsNull()) continue;

			auto Deliver = [&](TObjectKey<AActor> Self, TObjectKey<AActor> Other, auto Callback) {
				if (!CanDispatchSubsystems()) return;
				auto* Actor = Self.ResolveObjectPtr();
				if (!Actor || Actor->IsBeingDestroyed() || !Actor->HasBegunPlay() || Actor->IsEndingPlay()) return;
				const auto* Level = Cast<DLevel>(Actor->GetOuter());
				if (Level && Level->GetWorld() == this) Callback(*Actor, Other);
			};
			if (Event.bBegin)
			{
				auto Begin = [](AActor& Actor, TObjectKey<AActor> Other) { Actor.NotifyActorBeginOverlap(Other); };
				auto Broadcast = [](AActor& Actor, TObjectKey<AActor> Other) { Actor.OnActorBeginOverlap.Broadcast(Actor, Other); };
				Deliver(Event.FirstActor, Event.SecondActor, Begin);
				Deliver(Event.SecondActor, Event.FirstActor, Begin);
				Deliver(Event.FirstActor, Event.SecondActor, Broadcast);
				Deliver(Event.SecondActor, Event.FirstActor, Broadcast);
			}
			else
			{
				auto End = [](AActor& Actor, TObjectKey<AActor> Other) { Actor.NotifyActorEndOverlap(Other); };
				auto Broadcast = [](AActor& Actor, TObjectKey<AActor> Other) { Actor.OnActorEndOverlap.Broadcast(Actor, Other); };
				Deliver(Event.FirstActor, Event.SecondActor, End);
				Deliver(Event.FirstActor, Event.SecondActor, Broadcast);
				Deliver(Event.SecondActor, Event.FirstActor, End);
				Deliver(Event.SecondActor, Event.FirstActor, Broadcast);
			}
		}
	}

	auto DWorld::IsCollisionDebugDrawEnabled() const -> bool
	{
		const auto* Debug = GetSubsystem<DCollisionDebugSubsystem>();
		return Debug && Debug->IsEnabled();
	}

	auto DWorld::SetCollisionDebugDrawEnabled(bool bEnabled) -> void
	{
		if (auto* Debug = GetSubsystem<DCollisionDebugSubsystem>()) Debug->SetEnabled(bEnabled);
	}

	auto DWorld::CaptureCollisionDebugSnapshot() const -> FCollisionDebugSnapshot
	{
		constexpr size_t MaximumDebugBodies = 4096;
		constexpr uint32 MaximumDebugTriangles = 256;
		FCollisionDebugSnapshot Result;
		uint32 RemainingDebugTriangles = MaximumDebugTriangles;
		if (!IsCollisionDebugDrawEnabled()) return Result;
		for (const FPhysicsBodySnapshot& Body : PhysicsScene.CaptureBodies())
		{
			if (Result.Bodies.size() >= MaximumDebugBodies) break;
			auto* Component = Cast<DPrimitiveComponent>(PhysicsPrivate::ResolveObjectID(Body.Desc.ComponentID));
			if (!Component) continue;
			for (uint32 Index = 0; Index < 2; ++Index)
			{
				const auto& Geometry = Index == 0 ? Body.Desc.SimpleGeometry : Body.Desc.ComplexGeometry;
				const auto& Transform = Index == 0 ? Body.Desc.SimpleTransform : Body.Desc.ComplexTransform;
				if (!Geometry || Result.Bodies.size() >= MaximumDebugBodies) continue;
				FCollisionDebugBody DebugBody;
				DebugBody.Handle = Body.Handle;
				DebugBody.Representation = Index == 0 ? EPhysicsQueryComplexity::Simple : EPhysicsQueryComplexity::Complex;
				DebugBody.GeometryKind = Geometry.GetKind();
				DebugBody.ResourceIdentity = Geometry.GetIdentity();
				DebugBody.RetainedBytes = Geometry.GetRetainedBytes();
				DebugBody.Transform = Transform;
				DebugBody.ObjectChannel = static_cast<ECollisionChannel>(Body.Desc.Filter.ObjectChannel);
				DebugBody.Actor = Component->GetOwner();
				DebugBody.Component = Component;
				if (const FCollisionGeometryChild* Child = Geometry.GetChild(0))
				{
					DebugBody.Shape = Child->Shape;
					DebugBody.bHasPrimitiveShape = Child->Shape.IsValid();
				}
				Geometry.GetLocalBounds(DebugBody.LocalBoundsMinimum, DebugBody.LocalBoundsMaximum);
				DebugBody.TotalTriangles = Geometry.GetTriangleCount();
				const uint32 SampleCount = std::min(DebugBody.TotalTriangles, RemainingDebugTriangles);
				DebugBody.TriangleSample.reserve(SampleCount);
				for (uint32 Index = 0; Index < SampleCount; ++Index)
				{
					std::array<FVector3, 3> Triangle;
					if (!Geometry.GetTriangleVertices(
						Index, Triangle[0], Triangle[1], Triangle[2])) break;
					DebugBody.TriangleSample.push_back(Triangle);
				}
				RemainingDebugTriangles -= static_cast<uint32>(DebugBody.TriangleSample.size());
				Result.Bodies.push_back(std::move(DebugBody));
			}
		}
		Result.LastBlockingHit = GetSubsystem<DCollisionDebugSubsystem>()->GetLastHit();
		return Result;
	}
}
