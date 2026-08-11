#include "Physics/PhysicsScene.h"

#include "Collision/CollisionGeometry.h"

namespace Durin
{
	namespace
	{
		auto IsIgnored(FPhysicsActorHandle Handle, const FPhysicsQueryFilter& Filter) -> bool
		{
			return std::ranges::find(Filter.IgnoredActors, Handle) != Filter.IgnoredActors.end();
		}

		auto ResolveResponse(const FPhysicsFilterData& Body, const FPhysicsQueryFilter& Query)
			-> EPhysicsQueryResponse
		{
			if (Query.QueryChannel >= MaximumPhysicsChannels || Body.ObjectChannel >= MaximumPhysicsChannels)
				return EPhysicsQueryResponse::Ignore;
			const EPhysicsQueryResponse BodyResponse = Body.Responses[Query.QueryChannel];
			const EPhysicsQueryResponse QueryResponse = Query.Responses[Body.ObjectChannel];
			if (BodyResponse == EPhysicsQueryResponse::Ignore || QueryResponse == EPhysicsQueryResponse::Ignore)
				return EPhysicsQueryResponse::Ignore;
			if (BodyResponse == EPhysicsQueryResponse::Overlap || QueryResponse == EPhysicsQueryResponse::Overlap)
				return EPhysicsQueryResponse::Overlap;
			return EPhysicsQueryResponse::Block;
		}

		auto IsCloser(const FPhysicsQueryHit& Candidate, const FPhysicsQueryHit& Current) -> bool
		{
			return !Current.IsHit() || Candidate.Time < Current.Time
				|| (Candidate.Time == Current.Time && Candidate.ActorHandle < Current.ActorHandle);
		}
	}

	FPhysicsScene::FPhysicsScene()
		: OwningThread(std::this_thread::get_id())
	{
	}

	FPhysicsScene::~FPhysicsScene() = default;

	auto FPhysicsScene::AddBody(const FPhysicsBodyDesc& Desc) -> FPhysicsActorHandle
	{
		if (!IsOwningThread() || !Desc.Shape.IsValid() || !IsValidPhysicsTransform(Desc.Transform)
			|| Desc.Filter.ObjectChannel >= MaximumPhysicsChannels || NextHandleId == 0) return {};
		const FPhysicsActorHandle Handle{NextHandleId++, 1};
		Bodies.push_back({Handle, Desc});
		return Handle;
	}

	auto FPhysicsScene::RemoveBody(FPhysicsActorHandle Handle) -> bool
	{
		if (!IsOwningThread()) return false;
		const auto It = std::ranges::find(Bodies, Handle, &FBodyRecord::Handle);
		if (It == Bodies.end()) return false;
		Bodies.erase(It);
		return true;
	}

	auto FPhysicsScene::UpdateBody(FPhysicsActorHandle Handle, const FPhysicsBodyDesc& Desc) -> bool
	{
		if (!IsOwningThread() || !Desc.Shape.IsValid() || !IsValidPhysicsTransform(Desc.Transform)
			|| Desc.Filter.ObjectChannel >= MaximumPhysicsChannels) return false;
		FBodyRecord* Body = FindBody(Handle);
		if (!Body) return false;
		Body->Desc = Desc;
		return true;
	}

	auto FPhysicsScene::ContainsBody(FPhysicsActorHandle Handle) const -> bool
	{
		return IsOwningThread() && FindBody(Handle) != nullptr;
	}

	auto FPhysicsScene::GetBodyCount() const -> size_t
	{
		return IsOwningThread() ? Bodies.size() : 0;
	}

	auto FPhysicsScene::CaptureBodies() const -> std::vector<FPhysicsBodySnapshot>
	{
		std::vector<FPhysicsBodySnapshot> Result;
		if (!IsOwningThread()) return Result;
		Result.reserve(Bodies.size());
		for (const FBodyRecord& Body : Bodies) Result.push_back({Body.Handle, Body.Desc});
		return Result;
	}

	auto FPhysicsScene::LineTraceSingle(
		const FVector3& Start,
		const FVector3& End,
		const FPhysicsQueryFilter& Filter,
		FPhysicsQueryHit& OutHit) const -> bool
	{
		OutHit = {};
		if (!IsOwningThread() || !Math::IsFinite(Start) || !Math::IsFinite(End)
			|| Filter.QueryChannel >= MaximumPhysicsChannels) return false;
		for (const FBodyRecord& Body : Bodies)
		{
			if (IsIgnored(Body.Handle, Filter)) continue;
			const EPhysicsQueryResponse Response = ResolveResponse(Body.Desc.Filter, Filter);
			if (Response != EPhysicsQueryResponse::Block) continue;
			FPhysicsQueryHit Candidate;
			if (!CollisionGeometry::RaycastBox(Start, End, Body.Desc.Shape, Body.Desc.Transform, Candidate)) continue;
			Candidate.ActorHandle = Body.Handle;
			Candidate.Response = Response;
			Candidate.Distance = Math::Length(End - Start) * Candidate.Time;
			Candidate.UserToken = Body.Desc.UserToken;
			if (IsCloser(Candidate, OutHit)) OutHit = Candidate;
		}
		return OutHit.IsHit();
	}

	auto FPhysicsScene::SweepSingle(
		const FCollisionShape& Shape,
		const FTransform& StartTransform,
		const FVector3& Delta,
		const FPhysicsQueryFilter& Filter,
		FPhysicsQueryHit& OutHit) const -> bool
	{
		OutHit = {};
		if (!IsOwningThread() || !Shape.IsValid() || !IsValidPhysicsTransform(StartTransform)
			|| !Math::IsFinite(Delta) || Filter.QueryChannel >= MaximumPhysicsChannels) return false;
		for (const FBodyRecord& Body : Bodies)
		{
			if (IsIgnored(Body.Handle, Filter)) continue;
			const EPhysicsQueryResponse Response = ResolveResponse(Body.Desc.Filter, Filter);
			if (Response != EPhysicsQueryResponse::Block) continue;
			FPhysicsQueryHit Candidate;
			if (!CollisionGeometry::SweepCapsuleBox(
				Shape, StartTransform, Delta, Body.Desc.Shape, Body.Desc.Transform, Candidate)) continue;
			Candidate.ActorHandle = Body.Handle;
			Candidate.Response = Response;
			Candidate.Distance = Math::Length(Delta) * Candidate.Time;
			Candidate.UserToken = Body.Desc.UserToken;
			if (IsCloser(Candidate, OutHit)) OutHit = Candidate;
		}
		return OutHit.IsHit();
	}

	auto FPhysicsScene::OverlapMulti(
		const FCollisionShape& Shape,
		const FTransform& Transform,
		const FPhysicsQueryFilter& Filter,
		std::vector<FPhysicsQueryHit>& OutHits) const -> bool
	{
		OutHits.clear();
		if (!IsOwningThread() || !Shape.IsValid() || !IsValidPhysicsTransform(Transform)
			|| Filter.QueryChannel >= MaximumPhysicsChannels) return false;
		for (const FBodyRecord& Body : Bodies)
		{
			if (IsIgnored(Body.Handle, Filter)) continue;
			const EPhysicsQueryResponse Response = ResolveResponse(Body.Desc.Filter, Filter);
			if (Response == EPhysicsQueryResponse::Ignore) continue;
			FPhysicsQueryHit Hit;
			if (!CollisionGeometry::OverlapCapsuleBox(
				Shape, Transform, Body.Desc.Shape, Body.Desc.Transform, Hit)) continue;
			Hit.ActorHandle = Body.Handle;
			Hit.Response = Response;
			Hit.UserToken = Body.Desc.UserToken;
			OutHits.push_back(Hit);
		}
		std::ranges::sort(OutHits, {}, &FPhysicsQueryHit::ActorHandle);
		return !OutHits.empty();
	}

	auto FPhysicsScene::IsOwningThread() const -> bool
	{
		return std::this_thread::get_id() == OwningThread;
	}

	auto FPhysicsScene::FindBody(FPhysicsActorHandle Handle) -> FBodyRecord*
	{
		const auto It = std::ranges::find(Bodies, Handle, &FBodyRecord::Handle);
		return It != Bodies.end() ? &*It : nullptr;
	}

	auto FPhysicsScene::FindBody(FPhysicsActorHandle Handle) const -> const FBodyRecord*
	{
		const auto It = std::ranges::find(Bodies, Handle, &FBodyRecord::Handle);
		return It != Bodies.end() ? &*It : nullptr;
	}
}
