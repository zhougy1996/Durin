#include "Viewport/ViewportPickingService.h"

#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Math/Operations.h"
#include "SceneViewProjection.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"

namespace Durin
{
	namespace
	{
		constexpr double kIntersectionEpsilon = 1.e-8;

		auto IntersectRayBox(const FVector3& Origin, const FVector3& Direction, const FBox& Box) -> bool
		{
			if (!Box.bIsValid || !Math::IsFinite(Box.Min) || !Math::IsFinite(Box.Max)) return false;
			double TMin = 0.0;
			double TMax = std::numeric_limits<double>::max();
			for (uint32 Axis = 0; Axis < 3; ++Axis)
			{
				if (std::abs(Direction[Axis]) <= kIntersectionEpsilon)
				{
					if (Origin[Axis] < Box.Min[Axis] || Origin[Axis] > Box.Max[Axis]) return false;
					continue;
				}
				double Near = (Box.Min[Axis] - Origin[Axis]) / Direction[Axis];
				double Far = (Box.Max[Axis] - Origin[Axis]) / Direction[Axis];
				if (Near > Far) std::swap(Near, Far);
				TMin = std::max(TMin, Near);
				TMax = std::min(TMax, Far);
				if (TMin > TMax) return false;
			}
			return TMax >= 0.0;
		}

		auto IntersectRayTriangle(const FVector3& Origin, const FVector3& Direction, const FVector3& A,
			const FVector3& B, const FVector3& C, double& OutDistance) -> bool
		{
			if (!Math::IsFinite(A) || !Math::IsFinite(B) || !Math::IsFinite(C)) return false;
			const FVector3 Edge1 = B - A;
			const FVector3 Edge2 = C - A;
			const FVector3 P = Math::Cross(Direction, Edge2);
			const double Determinant = Math::Dot(Edge1, P);
			if (!std::isfinite(Determinant) || std::abs(Determinant) <= kIntersectionEpsilon) return false;
			const double InvDeterminant = 1.0 / Determinant;
			const FVector3 T = Origin - A;
			const double U = Math::Dot(T, P) * InvDeterminant;
			if (U < -kIntersectionEpsilon || U > 1.0 + kIntersectionEpsilon) return false;
			const FVector3 Q = Math::Cross(T, Edge1);
			const double V = Math::Dot(Direction, Q) * InvDeterminant;
			if (V < -kIntersectionEpsilon || U + V > 1.0 + kIntersectionEpsilon) return false;
			OutDistance = Math::Dot(Edge2, Q) * InvDeterminant;
			return std::isfinite(OutDistance) && OutDistance >= 0.0;
		}

		// Defines one built-in primitive-family query without exposing it to viewport clients or modes.
		class IViewportGeometryProvider
		{
		public:
			virtual ~IViewportGeometryProvider() = default;
			virtual auto Query(const FViewportPickingBackendRequest& Request,
				const FViewportPickingTarget& Target) const -> std::optional<FViewportPickingBackendHit> = 0;
		};

		// Provides the M1 LOD0 double-sided StaticMesh oracle.
		class FStaticMeshViewportGeometryProvider final : public IViewportGeometryProvider
		{
		public:
			auto Query(const FViewportPickingBackendRequest& Request,
				const FViewportPickingTarget& Target) const -> std::optional<FViewportPickingBackendHit> override
			{
				auto* Component = Cast<DStaticMeshComponent>(Target.Component.Get());
				const DStaticMesh* Mesh = Component ? Component->GetStaticMesh() : nullptr;
				const FStaticMeshRenderData* Data = Mesh ? Mesh->GetRenderData() : nullptr;
				if (!Component || !Component->IsRegistered() || !Data || !Data->LocalBounds.bIsValid || Data->LODResources.empty()) return std::nullopt;
				const FStaticMeshLODResources& LOD = Data->LODResources[0];
				const auto& Positions = LOD.VertexBuffers.PositionVertexBuffer.GetPositions();
				const auto& Indices = LOD.IndexBuffer.GetIndices();
				if (Positions.empty() || Indices.size() < 3) return std::nullopt;
				const FMatrix LocalToWorld = Component->GetRenderMatrix();
				const double Determinant = Math::Determinant(LocalToWorld);
				if (!std::isfinite(Determinant) || std::abs(Determinant) <= kIntersectionEpsilon) return std::nullopt;
				const FMatrix WorldToLocal = Math::Inverse(LocalToWorld);
				const FVector3 LocalOrigin = FVector3(WorldToLocal * FVector4(Request.RayOrigin, 1.0));
				const FVector3 LocalDirection = FVector3(WorldToLocal * FVector4(Request.RayDirection, 0.0));
				if (!Math::IsFinite(LocalOrigin) || !Math::IsFinite(LocalDirection)
					|| !IntersectRayBox(LocalOrigin, LocalDirection, Data->LocalBounds)) return std::nullopt;
				std::optional<FViewportPickingBackendHit> Best;
				for (size_t Index = 0; Index + 2 < Indices.size(); Index += 3)
				{
					const uint32 I0 = Indices[Index];
					const uint32 I1 = Indices[Index + 1];
					const uint32 I2 = Indices[Index + 2];
					if (I0 >= Positions.size() || I1 >= Positions.size() || I2 >= Positions.size()) continue;
					double LocalDistance = 0.0;
					if (!IntersectRayTriangle(LocalOrigin, LocalDirection, FVector3(Positions[I0]),
						FVector3(Positions[I1]), FVector3(Positions[I2]), LocalDistance)) continue;
					const FVector3 LocalHit = LocalOrigin + LocalDirection * LocalDistance;
					const FVector3 WorldHit = FVector3(LocalToWorld * FVector4(LocalHit, 1.0));
					const double WorldDistance = Math::Length(WorldHit - Request.RayOrigin);
					if (!std::isfinite(WorldDistance) || WorldDistance < 0.0) continue;
					if (!Best || WorldDistance < Best->Distance)
						Best = FViewportPickingBackendHit{Target.Token, WorldDistance, 0};
				}
				return Best;
			}
		};

		// Composes an ordered built-in provider list behind the complete-or-pending backend API.
		class FReferenceViewportPickingBackend final : public IViewportPickingBackend
		{
		public:
			FReferenceViewportPickingBackend()
			{
				Providers.push_back(std::make_unique<FStaticMeshViewportGeometryProvider>());
			}

			auto Submit(FViewportPickingBackendRequest Request) -> FViewportPickingBackendCompletion override
			{
				std::optional<FViewportPickingBackendHit> Best;
				uint64 BestStableKey = std::numeric_limits<uint64>::max();
				for (const FViewportPickingTarget& Target : Request.Targets)
					for (const std::unique_ptr<IViewportGeometryProvider>& Provider : Providers)
						if (const std::optional<FViewportPickingBackendHit> Candidate = Provider->Query(Request, Target);
							Candidate && (!Best || Candidate->Distance < Best->Distance - kIntersectionEpsilon
								|| (std::abs(Candidate->Distance - Best->Distance) <= kIntersectionEpsilon
									&& Target.StableTieKey < BestStableKey)))
						{
							Best = Candidate;
							BestStableKey = Target.StableTieKey;
						}
				return {EViewportPickStatus::Completed, Best};
			}

			auto Poll(FViewportPickTicket) -> FViewportPickingBackendCompletion override
			{
				return {EViewportPickStatus::Invalid, std::nullopt};
			}

			auto Cancel(FViewportPickTicket) -> void override {}

		private:
			std::vector<std::unique_ptr<IViewportGeometryProvider>> Providers;
		};
	} // namespace

	auto IsViewportPickHitPreferred(const FViewportPickHit& Candidate, const FViewportPickHit& Current) -> bool
	{
		if (Candidate.bDepthIndependent != Current.bDepthIndependent) return Candidate.bDepthIndependent;
		if (Candidate.Distance < Current.Distance - kIntersectionEpsilon) return true;
		if (Candidate.Distance > Current.Distance + kIntersectionEpsilon) return false;
		if (Candidate.Kind != Current.Kind) return Candidate.Kind == EViewportPickHitKind::SceneGeometry;
		if (Candidate.Priority != Current.Priority) return Candidate.Priority > Current.Priority;
		return Candidate.StableTieKey < Current.StableTieKey;
	}

	FViewportPickingService::FViewportPickingService()
		: Backend(std::make_unique<FReferenceViewportPickingBackend>())
	{
	}

	FViewportPickingService::FViewportPickingService(std::unique_ptr<IViewportPickingBackend> InBackend)
		: Backend(InBackend ? std::move(InBackend) : std::make_unique<FReferenceViewportPickingBackend>())
	{
	}

	FViewportPickingService::~FViewportPickingService()
	{
		Invalidate();
	}

	auto FViewportPickingService::SetLevel(DLevel* Level) -> void
	{
		Invalidate();
		CurrentLevel = Level;
	}

	auto FViewportPickingService::Submit(FViewportPickRequest Request, std::optional<FViewportPickHit> Visualization) -> FViewportPickSubmission
	{
		const FViewportPickTicket Ticket{NextTicketId++};
		if (const auto It = PurposeTickets.find(Request.Purpose); It != PurposeTickets.end()) Cancel(It->second);
		PurposeTickets[Request.Purpose] = Ticket;
		if (!Ticket || !Request.Level.Get() || Request.Level.Get() != CurrentLevel.Get()
			|| Request.Precision != EViewportPickPrecision::ActorComponentSurface)
			return {Ticket, MakeTerminal(EViewportPickStatus::Invalid)};
		FVector3 RayOrigin;
		FVector3 RayDirection;
		if (!SceneViewProjection::BuildViewportRay(Request.View, Request.ViewportPosition, RayOrigin, RayDirection)
			|| !Math::IsFinite(RayOrigin) || !Math::IsFinite(RayDirection))
			return {Ticket, MakeTerminal(EViewportPickStatus::Invalid)};

		Request.RequestId = Ticket.Id;
		Request.ViewportGeneration = Generation;

		FRequestRecord Record;
		Record.Level = Request.Level;
		Record.Generation = Generation;
		if (Visualization && std::isfinite(Visualization->Distance) && Visualization->Distance >= 0.0)
		{
			if (DActorComponent* Component = Visualization->Component.Get())
				Record.VisualizationRegistrationGeneration = Component->GetRegistrationGeneration();
			Record.Visualization = std::move(Visualization);
		}

		if (EnumHasAnyFlags(Request.Layers, EViewportPickLayer::SceneGeometry))
		{
			DLevel* Level = Request.Level.Get();
			for (const TObjectPtr<AActor>& ActorPtr : Level->GetActors())
			{
				AActor* Actor = ActorPtr.Get();
				if (!Actor || Actor->IsHidden()) continue;
				for (const TObjectPtr<DActorComponent>& ComponentPtr : Actor->GetOwnedComponents())
				{
					auto* Component = Cast<DPrimitiveComponent>(ComponentPtr.Get());
					if (!Component || !Component->IsRegistered()) continue;
					const FPrimitiveSceneId PrimitiveId = Component->GetPrimitiveSceneId();
					if (PrimitiveId == InvalidPrimitiveSceneId) continue;
					Record.Targets.push_back({static_cast<uint32>(Record.Targets.size() + 1), PrimitiveId,
						Actor, Component, PrimitiveId.Value, Component->GetRegistrationGeneration()});
				}
			}
		}

		FViewportPickingBackendCompletion BackendCompletion{EViewportPickStatus::Completed, std::nullopt};
		if (EnumHasAnyFlags(Request.Layers, EViewportPickLayer::SceneGeometry))
			BackendCompletion = Backend->Submit({Ticket, RayOrigin, RayDirection, Record.Targets});
		FViewportPickCompletion Completion = Complete(Record, BackendCompletion);
		if (Completion.Status != EViewportPickStatus::Pending) Record.Completion = Completion;
		Requests.emplace(Ticket.Id, std::move(Record));
		return {Ticket, Completion};
	}

	auto FViewportPickingService::Poll(FViewportPickTicket Ticket) -> FViewportPickCompletion
	{
		const auto It = Requests.find(Ticket.Id);
		if (!Ticket || It == Requests.end()) return MakeTerminal(EViewportPickStatus::Invalid);
		FRequestRecord& Record = It->second;
		if (Record.Completion) return *Record.Completion;
		if (Record.Generation != Generation || Record.Level.Get() != CurrentLevel.Get())
		{
			Record.Completion = MakeTerminal(EViewportPickStatus::Invalidated);
			return *Record.Completion;
		}
		const FViewportPickingBackendCompletion BackendCompletion = Backend->Poll(Ticket);
		FViewportPickCompletion Completion = Complete(Record, BackendCompletion);
		if (Completion.Status != EViewportPickStatus::Pending) Record.Completion = Completion;
		return Completion;
	}

	auto FViewportPickingService::Cancel(FViewportPickTicket Ticket) -> void
	{
		const auto It = Requests.find(Ticket.Id);
		if (!Ticket || It == Requests.end()) return;
		Backend->Cancel(Ticket);
		It->second.Completion = MakeTerminal(EViewportPickStatus::Cancelled);
	}

	auto FViewportPickingService::Release(FViewportPickTicket Ticket) -> void
	{
		if (!Ticket) return;
		for (auto It = PurposeTickets.begin(); It != PurposeTickets.end();)
			if (It->second == Ticket) It = PurposeTickets.erase(It); else ++It;
		Requests.erase(Ticket.Id);
	}

	auto FViewportPickingService::Invalidate() -> void
	{
		for (auto& [Id, Record] : Requests)
		{
			Backend->Cancel({Id});
			Record.Completion = MakeTerminal(EViewportPickStatus::Invalidated);
		}
		PurposeTickets.clear();
		++Generation;
	}

	auto FViewportPickingService::SetBackendForTesting(std::unique_ptr<IViewportPickingBackend> InBackend) -> void
	{
		Invalidate();
		Backend = InBackend ? std::move(InBackend) : std::make_unique<FReferenceViewportPickingBackend>();
	}

	auto FViewportPickingService::Complete(FRequestRecord& Record,
		const FViewportPickingBackendCompletion& BackendCompletion) const -> FViewportPickCompletion
	{
		if (BackendCompletion.Status == EViewportPickStatus::Pending) return MakeTerminal(EViewportPickStatus::Pending);
		if (BackendCompletion.Status != EViewportPickStatus::Completed) return MakeTerminal(BackendCompletion.Status);
		std::optional<FViewportPickHit> Winner = Record.Visualization;
		if (BackendCompletion.Hit)
		{
			const auto It = std::ranges::find(Record.Targets, BackendCompletion.Hit->Token, &FViewportPickingTarget::Token);
			if (It == Record.Targets.end()) return MakeTerminal(EViewportPickStatus::Failed);
			FViewportPickHit Geometry{
				.Kind = EViewportPickHitKind::SceneGeometry,
				.PrimitiveId = It->PrimitiveId,
				.Actor = It->Actor,
				.Component = It->Component.Get(),
				.Distance = BackendCompletion.Hit->Distance,
				.Priority = BackendCompletion.Hit->Priority,
				.StableTieKey = It->StableTieKey,
			};
			if (!std::isfinite(Geometry.Distance) || Geometry.Distance < 0.0) return MakeTerminal(EViewportPickStatus::Failed);
			if (!Winner || IsViewportPickHitPreferred(Geometry, *Winner)) Winner = Geometry;
		}
		if (Winner && !ValidateHit(Record, *Winner)) return MakeTerminal(EViewportPickStatus::Invalidated);
		return {EViewportPickStatus::Completed, Winner};
	}

	auto FViewportPickingService::ValidateHit(const FRequestRecord& Record, const FViewportPickHit& Hit) const -> bool
	{
		DLevel* Level = Record.Level.Get();
		AActor* Actor = Hit.Actor.Get();
		DActorComponent* Component = Hit.Component.Get();
		if (!Level || Level != CurrentLevel.Get() || Record.Generation != Generation || !Actor || !Component
			|| Actor->IsHidden() || !Level->ContainsActor(Actor) || !Actor->OwnsComponent(Component)
			|| Component->GetOwner() != Actor || !Component->IsRegistered() || Component->IsBeingDestroyed()) return false;
		if (Hit.Kind == EViewportPickHitKind::SceneGeometry)
		{
			auto* Primitive = Cast<DPrimitiveComponent>(Component);
			if (!Primitive || Hit.PrimitiveId == InvalidPrimitiveSceneId || Primitive->GetPrimitiveSceneId() != Hit.PrimitiveId) return false;
			const auto Target = std::ranges::find(Record.Targets, Hit.PrimitiveId, &FViewportPickingTarget::PrimitiveId);
			if (Target == Record.Targets.end() || Primitive->GetRegistrationGeneration() != Target->RegistrationGeneration) return false;
		}
		else if (Component->GetRegistrationGeneration() != Record.VisualizationRegistrationGeneration) return false;
		return true;
	}

	auto FViewportPickingService::MakeTerminal(EViewportPickStatus Status) const -> FViewportPickCompletion
	{
		return {Status, std::nullopt};
	}
} // namespace Durin
