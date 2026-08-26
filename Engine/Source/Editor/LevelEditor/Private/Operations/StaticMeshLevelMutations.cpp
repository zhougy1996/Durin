#include "StaticMeshLevelMutations.h"

#include "Actors/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "Editor/Transaction.h"
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

		auto MakeDiagnostic(EStaticMeshLevelMutationError Error, std::string Message,
			size_t MutationIndex = std::numeric_limits<size_t>::max())
			-> FStaticMeshLevelMutationDiagnostic
		{
			return {.Error = Error, .MutationIndex = MutationIndex, .Message = std::move(Message)};
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

		auto ValidateState(DLevel& Level, const FStaticMeshActorMutationState& Expected,
			std::string& OutError) -> AStaticMeshActor*
		{
			AActor* Actor = Level.FindActorByName(Expected.Name);
			auto* StaticMeshActor = Cast<AStaticMeshActor>(Actor);
			if (!StaticMeshActor)
			{
				OutError = Actor
					? std::format("Actor '{}' is no longer a StaticMeshActor.", Expected.Name.ToString())
					: std::format("Actor '{}' no longer exists.", Expected.Name.ToString());
				return nullptr;
			}
			if (!FStaticMeshLevelMutations::IsSupportedActor(*StaticMeshActor, &OutError)) return nullptr;
			if (!EqualState(CaptureState(*StaticMeshActor), Expected))
			{
				OutError = std::format("Actor '{}' changed after the operation was planned.", Expected.Name.ToString());
				return nullptr;
			}
			return StaticMeshActor;
		}

		auto ApplyStates(DLevel& Level, std::span<const FStaticMeshActorMutationDelta> Deltas,
			bool bAfter, std::string& OutError) -> bool
		{
			if (DWorld* World = Level.GetWorld(); World && World->IsEndingPlay())
			{
				OutError = "The target World is ending play.";
				return false;
			}
			std::vector<AStaticMeshActor*> Sources(Deltas.size(), nullptr);
			std::unordered_set<AActor*> SourceActors;
			for (size_t Index = 0; Index < Deltas.size(); ++Index)
			{
				const auto& Source = bAfter ? Deltas[Index].Before : Deltas[Index].After;
				if (!Source) continue;
				Sources[Index] = ValidateState(Level, *Source, OutError);
				if (!Sources[Index]) return false;
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

			auto FailAfterMutation = [&](std::string Message) -> bool
			{
				const bool bRestored = Rollback();
				OutError = bRestored ? std::move(Message)
					: std::format("{} Rollback also failed.", Message);
				return false;
			};

			for (const FStaticMeshActorMutationDelta& Delta : Deltas)
			{
				const auto& Destination = bAfter ? Delta.After : Delta.Before;
				if (!Destination) continue;
				if (Destination->StaticMesh.Get() && !IsValid(Destination->StaticMesh.Get()))
				{
					OutError = std::format("StaticMesh for actor '{}' is no longer available.", Destination->Name.ToString());
					return false;
				}
				if (AActor* Collision = Level.FindActorByName(Destination->Name);
					Collision && !SourceActors.contains(Collision))
				{
					OutError = std::format("Actor name '{}' is now occupied.", Destination->Name.ToString());
					return false;
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
					OutError = std::format("Failed to reserve a temporary name for '{}'.", Source->Name.ToString());
					return false;
				}
				Renames.push_back({Sources[Index], Source->Name});
				#if DURIN_LEVEL_AUTHORING_TEST_FAILURE_INJECTION
				if (ConsumeInjectedFailure(Testing::EStaticMeshLevelMutationFailurePoint::AfterTemporaryRename))
					return FailAfterMutation("Injected failure after temporary rename.");
				#endif
			}

			for (size_t Index = 0; Index < Deltas.size(); ++Index)
			{
				const auto& Source = bAfter ? Deltas[Index].Before : Deltas[Index].After;
				const auto& Destination = bAfter ? Deltas[Index].After : Deltas[Index].Before;
				if (Source && !Destination)
				{
					if (!Level.DestroyActor(Sources[Index]))
						return FailAfterMutation(std::format("Failed to remove actor '{}'.", Source->Name.ToString()));
					Removed.push_back(*Source);
					#if DURIN_LEVEL_AUTHORING_TEST_FAILURE_INJECTION
					if (ConsumeInjectedFailure(Testing::EStaticMeshLevelMutationFailurePoint::AfterRemove))
						return FailAfterMutation("Injected failure after remove.");
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
						OutError = std::format("Failed to create actor '{}'.", Destination->Name.ToString());
						return FailAfterMutation(OutError);
					}
					Created.push_back(Actor);
					#if DURIN_LEVEL_AUTHORING_TEST_FAILURE_INJECTION
					if (ConsumeInjectedFailure(Testing::EStaticMeshLevelMutationFailurePoint::AfterCreate))
						return FailAfterMutation("Injected failure after create.");
					#endif
				}
				else if (Source->Name != Destination->Name)
				{
					const FName PreviousName = Actor->GetFName();
					if (!Level.RenameActor(Actor, Destination->Name) || Actor->GetFName() != Destination->Name)
						return FailAfterMutation(std::format("Failed to rename actor to '{}'.", Destination->Name.ToString()));
					Renames.push_back({Actor, PreviousName});
					#if DURIN_LEVEL_AUTHORING_TEST_FAILURE_INJECTION
					if (ConsumeInjectedFailure(Testing::EStaticMeshLevelMutationFailurePoint::AfterFinalRename))
						return FailAfterMutation("Injected failure after final rename.");
					#endif
				}
				Updates.push_back({Actor, CaptureState(*Actor)});
				Actor->GetStaticMeshComponent()->SetStaticMesh(Destination->StaticMesh.Get());
				if (!Actor->SetActorTransform(Destination->Transform))
				{
					return FailAfterMutation(std::format("Failed to set the transform for '{}'.", Destination->Name.ToString()));
				}
				Actor->SetHidden(Destination->bHidden);
				#if DURIN_LEVEL_AUTHORING_TEST_FAILURE_INJECTION
				if (ConsumeInjectedFailure(Testing::EStaticMeshLevelMutationFailurePoint::AfterUpdate))
					return FailAfterMutation("Injected failure after update.");
				#endif
			}
			return true;
		}

		class FStaticMeshLevelMutationTransaction final : public ::Durin::Editor::ITransaction
		{
		public:
			FStaticMeshLevelMutationTransaction(const FStaticMeshLevelMutationPlan& Plan)
				: Level(Plan.Level), Description(Plan.Description), Deltas(Plan.Deltas)
			{
				AffectedPackages.front() = Plan.Package.Get();
			}

			auto GetDescription() const -> std::string_view override { return Description; }
			auto GetDetails(::Durin::Editor::ETransactionOperation) const -> std::string override
			{
				return LastError.empty()
					? std::format("Edit {} static mesh actor(s)", Deltas.size())
					: LastError;
			}
			auto GetAffectedPackages() const -> std::span<DPackage* const> override { return AffectedPackages; }
			auto Undo() -> bool override { return Apply(false); }
			auto Redo() -> bool override { return Apply(true); }

		private:
			auto Apply(bool bAfter) -> bool
			{
				LastError.clear();
				return Level && ApplyStates(*Level, Deltas, bAfter, LastError);
			}

			TObjectPtr<DLevel> Level;
			std::string Description;
			std::vector<FStaticMeshActorMutationDelta> Deltas;
			std::array<DPackage*, 1> AffectedPackages{};
			std::string LastError;
		};
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

	auto FStaticMeshLevelMutations::IsSupportedActor(const AStaticMeshActor& Actor,
		std::string* OutReason) -> bool
	{
		if (Actor.GetClass() != AStaticMeshActor::StaticClass()
			|| Actor.GetStaticMeshComponent() == nullptr
			|| Actor.GetRootComponent() != Actor.GetStaticMeshComponent()
			|| Actor.GetComponents().size() != 1
			|| !Actor.GetInstanceComponents().empty()
			|| Actor.GetAttachParentActor() != nullptr
			|| Actor.IsBeginningPlay()
			|| Actor.IsEndingPlay())
		{
			if (OutReason) *OutReason = std::format(
				"Actor '{}' has an unsupported class, component graph, or attachment.", Actor.GetName());
			return false;
		}
		DLevel* Level = Cast<DLevel>(Actor.GetOuter());
		if (Level)
		{
			for (const TObjectPtr<AActor>& Candidate : Level->GetActors())
			{
				if (Candidate && Candidate->GetAttachParentActor() == &Actor)
				{
					if (OutReason) *OutReason = std::format("Actor '{}' has attached children.", Actor.GetName());
					return false;
				}
			}
		}
		return true;
	}

	auto FStaticMeshLevelMutations::Plan(const FStaticMeshLevelMutationRequest& Request)
		-> FStaticMeshLevelMutationPlan
	{
		FStaticMeshLevelMutationPlan Result;
		Result.Diagnostic = {};
		if (GIsGameThreadIdInitialized && !IsInGameThread())
		{
			Result.Diagnostic = MakeDiagnostic(EStaticMeshLevelMutationError::WrongThread,
				"Static mesh level mutation must run on the game thread.");
			return Result;
		}
		if (!Request.Level || Request.Mutations.empty())
		{
			Result.Diagnostic = MakeDiagnostic(EStaticMeshLevelMutationError::InvalidRequest,
				"A target Level and at least one mutation are required.");
			return Result;
		}
		if (Request.bReadOnly)
		{
			Result.Diagnostic = MakeDiagnostic(EStaticMeshLevelMutationError::ReadOnly,
				"The target Level is read-only.");
			return Result;
		}
		DPackage* Package = Request.Level->GetPackage();
		if (!Package || !Package->IsAssetPackage()
			|| Package->GetPackagePath() != Request.ExpectedPackagePath
			|| Package->GetEditRevision() != Request.ExpectedPackageEditRevision)
		{
			Result.Diagnostic = MakeDiagnostic(EStaticMeshLevelMutationError::StaleTarget,
				"The target Level package no longer matches the captured request.");
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
					"Mutation target names cannot be empty.", Index);
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
						std::format("Actor name '{}' is already occupied.", Mutation.TargetName.ToString()), Index);
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
						std::format("Actor '{}' does not exist.", Mutation.TargetName.ToString()), Index);
					return Result;
				}
				std::string Reason;
				if (!StaticMeshActor || !IsSupportedActor(*StaticMeshActor, &Reason))
				{
					Result.Diagnostic = MakeDiagnostic(EStaticMeshLevelMutationError::UnsupportedActor,
						Reason.empty() ? std::format("Actor '{}' is not supported.", Mutation.TargetName.ToString()) : Reason, Index);
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
							"Rename destinations cannot be empty.", Index);
						return Result;
					}
					Delta.After = Delta.Before;
					Delta.After->Name = Mutation.Desired.Name;
				}
			}
			if (Delta.After && !IsFiniteTransform(Delta.After->Transform))
			{
				Result.Diagnostic = MakeDiagnostic(EStaticMeshLevelMutationError::InvalidTransform,
					std::format("Actor '{}' has a non-finite transform.", Delta.After->Name.ToString()), Index);
				return Result;
			}
			if (Delta.After && Delta.After->StaticMesh.Get() && !IsValid(Delta.After->StaticMesh.Get()))
			{
				Result.Diagnostic = MakeDiagnostic(EStaticMeshLevelMutationError::InvalidRequest,
					std::format("StaticMesh for actor '{}' is unavailable.", Delta.After->Name.ToString()), Index);
				return Result;
			}
			if (!ClaimedNames.insert(Mutation.TargetName).second
				|| (Delta.After && Delta.After->Name != Mutation.TargetName
					&& !ClaimedNames.insert(Delta.After->Name).second))
			{
				Result.Diagnostic = MakeDiagnostic(EStaticMeshLevelMutationError::NameConflict,
					"A batch cannot address the same actor name more than once.", Index);
				return Result;
			}
			if (Delta.After && Delta.After->Name != Mutation.TargetName)
			{
				if (AActor* Collision = Request.Level->FindActorByName(Delta.After->Name);
					Collision && Collision != Existing)
				{
					Result.Diagnostic = MakeDiagnostic(EStaticMeshLevelMutationError::NameConflict,
						std::format("Actor name '{}' is already occupied.", Delta.After->Name.ToString()), Index);
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
				"Static mesh level mutation must run on the game thread.");
			return Result;
		}
		DLevel* Level = Plan.Level.Get();
		DPackage* Package = Plan.Package.Get();
		if (Context.bReadOnly)
		{
			Result.Diagnostic = MakeDiagnostic(EStaticMeshLevelMutationError::ReadOnly,
				"The target Level became read-only before execution.");
			return Result;
		}
		if (!Level || Context.OpenLevel != Level || !Package || Level->GetPackage() != Package
			|| Package->GetPackagePath() != Plan.PackagePath
			|| Package->GetEditRevision() != Plan.PackageEditRevision
			|| Level->GetEditorActorHierarchyRevision() != Plan.ActorHierarchyRevision)
		{
			Result.Diagnostic = MakeDiagnostic(EStaticMeshLevelMutationError::StaleTarget,
				"The Level changed after the operation was planned.");
			return Result;
		}
		if (!Plan.bHasChanges)
		{
			Result.Diagnostic = {};
			return Result;
		}
		auto Transaction = std::make_unique<FStaticMeshLevelMutationTransaction>(Plan);
		const bool bSucceeded = Context.Transactions
			? Context.Transactions->Execute(std::move(Transaction))
			: Transaction->Redo();
		if (!bSucceeded)
		{
			Result.Diagnostic = MakeDiagnostic(EStaticMeshLevelMutationError::ExecutionFailed,
				"The static mesh actor batch could not be applied.");
			return Result;
		}
		for (const FStaticMeshActorMutationDelta& Delta : Plan.Deltas)
			if (Delta.After) Result.ResultActorNames.push_back(Delta.After->Name);
		Result.bChanged = true;
		Result.Diagnostic = {};
		return Result;
	}
}
