#include "Engine/World.h"

#include "Components/PrimitiveComponent.h"
#include "Components/TerrainComponent.h"
#include "Engine/Actor.h"

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
			Result.QueryChannel = static_cast<uint8>(TraceChannel);
			for (uint8 Index = 0; Index < MaximumPhysicsChannels; ++Index)
				Result.Responses[Index] = ToPhysicsResponse(ResponseParams.CollisionResponse.Responses[Index]);
			for (const DPrimitiveComponent* Component : QueryParams.IgnoredComponents)
			{
				if (Component && Component->GetPhysicsActorHandle().IsValid())
					Result.IgnoredActors.push_back(Component->GetPhysicsActorHandle());
			}
			return Result;
		}

		auto IsIgnoredByOwner(
			const DPrimitiveComponent* Component,
			const FCollisionQueryParams& QueryParams) -> bool
		{
			return Component && std::ranges::find(
				QueryParams.IgnoredActors, Component->GetOwner()) != QueryParams.IgnoredActors.end();
		}

		auto MapPhysicsHit(const FPhysicsQueryHit& Source, FHitResult& OutHit) -> bool
		{
			auto* Component = reinterpret_cast<DPrimitiveComponent*>(Source.UserToken);
			if (!Component) return false;
			OutHit.bBlockingHit = Source.Response == EPhysicsQueryResponse::Block;
			OutHit.bStartPenetrating = Source.bStartPenetrating;
			OutHit.Time = Source.Time;
			OutHit.Distance = Source.Distance;
			OutHit.Location = Source.Location;
			OutHit.ImpactPoint = Source.ImpactPoint;
			OutHit.ImpactNormal = Source.ImpactNormal;
			OutHit.PenetrationDepth = Source.PenetrationDepth;
			OutHit.Component = Component;
			OutHit.Actor = Component->GetOwner();
			return true;
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
		FPhysicsQueryFilter Filter = MakePhysicsQueryFilter(TraceChannel, QueryParams, ResponseParams);
		while (true)
		{
			FPhysicsQueryHit Hit;
			if (!PhysicsScene.LineTraceSingle(Start, End, Filter, Hit)) return false;
			auto* Component = reinterpret_cast<DPrimitiveComponent*>(Hit.UserToken);
			if (!IsIgnoredByOwner(Component, QueryParams))
			{
				const bool bMapped = MapPhysicsHit(Hit, OutHit);
				if (bMapped && bCollisionDebugDrawEnabled) LastCollisionDebugHit = OutHit;
				return bMapped;
			}
			Filter.IgnoredActors.push_back(Hit.ActorHandle);
		}
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
		FPhysicsQueryFilter Filter = MakePhysicsQueryFilter(TraceChannel, QueryParams, ResponseParams);
		while (true)
		{
			FPhysicsQueryHit Hit;
			if (!PhysicsScene.SweepSingle(Shape, StartTransform, Delta, Filter, Hit)) return false;
			auto* Component = reinterpret_cast<DPrimitiveComponent*>(Hit.UserToken);
			if (!IsIgnoredByOwner(Component, QueryParams))
			{
				const bool bMapped = MapPhysicsHit(Hit, OutHit);
				if (bMapped && bCollisionDebugDrawEnabled) LastCollisionDebugHit = OutHit;
				return bMapped;
			}
			Filter.IgnoredActors.push_back(Hit.ActorHandle);
		}
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
			auto* Component = reinterpret_cast<DPrimitiveComponent*>(Hit.UserToken);
			if (!Component || IsIgnoredByOwner(Component, QueryParams)) continue;
			OutOverlaps.push_back({Component->GetOwner(), Component, Hit.Response == EPhysicsQueryResponse::Block});
		}
		return !OutOverlaps.empty();
	}

	auto DWorld::SetCollisionDebugDrawEnabled(bool bEnabled) -> void
	{
		bCollisionDebugDrawEnabled = bEnabled;
		if (!bEnabled)
		{
			LastCollisionDebugHit.reset();
			return;
		}
		if (!CurrentLevel) return;
		for (const TObjectPtr<AActor>& Actor : CurrentLevel->GetActors())
		{
			if (!Actor) continue;
			for (const TObjectPtr<DActorComponent>& Component : Actor->GetComponents())
				if (auto* Terrain = Cast<DTerrainComponent>(Component.Get()); Terrain && Terrain->IsRegistered())
					(void)Terrain->RequestPhysicsStateCreation(false);
		}
	}

	auto DWorld::CaptureCollisionDebugSnapshot() const -> FCollisionDebugSnapshot
	{
		constexpr size_t MaximumDebugBodies = 4096;
		constexpr uint32 MaximumDebugTriangles = 256;
		constexpr uint32 MaximumDebugHeightFieldNodes = 64;
		FCollisionDebugSnapshot Result;
		uint32 RemainingDebugTriangles = MaximumDebugTriangles;
		uint32 RemainingDebugHeightFieldNodes = MaximumDebugHeightFieldNodes;
		if (!bCollisionDebugDrawEnabled) return Result;
		for (const FPhysicsBodySnapshot& Body : PhysicsScene.CaptureBodies())
		{
			if (Result.Bodies.size() >= MaximumDebugBodies) break;
			auto* Component = reinterpret_cast<DPrimitiveComponent*>(Body.Desc.UserToken);
			if (!Component) continue;
			FCollisionDebugBody DebugBody;
			DebugBody.Handle = Body.Handle;
			DebugBody.GeometryKind = Body.Desc.Geometry.GetKind();
			DebugBody.ResourceIdentity = Body.Desc.Geometry.GetIdentity();
			DebugBody.RetainedBytes = Body.Desc.Geometry.GetRetainedBytes();
			DebugBody.Transform = Body.Desc.Transform;
			DebugBody.ObjectChannel = static_cast<ECollisionChannel>(Body.Desc.Filter.ObjectChannel);
			DebugBody.Actor = Component->GetOwner();
			DebugBody.Component = Component;
			if (const FCollisionGeometryChild* Child = Body.Desc.Geometry.GetChild(0))
			{
				DebugBody.Shape = Child->Shape;
				DebugBody.bHasPrimitiveShape = true;
			}
			Body.Desc.Geometry.GetLocalBounds(DebugBody.LocalBoundsMinimum, DebugBody.LocalBoundsMaximum);
			DebugBody.TotalTriangles = Body.Desc.Geometry.GetTriangleCount();
			if (DebugBody.GeometryKind == ECollisionGeometryKind::HeightField)
			{
				DebugBody.HeightFieldWidth = Body.Desc.Geometry.GetHeightFieldWidth();
				DebugBody.HeightFieldHeight = Body.Desc.Geometry.GetHeightFieldHeight();
				DebugBody.HeightFieldNodes = Body.Desc.Geometry.GetNodeCount();
				DebugBody.HeightFieldRegions = Body.Desc.Geometry.GetHeightFieldRegionCount();
				const uint32 NodeSampleCount = std::min(
					DebugBody.HeightFieldNodes, RemainingDebugHeightFieldNodes);
				DebugBody.HeightFieldNodeBoundsSample.reserve(NodeSampleCount);
				for (uint32 Index = 0; Index < NodeSampleCount; ++Index)
				{
					const FCollisionGeometryNode* Node = Body.Desc.Geometry.GetNode(Index);
					if (!Node) break;
					DebugBody.HeightFieldNodeBoundsSample.push_back({
						FVector3(Node->Minimum), FVector3(Node->Maximum)});
				}
				RemainingDebugHeightFieldNodes -= static_cast<uint32>(
					DebugBody.HeightFieldNodeBoundsSample.size());
			}
			const uint32 SampleCount = std::min(DebugBody.TotalTriangles, RemainingDebugTriangles);
			DebugBody.TriangleSample.reserve(SampleCount);
			for (uint32 Index = 0; Index < SampleCount; ++Index)
			{
				std::array<FVector3, 3> Triangle;
				if (!Body.Desc.Geometry.GetTriangleVertices(
					Index, Triangle[0], Triangle[1], Triangle[2])) break;
				DebugBody.TriangleSample.push_back(Triangle);
			}
			RemainingDebugTriangles -= static_cast<uint32>(DebugBody.TriangleSample.size());
			Result.Bodies.push_back(std::move(DebugBody));
		}
		Result.LastBlockingHit = LastCollisionDebugHit;
		return Result;
	}
}
