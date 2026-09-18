#include "StaticMeshLevelMutations.h"

#include "Actors/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "Editor/Transactor.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Math/Operations.h"
#include "StaticMesh/StaticMesh.h"
#include "Threading/RunnableThread.h"

#if DURIN_LEVEL_AUTHORING_TEST_FAILURE_INJECTION
#include "Operations/StaticMeshLevelMutationTestHooks.h"
#endif

namespace Durin::Editor::Level
{
	#if DURIN_LEVEL_AUTHORING_TEST_FAILURE_INJECTION
	namespace Testing
	{
		namespace
		{
			EStaticMeshLevelMutationFailurePoint GFailurePoint =
				EStaticMeshLevelMutationFailurePoint::None;
		}

		auto SetStaticMeshLevelMutationFailurePoint(
			EStaticMeshLevelMutationFailurePoint Point) -> void
		{
			GFailurePoint = Point;
		}
	}
	#endif

	namespace
	{
		#if DURIN_LEVEL_AUTHORING_TEST_FAILURE_INJECTION
		auto ConsumeInjectedFailure(Testing::EStaticMeshLevelMutationFailurePoint Point) -> bool
		{
			if (Testing::GFailurePoint != Point) return false;
			Testing::GFailurePoint = Testing::EStaticMeshLevelMutationFailurePoint::None;
			return true;
		}
		#endif

		auto MakeDiagnostic(EStaticMeshLevelMutationError Error, EStaticMeshLevelMutationReason Reason,
			size_t MutationIndex = std::numeric_limits<size_t>::max(), std::string ActorName = {})
			-> FStaticMeshLevelMutationDiagnostic
		{
			return {.Error = Error, .MutationIndex = MutationIndex, .Reason = Reason, .ActorName = std::move(ActorName)};
		}

		auto EqualTransform(const FTransform& Left, const FTransform& Right) -> bool
		{
			return Left.Translation.x == Right.Translation.x
				&& Left.Translation.y == Right.Translation.y
				&& Left.Translation.z == Right.Translation.z
				&& Left.Rotation.x == Right.Rotation.x
				&& Left.Rotation.y == Right.Rotation.y
				&& Left.Rotation.z == Right.Rotation.z
				&& Left.Rotation.w == Right.Rotation.w
				&& Left.Scale3D.x == Right.Scale3D.x
				&& Left.Scale3D.y == Right.Scale3D.y
				&& Left.Scale3D.z == Right.Scale3D.z;
		}

		auto IsFiniteTransform(const FTransform& Transform) -> bool
		{
			return Math::IsFinite(Transform.Translation)
				&& Math::IsFinite(Transform.Rotation)
				&& Math::IsFinite(Transform.Scale3D);
		}

		auto EqualState(const FStaticMeshActorMutationState& Left,
			const FStaticMeshActorMutationState& Right) -> bool
		{
			return Left.Name == Right.Name
				&& Left.StaticMesh.Get() == Right.StaticMesh.Get()
				&& EqualTransform(Left.Transform, Right.Transform)
				&& Left.bHidden == Right.bHidden;
		}

		auto CaptureState(AStaticMeshActor& Actor) -> FStaticMeshActorMutationState
		{
			return {
				.Name = Actor.GetFName(),
				.StaticMesh = Actor.GetStaticMeshComponent()->GetStaticMesh(),
				.Transform = Actor.GetActorTransform(),
				.bHidden = Actor.IsHidden(),
			};
		}

		struct FValidatedActorState
		{
			AStaticMeshActor* Actor = nullptr;
			::Durin::Editor::FTransactionCustomError Error;
			explicit operator bool() const { return Error.Code == ::Durin::Editor::ETransactionCustomError::None; }
		};

		auto ValidateState(DLevel& Level, const FStaticMeshActorMutationState& Expected) -> FValidatedActorState
		{
			AActor* Actor = Level.FindActorByName(Expected.Name);
			auto* StaticMeshActor = Cast<AStaticMeshActor>(Actor);
			if (!StaticMeshActor)
			{
				return {.Error = {.Code = Actor ? ::Durin::Editor::ETransactionCustomError::ActorType : ::Durin::Editor::ETransactionCustomError::TargetUnavailable,
					.TargetLabel = Expected.Name.ToString()}};
			}
			if (auto Supported = FStaticMeshLevelMutations::IsSupportedActor(*StaticMeshActor); !Supported)
			{
				return {.Error = std::move(Supported.Error)};
			}
			if (!EqualState(CaptureState(*StaticMeshActor), Expected))
			{
				return {.Error = {.Code = ::Durin::Editor::ETransactionCustomError::ActorChanged, .TargetLabel = Expected.Name.ToString()}};
			}
			return {.Actor = StaticMeshActor};
		}

		auto ApplyStates(DLevel& Level, std::span<const FStaticMeshActorMutationDelta> Deltas,
			bool bAfter) -> ::Durin::Editor::FTransactionCustomResult
		{
			if (DWorld* World = Level.GetWorld(); World && World->IsEndingPlay())
			{
				return {{.Code = ::Durin::Editor::ETransactionCustomError::WorldEnding}};
			}
			std::vector<AStaticMeshActor*> Sources(Deltas.size(), nullptr);
			std::unordered_set<AActor*> SourceActors;
			for (size_t Index = 0; Index < Deltas.size(); ++Index)
			{
				const auto& Source = bAfter ? Deltas[Index].Before : Deltas[Index].After;
				if (!Source) continue;
				auto Validated = ValidateState(Level, *Source);
				if (!Validated) { Validated.Error.MemberIndex = Index; return {std::move(Validated.Error)}; }
				Sources[Index] = Validated.Actor;
				SourceActors.insert(Sources[Index]);
			}

			struct FRenameJournalEntry
			{
				AStaticMeshActor* Actor = nullptr;
				FName PreviousName;
			};
			struct FUpdateJournalEntry
			{
				AStaticMeshActor* Actor = nullptr;
				FStaticMeshActorMutationState Previous;
			};
			std::vector<FRenameJournalEntry> Renames;
			std::vector<FStaticMeshActorMutationState> Removed;
			std::vector<AStaticMeshActor*> Created;
			std::vector<FUpdateJournalEntry> Updates;
			DPackage* Package = Level.GetPackage();
			const bool bPackageWasDirty = Package && Package->IsDirty();

			auto Rollback = [&]() -> bool
			{
				bool bRestored = true;
				for (auto It = Updates.rbegin(); It != Updates.rend(); ++It)
				{
					if (!Level.ContainsActor(It->Actor)) { bRestored = false; continue; }
					It->Actor->GetStaticMeshComponent()->SetStaticMesh(It->Previous.StaticMesh.Get());
					bRestored = It->Actor->SetActorTransform(It->Previous.Transform) && bRestored;
					It->Actor->SetHidden(It->Previous.bHidden);
				}
				for (auto It = Created.rbegin(); It != Created.rend(); ++It)
					if (Level.ContainsActor(*It)) bRestored = Level.DestroyActor(*It) && bRestored;
				for (auto It = Renames.rbegin(); It != Renames.rend(); ++It)
				{
					if (!Level.ContainsActor(It->Actor)) { bRestored = false; continue; }
					bRestored = Level.RenameActor(It->Actor, It->PreviousName) && bRestored;
					bRestored = It->Actor->GetFName() == It->PreviousName && bRestored;
				}
				for (const FStaticMeshActorMutationState& State : Removed)
				{
					auto* Actor = Level.SpawnActor<AStaticMeshActor>(State.Name);
					if (!Actor || Actor->GetFName() != State.Name) { bRestored = false; continue; }
					Actor->GetStaticMeshComponent()->SetStaticMesh(State.StaticMesh.Get());
					bRestored = Actor->SetActorTransform(State.Transform) && bRestored;
					Actor->SetHidden(State.bHidden);
				}
				if (Package && !bPackageWasDirty) Package->ClearDirty();
				return bRestored;
			};

			auto FailAfterMutation = [&](::Durin::Editor::FTransactionCustomError Error) -> ::Durin::Editor::FTransactionCustomResult
			{
				const bool bRestored = Rollback();
				if (!bRestored) Error.CleanupCause = std::make_shared<::Durin::Editor::FTransactionCustomError>(
					::Durin::Editor::FTransactionCustomError{.Code = ::Durin::Editor::ETransactionCustomError::RollbackIncomplete});
				return {std::move(Error)};
			};

			for (const FStaticMeshActorMutationDelta& Delta : Deltas)
			{
				const auto& Destination = bAfter ? Delta.After : Delta.Before;
				if (!Destination) continue;
				if (Destination->StaticMesh.Get() && !IsValid(Destination->StaticMesh.Get()))
				{
					return {{.Code = ::Durin::Editor::ETransactionCustomError::ResourceUnavailable, .TargetLabel = Destination->Name.ToString()}};
				}
				if (AActor* Collision = Level.FindActorByName(Destination->Name);
					Collision && !SourceActors.contains(Collision))
				{
					return {{.Code = ::Durin::Editor::ETransactionCustomError::ActorNameCollision, .TargetLabel = Destination->Name.ToString()}};
				}
			}

			std::vector<FName> TemporaryNames(Deltas.size());
			std::unordered_set<FName> ReservedNames;
			for (const TObjectPtr<AActor>& Actor : Level.GetActors())
				if (Actor) ReservedNames.insert(Actor->GetFName());
			for (const FStaticMeshActorMutationDelta& Delta : Deltas)
			{
				const auto& Destination = bAfter ? Delta.After : Delta.Before;
				if (Destination) ReservedNames.insert(Destination->Name);
			}
			for (size_t Index = 0; Index < Deltas.size(); ++Index)
			{
				const auto& Source = bAfter ? Deltas[Index].Before : Deltas[Index].After;
				const auto& Destination = bAfter ? Deltas[Index].After : Deltas[Index].Before;
				if (!Source || !Destination || Source->Name == Destination->Name) continue;
				for (uint32 Suffix = 1;; ++Suffix)
				{
					FName Candidate(std::format("__LevelMutation_{}_{}", Index, Suffix));
					if (ReservedNames.insert(Candidate).second)
					{
						TemporaryNames[Index] = Candidate;
						break;
					}
				}
			}

			// Move renames aside first so future swap/cycle lowering remains deterministic.
			for (size_t Index = 0; Index < Deltas.size(); ++Index)
			{
				const auto& Source = bAfter ? Deltas[Index].Before : Deltas[Index].After;
				const auto& Destination = bAfter ? Deltas[Index].After : Deltas[Index].Before;
				if (!Source || !Destination || Source->Name == Destination->Name) continue;
				const FName Temporary = TemporaryNames[Index];
				if (!Level.RenameActor(Sources[Index], Temporary)
					|| Sources[Index]->GetFName() != Temporary)
				{
					return {{.Code = ::Durin::Editor::ETransactionCustomError::ActorRename, .TargetLabel = Source->Name.ToString()}};
				}
				Renames.push_back({Sources[Index], Source->Name});
				#if DURIN_LEVEL_AUTHORING_TEST_FAILURE_INJECTION
				if (ConsumeInjectedFailure(Testing::EStaticMeshLevelMutationFailurePoint::AfterTemporaryRename))
					return FailAfterMutation({.Code = ::Durin::Editor::ETransactionCustomError::InjectedMutation, .MemberIndex = Index, .MutationPhase = ::Durin::Editor::ETransactionMutationPhase::TemporaryRename});
				#endif
			}

			for (size_t Index = 0; Index < Deltas.size(); ++Index)
			{
				const auto& Source = bAfter ? Deltas[Index].Before : Deltas[Index].After;
				const auto& Destination = bAfter ? Deltas[Index].After : Deltas[Index].Before;
				if (Source && !Destination)
				{
					if (!Level.DestroyActor(Sources[Index]))
						return FailAfterMutation({.Code = ::Durin::Editor::ETransactionCustomError::ActorDestroy, .TargetLabel = Source->Name.ToString(), .MemberIndex = Index});
					Removed.push_back(*Source);
					#if DURIN_LEVEL_AUTHORING_TEST_FAILURE_INJECTION
					if (ConsumeInjectedFailure(Testing::EStaticMeshLevelMutationFailurePoint::AfterRemove))
						return FailAfterMutation({.Code = ::Durin::Editor::ETransactionCustomError::InjectedMutation, .MemberIndex = Index, .MutationPhase = ::Durin::Editor::ETransactionMutationPhase::Remove});
					#endif
				}
			}

			for (size_t Index = 0; Index < Deltas.size(); ++Index)
			{
				const auto& Source = bAfter ? Deltas[Index].Before : Deltas[Index].After;
				const auto& Destination = bAfter ? Deltas[Index].After : Deltas[Index].Before;
				if (!Destination) continue;
				AStaticMeshActor* Actor = Sources[Index];
				if (!Source)
				{
					Actor = Level.SpawnActor<AStaticMeshActor>(Destination->Name);
					if (!Actor || Actor->GetFName() != Destination->Name)
					{
						if (Actor) Level.DestroyActor(Actor);
						return FailAfterMutation({.Code = ::Durin::Editor::ETransactionCustomError::ActorSpawn, .TargetLabel = Destination->Name.ToString(), .MemberIndex = Index});
					}
					Created.push_back(Actor);
					#if DURIN_LEVEL_AUTHORING_TEST_FAILURE_INJECTION
					if (ConsumeInjectedFailure(Testing::EStaticMeshLevelMutationFailurePoint::AfterCreate))
						return FailAfterMutation({.Code = ::Durin::Editor::ETransactionCustomError::InjectedMutation, .MemberIndex = Index, .MutationPhase = ::Durin::Editor::ETransactionMutationPhase::Create});
					#endif
				}
				else if (Source->Name != Destination->Name)
				{
					const FName PreviousName = Actor->GetFName();
					if (!Level.RenameActor(Actor, Destination->Name) || Actor->GetFName() != Destination->Name)
						return FailAfterMutation({.Code = ::Durin::Editor::ETransactionCustomError::ActorRename, .TargetLabel = Destination->Name.ToString(), .MemberIndex = Index});
					Renames.push_back({Actor, PreviousName});
					#if DURIN_LEVEL_AUTHORING_TEST_FAILURE_INJECTION
					if (ConsumeInjectedFailure(Testing::EStaticMeshLevelMutationFailurePoint::AfterFinalRename))
						return FailAfterMutation({.Code = ::Durin::Editor::ETransactionCustomError::InjectedMutation, .MemberIndex = Index, .MutationPhase = ::Durin::Editor::ETransactionMutationPhase::FinalRename});
					#endif
				}
				Updates.push_back({Actor, CaptureState(*Actor)});
				Actor->GetStaticMeshComponent()->SetStaticMesh(Destination->StaticMesh.Get());
				if (!Actor->SetActorTransform(Destination->Transform))
				{
					return FailAfterMutation({.Code = ::Durin::Editor::ETransactionCustomError::TransformWrite, .TargetLabel = Destination->Name.ToString(), .MemberIndex = Index});
				}
				Actor->SetHidden(Destination->bHidden);
				#if DURIN_LEVEL_AUTHORING_TEST_FAILURE_INJECTION
				if (ConsumeInjectedFailure(Testing::EStaticMeshLevelMutationFailurePoint::AfterUpdate))
					return FailAfterMutation({.Code = ::Durin::Editor::ETransactionCustomError::InjectedMutation, .MemberIndex = Index, .MutationPhase = ::Durin::Editor::ETransactionMutationPhase::Update});
				#endif
			}
			return {};
		}

		class FStaticMeshLevelMutationTransaction final : public ::Durin::Editor::ITransactionCustomChange
		{
		public:
			FStaticMeshLevelMutationTransaction(const FStaticMeshLevelMutationPlan& Plan)
				: Level(Plan.Level), Description(Plan.Description), Deltas(Plan.Deltas)
			{
				AffectedPackages.front() = Plan.Package.Get();
			}

			auto GetDescription() const -> std::string_view override { return Description; }
			auto GetOwningModule() const -> std::string_view override { return "LevelEditor"; }
			auto GetDetails(::Durin::Editor::ETransactionOperation) const -> std::string override
			{
				return std::format("Edit {} static mesh actor(s)", Deltas.size());
			}
			auto GetAffectedPackages() const -> std::span<DPackage* const> override { return AffectedPackages; }
			auto Replay(::Durin::Editor::ETransactionOperation Operation) -> ::Durin::Editor::FTransactionCustomResult override
			{ return Apply(Operation != ::Durin::Editor::ETransactionOperation::Undo); }
			auto AddReferencedObjects(FReferenceCollector& Collector) const -> void override
			{
				DObject* Object = Level.Get();
				if (Object) Collector.AddReferencedObject(Object);
				for (const FStaticMeshActorMutationDelta& Delta : Deltas)
					for (const FStaticMeshActorMutationState* State : {
						Delta.Before ? &*Delta.Before : nullptr,
						Delta.After ? &*Delta.After : nullptr})
						if (State && State->StaticMesh)
						{
							Object = State->StaticMesh.Get();
							Collector.AddReferencedObject(Object);
						}
			}

		private:
			auto Apply(bool bAfter) -> ::Durin::Editor::FTransactionCustomResult
			{
				if (!Level) return {{.Code = ::Durin::Editor::ETransactionCustomError::TargetUnavailable}};
				auto Result = ApplyStates(*Level, Deltas, bAfter);
				if (!Result)
				{
					Result.Error.TargetPath = Level->GetObjectPath();
					Result.Error.NodeCount = Deltas.size();
				}
				return Result;
			}

			TObjectPtr<DLevel> Level;
			std::string Description;
			std::vector<FStaticMeshActorMutationDelta> Deltas;
			std::array<DPackage*, 1> AffectedPackages{};
		};
	}

	auto FormatStaticMeshLevelMutationDiagnostic(const FStaticMeshLevelMutationDiagnostic& Diagnostic) -> std::string
	{
		if (Diagnostic.TransactionCause) return FormatTransactorResult(*Diagnostic.TransactionCause);
		if (Diagnostic.ReplayCause) return FormatTransactionCustomError(*Diagnostic.ReplayCause);
		if (Diagnostic.SupportCause) return FormatTransactionCustomError(*Diagnostic.SupportCause);
		switch (Diagnostic.Reason)
		{
		case EStaticMeshLevelMutationReason::TargetRequired: return "A target Level and at least one mutation are required.";
		case EStaticMeshLevelMutationReason::CapturedRequest: return "The target Level package no longer matches the captured request.";
		case EStaticMeshLevelMutationReason::TargetName: return "Mutation target names cannot be empty.";
		case EStaticMeshLevelMutationReason::RenameName: return "Rename destinations cannot be empty.";
		case EStaticMeshLevelMutationReason::DuplicateName: return "A batch cannot address the same actor name more than once.";
		case EStaticMeshLevelMutationReason::PlannedState: return "The Level changed after the operation was planned.";
		case EStaticMeshLevelMutationReason::MeshUnavailable: return std::format("StaticMesh for actor '{}' is unavailable.", Diagnostic.ActorName);
		case EStaticMeshLevelMutationReason::None: break;
		}
		switch (Diagnostic.Error)
		{
		case EStaticMeshLevelMutationError::None: return {};
		case EStaticMeshLevelMutationError::WrongThread: return "Static mesh level mutation must run on the game thread.";
		case EStaticMeshLevelMutationError::ReadOnly: return "The target Level is read-only.";
		case EStaticMeshLevelMutationError::NameConflict: return std::format("Actor name '{}' is already occupied.", Diagnostic.ActorName);
		case EStaticMeshLevelMutationError::MissingActor: return std::format("Actor '{}' does not exist.", Diagnostic.ActorName);
		case EStaticMeshLevelMutationError::InvalidTransform: return std::format("Actor '{}' has a non-finite transform.", Diagnostic.ActorName);
		case EStaticMeshLevelMutationError::UnsupportedActor: return "The actor is unsupported.";
		case EStaticMeshLevelMutationError::InvalidRequest: return "The mutation request is invalid.";
		case EStaticMeshLevelMutationError::StaleTarget: return "The target Level changed.";
		case EStaticMeshLevelMutationError::ExecutionFailed: return "The static mesh actor batch could not be applied.";
		}
		return {};
	}

	auto FStaticMeshLevelMutations::CaptureTarget(DLevel& Level)
		-> FStaticMeshLevelMutationRequest
	{
		DPackage* Package = Level.GetPackage();
		return {
			.Level = &Level,
			.ExpectedPackagePath = Package ? Package->GetPackagePath() : std::string(),
			.ExpectedPackageEditRevision = Package ? Package->GetEditRevision() : 0,
		};
	}

	auto FStaticMeshLevelMutations::IsSupportedActor(const AStaticMeshActor& Actor) -> FTransactionCustomResult
	{
		auto Reject = [&](ETransactionActorConstraint Constraint) -> FTransactionCustomResult
		{
			return {{.Code = ETransactionCustomError::ActorUnsupported,
				.TargetPath = Actor.GetObjectPath(), .TargetLabel = Actor.GetName(), .ActorConstraint = Constraint}};
		};
		if (Actor.GetClass() != AStaticMeshActor::StaticClass()) return Reject(ETransactionActorConstraint::Class);
		if (Actor.GetStaticMeshComponent() == nullptr
			|| Actor.GetRootComponent() != Actor.GetStaticMeshComponent()
			|| Actor.GetComponents().size() != 1 || !Actor.GetInstanceComponents().empty())
			return Reject(ETransactionActorConstraint::ComponentGraph);
		if (Actor.GetAttachParentActor()) return Reject(ETransactionActorConstraint::Parent);
		if (Actor.IsBeginningPlay()) return Reject(ETransactionActorConstraint::BeginningPlay);
		if (Actor.IsEndingPlay()) return Reject(ETransactionActorConstraint::EndingPlay);
		if (DLevel* Level = Cast<DLevel>(Actor.GetOuter()))
		{
			for (const TObjectPtr<AActor>& Candidate : Level->GetActors())
				if (Candidate && Candidate->GetAttachParentActor() == &Actor)
					return Reject(ETransactionActorConstraint::Children);
		}
		return {};
	}

	auto FStaticMeshLevelMutations::Plan(const FStaticMeshLevelMutationRequest& Request)
		-> FStaticMeshLevelMutationPlan
	{
		FStaticMeshLevelMutationPlan Result;
		Result.Diagnostic = {};
		if (GIsGameThreadIdInitialized && !IsInGameThread())
		{
			Result.Diagnostic = MakeDiagnostic(EStaticMeshLevelMutationError::WrongThread,
				EStaticMeshLevelMutationReason::None);
			return Result;
		}
		if (!Request.Level || Request.Mutations.empty())
		{
			Result.Diagnostic = MakeDiagnostic(EStaticMeshLevelMutationError::InvalidRequest,
				EStaticMeshLevelMutationReason::TargetRequired);
			return Result;
		}
		if (Request.bReadOnly)
		{
			Result.Diagnostic = MakeDiagnostic(EStaticMeshLevelMutationError::ReadOnly,
				EStaticMeshLevelMutationReason::None);
			return Result;
		}
		DPackage* Package = Request.Level->GetPackage();
		if (!Package || !Package->IsAssetPackage()
			|| Package->GetPackagePath() != Request.ExpectedPackagePath
			|| Package->GetEditRevision() != Request.ExpectedPackageEditRevision)
		{
			Result.Diagnostic = MakeDiagnostic(EStaticMeshLevelMutationError::StaleTarget,
				EStaticMeshLevelMutationReason::CapturedRequest);
			return Result;
		}

		Result.Level = Request.Level;
		Result.Package = Package;
		Result.PackagePath = Package->GetPackagePath();
		Result.PackageEditRevision = Package->GetEditRevision();
		Result.ActorHierarchyRevision = Request.Level->GetEditorActorHierarchyRevision();
		Result.Description = Request.Description.empty() ? "Edit static mesh actors" : Request.Description;
		std::unordered_set<FName> ClaimedNames;
		for (size_t Index = 0; Index < Request.Mutations.size(); ++Index)
		{
			const FStaticMeshLevelMutation& Mutation = Request.Mutations[Index];
			if (Mutation.TargetName.IsNone())
			{
				Result.Diagnostic = MakeDiagnostic(EStaticMeshLevelMutationError::InvalidRequest,
					EStaticMeshLevelMutationReason::TargetName, Index);
				return Result;
			}
			FStaticMeshActorMutationDelta Delta;
			AActor* Existing = Request.Level->FindActorByName(Mutation.TargetName);
			auto* StaticMeshActor = Cast<AStaticMeshActor>(Existing);
			if (Mutation.Kind == EStaticMeshLevelMutationKind::Create)
			{
				if (Existing)
				{
					Result.Diagnostic = MakeDiagnostic(EStaticMeshLevelMutationError::NameConflict,
						EStaticMeshLevelMutationReason::None, Index, Mutation.TargetName.ToString());
					return Result;
				}
				Delta.After = Mutation.Desired;
				Delta.After->Name = Mutation.TargetName;
			}
			else
			{
				if (!Existing)
				{
					Result.Diagnostic = MakeDiagnostic(EStaticMeshLevelMutationError::MissingActor,
						EStaticMeshLevelMutationReason::None, Index, Mutation.TargetName.ToString());
					return Result;
				}
				auto Supported = StaticMeshActor ? IsSupportedActor(*StaticMeshActor)
					: FTransactionCustomResult{{.Code = ETransactionCustomError::ActorType, .TargetLabel = Mutation.TargetName.ToString()}};
				if (!Supported)
				{
					Result.Diagnostic = MakeDiagnostic(EStaticMeshLevelMutationError::UnsupportedActor,
						EStaticMeshLevelMutationReason::None, Index, Mutation.TargetName.ToString());
					Result.Diagnostic.SupportCause = std::make_shared<FTransactionCustomError>(std::move(Supported.Error));
					return Result;
				}
				Delta.Before = CaptureState(*StaticMeshActor);
				if (Mutation.Kind == EStaticMeshLevelMutationKind::Update)
				{
					Delta.After = Mutation.Desired;
					Delta.After->Name = Mutation.TargetName;
				}
				else if (Mutation.Kind == EStaticMeshLevelMutationKind::Rename)
				{
					if (Mutation.Desired.Name.IsNone())
					{
						Result.Diagnostic = MakeDiagnostic(EStaticMeshLevelMutationError::InvalidRequest,
							EStaticMeshLevelMutationReason::RenameName, Index);
						return Result;
					}
					Delta.After = Delta.Before;
					Delta.After->Name = Mutation.Desired.Name;
				}
			}
			if (Delta.After && !IsFiniteTransform(Delta.After->Transform))
			{
				Result.Diagnostic = MakeDiagnostic(EStaticMeshLevelMutationError::InvalidTransform,
					EStaticMeshLevelMutationReason::None, Index, Delta.After->Name.ToString());
				return Result;
			}
			if (Delta.After && Delta.After->StaticMesh.Get() && !IsValid(Delta.After->StaticMesh.Get()))
			{
				Result.Diagnostic = MakeDiagnostic(EStaticMeshLevelMutationError::InvalidRequest,
					EStaticMeshLevelMutationReason::MeshUnavailable, Index, Delta.After->Name.ToString());
				return Result;
			}
			if (!ClaimedNames.insert(Mutation.TargetName).second
				|| (Delta.After && Delta.After->Name != Mutation.TargetName
					&& !ClaimedNames.insert(Delta.After->Name).second))
			{
				Result.Diagnostic = MakeDiagnostic(EStaticMeshLevelMutationError::NameConflict,
					EStaticMeshLevelMutationReason::DuplicateName, Index);
				return Result;
			}
			if (Delta.After && Delta.After->Name != Mutation.TargetName)
			{
				if (AActor* Collision = Request.Level->FindActorByName(Delta.After->Name);
					Collision && Collision != Existing)
				{
					Result.Diagnostic = MakeDiagnostic(EStaticMeshLevelMutationError::NameConflict,
						EStaticMeshLevelMutationReason::None, Index, Delta.After->Name.ToString());
					return Result;
				}
			}
			const bool bChanged = !Delta.Before || !Delta.After || !EqualState(*Delta.Before, *Delta.After);
			if (bChanged) Result.Deltas.push_back(std::move(Delta));
		}
		Result.bHasChanges = !Result.Deltas.empty();
		return Result;
	}

	auto FStaticMeshLevelMutations::Execute(const FStaticMeshLevelMutationPlan& Plan,
		const FStaticMeshLevelExecutionContext& Context) -> FStaticMeshLevelMutationResult
	{
		FStaticMeshLevelMutationResult Result;
		if (!Plan)
		{
			Result.Diagnostic = Plan.Diagnostic;
			return Result;
		}
		if (GIsGameThreadIdInitialized && !IsInGameThread())
		{
			Result.Diagnostic = MakeDiagnostic(EStaticMeshLevelMutationError::WrongThread,
				EStaticMeshLevelMutationReason::None);
			return Result;
		}
		DLevel* Level = Plan.Level.Get();
		DPackage* Package = Plan.Package.Get();
		if (Context.bReadOnly)
		{
			Result.Diagnostic = MakeDiagnostic(EStaticMeshLevelMutationError::ReadOnly,
				EStaticMeshLevelMutationReason::None);
			return Result;
		}
		if (!Level || Context.OpenLevel != Level || !Package || Level->GetPackage() != Package
			|| Package->GetPackagePath() != Plan.PackagePath
			|| Package->GetEditRevision() != Plan.PackageEditRevision
			|| Level->GetEditorActorHierarchyRevision() != Plan.ActorHierarchyRevision)
		{
			Result.Diagnostic = MakeDiagnostic(EStaticMeshLevelMutationError::StaleTarget,
				EStaticMeshLevelMutationReason::PlannedState);
			return Result;
		}
		if (!Plan.bHasChanges)
		{
			Result.Diagnostic = {};
			return Result;
		}
		auto Transaction = std::make_unique<FStaticMeshLevelMutationTransaction>(Plan);
		if (Context.Transactions)
		{
			auto Applied = Context.Transactions->Execute(std::move(Transaction));
			if (!Applied)
			{
				Result.Diagnostic.Error = EStaticMeshLevelMutationError::ExecutionFailed;
				Result.Diagnostic.TransactionCause = std::make_shared<::Durin::Editor::FTransactorResult>(std::move(Applied));
				return Result;
			}
		}
		else
		{
			auto Applied = Transaction->Replay(::Durin::Editor::ETransactionOperation::Redo);
			if (!Applied)
			{
				Result.Diagnostic.Error = EStaticMeshLevelMutationError::ExecutionFailed;
				Result.Diagnostic.ReplayCause = std::make_shared<::Durin::Editor::FTransactionCustomError>(std::move(Applied.Error));
				return Result;
			}
		}
		for (const FStaticMeshActorMutationDelta& Delta : Plan.Deltas)
			if (Delta.After) Result.ResultActorNames.push_back(Delta.After->Name);
		Result.bChanged = true;
		Result.Diagnostic = {};
		return Result;
	}
}
