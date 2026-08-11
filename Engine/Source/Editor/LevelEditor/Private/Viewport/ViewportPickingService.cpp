#include "Viewport/ViewportPickingService.h"

#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Math/Operations.h"
#include "SceneViewProjection.h"
#include "SkeletalMesh/SkeletalMesh.h"
#include "SkeletalMesh/SkeletalMeshResources.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"

namespace Durin
{
	namespace
	{
		constexpr double kIntersectionEpsilon = 1.e-8;
		constexpr double kInfluenceWeightEpsilon = 1.e-4;

		enum class EViewportGeometryQueryStatus : uint8
		{
			NotApplicable,
			Miss,
			Hit,
			InvalidComponent,
			Failed
		};

		struct FViewportGeometryQueryResult
		{
			EViewportGeometryQueryStatus Status = EViewportGeometryQueryStatus::NotApplicable;
			std::optional<FViewportPickingBackendHit> Hit;
		};

		struct FViewportGeometryQueryContext
		{
			FViewportPickingWorkBudget Budget;
			FViewportPickingBackendDiagnostics Diagnostics;
		};

		auto ToDoubleMatrix(const FMatrix4f& Source) -> FMatrix
		{
			FMatrix Result(0.0);
			for (uint32 Column = 0; Column < 4; ++Column)
				for (uint32 Row = 0; Row < 4; ++Row)
					Result[Column][Row] = Source[Column][Row];
			return Result;
		}

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
				const FViewportPickingTarget& Target,
				FViewportGeometryQueryContext& Context) const -> FViewportGeometryQueryResult = 0;
		};

		// Provides the M1 LOD0 double-sided StaticMesh oracle.
		class FStaticMeshViewportGeometryProvider final : public IViewportGeometryProvider
		{
		public:
			auto Query(const FViewportPickingBackendRequest& Request,
				const FViewportPickingTarget& Target,
				FViewportGeometryQueryContext&) const -> FViewportGeometryQueryResult override
			{
				auto* Component = Cast<DStaticMeshComponent>(Target.Component.Get());
				if (!Component) return {};
				const DStaticMesh* Mesh = Component ? Component->GetStaticMesh() : nullptr;
				const FStaticMeshRenderData* Data = Mesh ? Mesh->GetRenderData() : nullptr;
				if (!Component->IsRegistered() || !Data || !Data->LocalBounds.bIsValid || Data->LODResources.empty())
					return {EViewportGeometryQueryStatus::Miss, std::nullopt};
				const FStaticMeshLODResources& LOD = Data->LODResources[0];
				const auto& Positions = LOD.VertexBuffers.PositionVertexBuffer.GetPositions();
				const auto& Indices = LOD.IndexBuffer.GetIndices();
				if (Positions.empty() || Indices.size() < 3) return {EViewportGeometryQueryStatus::Miss, std::nullopt};
				const FMatrix LocalToWorld = Component->GetRenderMatrix();
				const double Determinant = Math::Determinant(LocalToWorld);
				if (!std::isfinite(Determinant) || std::abs(Determinant) <= kIntersectionEpsilon)
					return {EViewportGeometryQueryStatus::Miss, std::nullopt};
				const FMatrix WorldToLocal = Math::Inverse(LocalToWorld);
				const FVector3 LocalOrigin = FVector3(WorldToLocal * FVector4(Request.RayOrigin, 1.0));
				const FVector3 LocalDirection = FVector3(WorldToLocal * FVector4(Request.RayDirection, 0.0));
				if (!Math::IsFinite(LocalOrigin) || !Math::IsFinite(LocalDirection)
					|| !IntersectRayBox(LocalOrigin, LocalDirection, Data->LocalBounds))
					return {EViewportGeometryQueryStatus::Miss, std::nullopt};
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
				return {Best ? EViewportGeometryQueryStatus::Hit : EViewportGeometryQueryStatus::Miss, Best};
			}
		};

		// Provides exact current-pose LOD0 double-sided skeletal surface intersection.
		class FSkeletalMeshViewportGeometryProvider final : public IViewportGeometryProvider
		{
		public:
			auto Query(const FViewportPickingBackendRequest& Request,
				const FViewportPickingTarget& Target,
				FViewportGeometryQueryContext& Context) const -> FViewportGeometryQueryResult override
			{
				auto* Component = Cast<DSkeletalMeshComponent>(Target.Component.Get());
				if (!Component) return {};
				++Context.Diagnostics.ApplicableSkeletalTargets;
				const auto Invalid = [&Context]()
				{
					++Context.Diagnostics.InvalidSkeletalTargets;
					return FViewportGeometryQueryResult{EViewportGeometryQueryStatus::InvalidComponent, std::nullopt};
				};

				const DSkeletalMesh* Mesh = Component->GetSkeletalMesh();
				const FSkeletalMeshRenderData* Data = Mesh ? Mesh->GetRenderData() : nullptr;
				const std::shared_ptr<const FSkeletalPosePalette> Pose = Component->GetLatestPosePalette();
				if (!Component->IsRegistered() || !Mesh || !Data || Data->LODIndex != 0 || !Pose
					|| Pose->Revision == 0 || Mesh->GetSkeletonCompatibilityIdentity().empty()
					|| Pose->SkeletonCompatibilityIdentity != Mesh->GetSkeletonCompatibilityIdentity()
					|| Data->PaletteBoneIndices.empty()
					|| Pose->Matrices.size() != Data->PaletteBoneIndices.size()
					|| !Pose->LocalBounds.bIsValid || !Math::IsFinite(Pose->LocalBounds.Min)
					|| !Math::IsFinite(Pose->LocalBounds.Max)
					|| Pose->LocalBounds.Min.x > Pose->LocalBounds.Max.x
					|| Pose->LocalBounds.Min.y > Pose->LocalBounds.Max.y
					|| Pose->LocalBounds.Min.z > Pose->LocalBounds.Max.z) return Invalid();

				const auto& Positions = Data->VertexBuffers.Geometry.PositionVertexBuffer.GetPositions();
				const auto& Influences = Data->VertexBuffers.InfluenceVertexBuffer.GetInfluences();
				const auto& Indices = Data->IndexBuffer.GetIndices();
				if (Positions.empty() || Positions.size() != Influences.size() || Indices.empty()
					|| Indices.size() % 3 != 0 || Data->Sections.empty()) return Invalid();

				std::unordered_map<uint16, size_t> BoneToPalette;
				std::vector<FMatrix> PaletteMatrices;
				PaletteMatrices.reserve(Pose->Matrices.size());
				for (size_t PaletteIndex = 0; PaletteIndex < Data->PaletteBoneIndices.size(); ++PaletteIndex)
				{
					FMatrix Matrix = ToDoubleMatrix(Pose->Matrices[PaletteIndex]);
					if (!Math::IsFinite(Matrix)
						|| !BoneToPalette.emplace(Data->PaletteBoneIndices[PaletteIndex], PaletteIndex).second)
						return Invalid();
					PaletteMatrices.push_back(std::move(Matrix));
				}

				uint64 CoveredIndices = 0;
				for (const FSkeletalMeshRenderSection& Section : Data->Sections)
				{
					const uint64 End = static_cast<uint64>(Section.FirstIndex) + Section.IndexCount;
					if (Section.FirstIndex != CoveredIndices || Section.IndexCount == 0
						|| Section.IndexCount % 3 != 0 || End > Indices.size()
						|| Section.MinVertexIndex > Section.MaxVertexIndex
						|| Section.MaxVertexIndex >= Positions.size()) return Invalid();
					CoveredIndices = End;
				}
				if (CoveredIndices != Indices.size()) return Invalid();

				std::vector<uint8> Referenced(Positions.size(), 0);
				uint64 ReferencedVertexCount = 0;
				for (uint32 Index : Indices)
				{
					if (Index >= Positions.size()) return Invalid();
					if (!Referenced[Index])
					{
						Referenced[Index] = 1;
						++ReferencedVertexCount;
					}
				}
				for (size_t VertexIndex = 0; VertexIndex < Positions.size(); ++VertexIndex)
				{
					if (!Math::IsFinite(FVector3(Positions[VertexIndex]))) return Invalid();
					const FSkeletalMeshVertexInfluences& VertexInfluences = Influences[VertexIndex];
					if (VertexInfluences.Count == 0 || VertexInfluences.Count > MaximumSkeletalMeshInfluences) return Invalid();
					double WeightSum = 0.0;
					for (uint8 InfluenceIndex = 0; InfluenceIndex < VertexInfluences.Count; ++InfluenceIndex)
					{
						const float Weight = VertexInfluences.Weights[InfluenceIndex];
						if (!std::isfinite(Weight) || Weight <= 0.0f
							|| !BoneToPalette.contains(VertexInfluences.BoneIndices[InfluenceIndex])) return Invalid();
						WeightSum += Weight;
					}
					if (!std::isfinite(WeightSum) || std::abs(WeightSum - 1.0) > kInfluenceWeightEpsilon) return Invalid();
				}

				const FMatrix LocalToWorld = Component->GetRenderMatrix();
				FMatrix WorldToLocal(1.0);
				if (!Math::TryInverse(LocalToWorld, WorldToLocal, kIntersectionEpsilon)) return Invalid();
				const FVector3 LocalOrigin(WorldToLocal * FVector4(Request.RayOrigin, 1.0));
				const FVector3 LocalDirection(WorldToLocal * FVector4(Request.RayDirection, 0.0));
				if (!Math::IsFinite(LocalOrigin) || !Math::IsFinite(LocalDirection)) return Invalid();
				if (!IntersectRayBox(LocalOrigin, LocalDirection, Pose->LocalBounds))
				{
					++Context.Diagnostics.SkeletalBoundsRejects;
					return {EViewportGeometryQueryStatus::Miss, std::nullopt};
				}

				const uint64 TriangleCount = static_cast<uint64>(Indices.size() / 3);
				if (ReferencedVertexCount > Context.Budget.MaximumSkinnedVertices - std::min(
					Context.Diagnostics.SkinnedVertices, Context.Budget.MaximumSkinnedVertices)
					|| TriangleCount > Context.Budget.MaximumTestedTriangles - std::min(
					Context.Diagnostics.TestedTriangles, Context.Budget.MaximumTestedTriangles))
				{
					++Context.Diagnostics.SkeletalBudgetFailures;
					return {EViewportGeometryQueryStatus::Failed, std::nullopt};
				}
				Context.Diagnostics.SkinnedVertices += ReferencedVertexCount;
				Context.Diagnostics.TestedTriangles += TriangleCount;

				std::vector<FVector3> Skinned(Positions.size(), FVector3(0.0));
				for (size_t VertexIndex = 0; VertexIndex < Positions.size(); ++VertexIndex)
				{
					if (!Referenced[VertexIndex]) continue;
					const FSkeletalMeshVertexInfluences& VertexInfluences = Influences[VertexIndex];
					for (uint8 InfluenceIndex = 0; InfluenceIndex < VertexInfluences.Count; ++InfluenceIndex)
					{
						const size_t PaletteIndex = BoneToPalette.at(VertexInfluences.BoneIndices[InfluenceIndex]);
						Skinned[VertexIndex] += static_cast<double>(VertexInfluences.Weights[InfluenceIndex])
							* FVector3(PaletteMatrices[PaletteIndex]
								* FVector4(FVector3(Positions[VertexIndex]), 1.0));
					}
					if (!Math::IsFinite(Skinned[VertexIndex])) return Invalid();
				}

				std::optional<FViewportPickingBackendHit> Best;
				for (size_t Index = 0; Index < Indices.size(); Index += 3)
				{
					double LocalDistance = 0.0;
					if (!IntersectRayTriangle(LocalOrigin, LocalDirection, Skinned[Indices[Index]],
						Skinned[Indices[Index + 1]], Skinned[Indices[Index + 2]], LocalDistance)) continue;
					const FVector3 WorldHit(LocalToWorld * FVector4(LocalOrigin + LocalDirection * LocalDistance, 1.0));
					const double WorldDistance = Math::Length(WorldHit - Request.RayOrigin);
					if (!std::isfinite(WorldDistance) || WorldDistance < 0.0) continue;
					if (!Best || WorldDistance < Best->Distance)
						Best = FViewportPickingBackendHit{Target.Token, WorldDistance, 0};
				}
				return {Best ? EViewportGeometryQueryStatus::Hit : EViewportGeometryQueryStatus::Miss, Best};
			}
		};

		// Composes an ordered built-in provider list behind the complete-or-pending backend API.
		class FReferenceViewportPickingBackend final : public IViewportPickingBackend
		{
		public:
			explicit FReferenceViewportPickingBackend(FViewportPickingWorkBudget InWorkBudget)
				: WorkBudget(InWorkBudget)
			{
				Providers.push_back(std::make_unique<FStaticMeshViewportGeometryProvider>());
				Providers.push_back(std::make_unique<FSkeletalMeshViewportGeometryProvider>());
			}

			auto Submit(FViewportPickingBackendRequest Request) -> FViewportPickingBackendCompletion override
			{
				FViewportGeometryQueryContext Context{WorkBudget};
				std::optional<FViewportPickingBackendHit> Best;
				uint64 BestStableKey = std::numeric_limits<uint64>::max();
				for (const FViewportPickingTarget& Target : Request.Targets)
					for (const std::unique_ptr<IViewportGeometryProvider>& Provider : Providers)
					{
						const FViewportGeometryQueryResult Result = Provider->Query(Request, Target, Context);
						if (Result.Status == EViewportGeometryQueryStatus::Failed)
							return {EViewportPickStatus::Failed, std::nullopt, Context.Diagnostics};
						if (const std::optional<FViewportPickingBackendHit>& Candidate = Result.Hit;
							Candidate && (!Best || Candidate->Distance < Best->Distance - kIntersectionEpsilon
								|| (std::abs(Candidate->Distance - Best->Distance) <= kIntersectionEpsilon
									&& Target.StableTieKey < BestStableKey)))
						{
							Best = Candidate;
							BestStableKey = Target.StableTieKey;
						}
					}
				return {EViewportPickStatus::Completed, Best, Context.Diagnostics};
			}

			auto Poll(FViewportPickTicket) -> FViewportPickingBackendCompletion override
			{
				return {EViewportPickStatus::Invalid, std::nullopt};
			}

			auto Cancel(FViewportPickTicket) -> void override {}

		private:
			std::vector<std::unique_ptr<IViewportGeometryProvider>> Providers;
			FViewportPickingWorkBudget WorkBudget;
		};
	} // namespace

	auto MakeReferenceViewportPickingBackend(FViewportPickingWorkBudget WorkBudget)
		-> std::unique_ptr<IViewportPickingBackend>
	{
		return std::make_unique<FReferenceViewportPickingBackend>(WorkBudget);
	}

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
		: Backend(MakeReferenceViewportPickingBackend())
	{
	}

	FViewportPickingService::FViewportPickingService(std::unique_ptr<IViewportPickingBackend> InBackend)
		: Backend(InBackend ? std::move(InBackend) : MakeReferenceViewportPickingBackend())
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
		Backend = InBackend ? std::move(InBackend) : MakeReferenceViewportPickingBackend();
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
