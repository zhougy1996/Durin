#include "Editor/Transaction.h"

#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"

namespace Durin::Editor
{
	struct FTransactionManager::FTrackedPackageState
	{
		FTrackedPackageState(DPackage& InPackage, FRevisionId InCurrentRevision, FRevisionId InSavedRevision, bool bInCheckpointValid)
			: Package(&InPackage)
			, Root(&InPackage)
			, CurrentRevision(InCurrentRevision)
			, SavedRevision(InSavedRevision)
			, bCheckpointValid(bInCheckpointValid)
		{
		}

		DPackage* Package = nullptr;
		FScopedObjectRoot Root;
		FRevisionId CurrentRevision = 0;
		FRevisionId SavedRevision = 0;
		bool bCheckpointValid = false;
	};

	FTransactionManager::FTransactionManager() = default;

	FTransactionManager::~FTransactionManager()
	{
		Clear();
	}

	auto FTransactionManager::Execute(std::unique_ptr<ITransaction> Transaction) -> bool
	{
		if (!Transaction || PendingTransactionId != 0) return false;
		FEntry Entry = PrepareEntry(std::move(Transaction));
		ITransaction& Operation = *Entry.Transaction;
		const std::string Description(Operation.GetDescription());
		if (!Operation.Redo())
		{
			RecordFailure(ETransactionOperation::Execute, 0, Description, Operation.GetDetails(ETransactionOperation::Execute));
			return false;
		}
		Entry.Id = NextId++;
		const FTransactionId Id = Entry.Id;
		const std::string Details(Operation.GetDetails(ETransactionOperation::Execute));
		RedoStack.clear();
		UndoStack.emplace_back(std::move(Entry));
		ApplyPackageTransitions(UndoStack.back(), true);
		if (UndoStack.back().Transaction->MutatesMountedContent()) NotifyMountedContentMutation();
		if (UndoStack.size() > MaxHistory) UndoStack.erase(UndoStack.begin());
		PendingEvents.push_back({ETransactionEventType::Executed, ETransactionOperation::Execute, Id, Description, Details});
		return true;
	}

	auto FTransactionManager::CommitApplied(std::unique_ptr<ITransaction> Transaction) -> bool
	{
		if (!Transaction || PendingTransactionId != 0) return false;
		FEntry Entry = PrepareEntry(std::move(Transaction));
		Entry.Id = NextId++;
		const FTransactionId Id = Entry.Id;
		const std::string Description(Entry.Transaction->GetDescription());
		const std::string Details(Entry.Transaction->GetDetails(ETransactionOperation::Execute));
		RedoStack.clear();
		UndoStack.emplace_back(std::move(Entry));
		ApplyPackageTransitions(UndoStack.back(), true);
		if (UndoStack.back().Transaction->MutatesMountedContent()) NotifyMountedContentMutation();
		if (UndoStack.size() > MaxHistory) UndoStack.erase(UndoStack.begin());
		PendingEvents.push_back({ETransactionEventType::Executed, ETransactionOperation::Execute, Id, Description, Details});
		return true;
	}

	auto FTransactionManager::Undo() -> bool
	{
		return !UndoStack.empty() && Undo(UndoStack.back().Id);
	}

	auto FTransactionManager::Undo(FTransactionId ExpectedId) -> bool
	{
		if (PendingTransactionId != 0 || !IsUndoHead(ExpectedId)) return false;
		FEntry& Entry = UndoStack.back();
		const std::string Description(Entry.Transaction->GetDescription());
		Entry.Transaction->SetDeferredOperationCompletion(
			[this, ExpectedId](bool bSucceeded) {
				CompleteDeferredOperation(ETransactionOperation::Undo, ExpectedId, bSucceeded);
			});
		if (!Entry.Transaction->Undo())
		{
			Entry.Transaction->SetDeferredOperationCompletion({});
			RecordFailure(ETransactionOperation::Undo, Entry.Id, Description, Entry.Transaction->GetDetails(ETransactionOperation::Undo));
			return false;
		}
		if (Entry.Transaction->IsDeferredOperationPending())
		{
			PendingOperation = ETransactionOperation::Undo;
			PendingTransactionId = ExpectedId;
			return true;
		}
		Entry.Transaction->SetDeferredOperationCompletion({});
		FinalizeUndo(ExpectedId);
		return true;
	}

	auto FTransactionManager::Redo() -> bool
	{
		return !RedoStack.empty() && Redo(RedoStack.back().Id);
	}

	auto FTransactionManager::Redo(FTransactionId ExpectedId) -> bool
	{
		if (PendingTransactionId != 0 || !IsRedoHead(ExpectedId)) return false;
		FEntry& Entry = RedoStack.back();
		const std::string Description(Entry.Transaction->GetDescription());
		Entry.Transaction->SetDeferredOperationCompletion(
			[this, ExpectedId](bool bSucceeded) {
				CompleteDeferredOperation(ETransactionOperation::Redo, ExpectedId, bSucceeded);
			});
		if (!Entry.Transaction->Redo())
		{
			Entry.Transaction->SetDeferredOperationCompletion({});
			RecordFailure(ETransactionOperation::Redo, Entry.Id, Description, Entry.Transaction->GetDetails(ETransactionOperation::Redo));
			return false;
		}
		if (Entry.Transaction->IsDeferredOperationPending())
		{
			PendingOperation = ETransactionOperation::Redo;
			PendingTransactionId = ExpectedId;
			return true;
		}
		Entry.Transaction->SetDeferredOperationCompletion({});
		FinalizeRedo(ExpectedId);
		return true;
	}

	auto FTransactionManager::FinalizeUndo(FTransactionId Id) -> void
	{
		check(IsUndoHead(Id));
		FEntry& Entry = UndoStack.back();
		const std::string Description(Entry.Transaction->GetDescription());
		const std::string Details(Entry.Transaction->GetDetails(ETransactionOperation::Undo));
		ApplyPackageTransitions(Entry, false);
		if (Entry.Transaction->MutatesMountedContent()) NotifyMountedContentMutation();
		FEntry Applied = std::move(Entry);
		UndoStack.pop_back();
		RedoStack.emplace_back(std::move(Applied));
		PendingEvents.push_back({ETransactionEventType::Undone, ETransactionOperation::Undo, Id, Description, Details});
	}

	auto FTransactionManager::FinalizeRedo(FTransactionId Id) -> void
	{
		check(IsRedoHead(Id));
		FEntry& Entry = RedoStack.back();
		const std::string Description(Entry.Transaction->GetDescription());
		const std::string Details(Entry.Transaction->GetDetails(ETransactionOperation::Redo));
		ApplyPackageTransitions(Entry, true);
		if (Entry.Transaction->MutatesMountedContent()) NotifyMountedContentMutation();
		FEntry Applied = std::move(Entry);
		RedoStack.pop_back();
		UndoStack.emplace_back(std::move(Applied));
		PendingEvents.push_back({ETransactionEventType::Redone, ETransactionOperation::Redo, Id, Description, Details});
	}

	auto FTransactionManager::CompleteDeferredOperation(
		ETransactionOperation Operation,
		FTransactionId Id,
		bool bSucceeded) -> void
	{
		if (PendingTransactionId != Id || PendingOperation != Operation) return;
		FEntry& Entry = Operation == ETransactionOperation::Undo
			? UndoStack.back() : RedoStack.back();
		Entry.Transaction->SetDeferredOperationCompletion({});
		PendingTransactionId = 0;
		PendingOperation = ETransactionOperation::Execute;
		if (!bSucceeded)
		{
			RecordFailure(Operation, Id, Entry.Transaction->GetDescription(), Entry.Transaction->GetDetails(Operation));
			return;
		}
		if (Operation == ETransactionOperation::Undo) FinalizeUndo(Id);
		else FinalizeRedo(Id);
	}

	auto FTransactionManager::IsUndoHead(FTransactionId Id) const -> bool
	{
		return Id != 0 && !UndoStack.empty() && UndoStack.back().Id == Id;
	}

	auto FTransactionManager::IsRedoHead(FTransactionId Id) const -> bool
	{
		return Id != 0 && !RedoStack.empty() && RedoStack.back().Id == Id;
	}

	auto FTransactionManager::GetUndoDescription() const -> std::string_view
	{
		return UndoStack.empty() ? std::string_view{} : UndoStack.back().Transaction->GetDescription();
	}

	auto FTransactionManager::GetRedoDescription() const -> std::string_view
	{
		return RedoStack.empty() ? std::string_view{} : RedoStack.back().Transaction->GetDescription();
	}

	auto FTransactionManager::GetUndoId() const -> FTransactionId
	{
		return UndoStack.empty() ? 0 : UndoStack.back().Id;
	}

	auto FTransactionManager::GetRedoId() const -> FTransactionId
	{
		return RedoStack.empty() ? 0 : RedoStack.back().Id;
	}

	auto FTransactionManager::ConsumeEvents() -> std::vector<FTransactionEvent>
	{
		std::vector<FTransactionEvent> Events;
		Events.swap(PendingEvents);
		return Events;
	}

	auto FTransactionManager::NotifyMountedContentMutation() -> void
	{
		check(MountedContentMutationRevision != std::numeric_limits<uint64>::max());
		++MountedContentMutationRevision;
	}

	auto FTransactionManager::EstablishSavedState(DPackage& Package) -> void
	{
		ForgetPackage(Package);
		const FRevisionId Revision = AllocateRevision();
		auto State = std::make_unique<FTrackedPackageState>(Package, Revision, Revision, true);
		PackageStates.emplace(&Package, std::move(State));
		Package.ClearDirty();
	}

	auto FTransactionManager::MarkSaved(DPackage& Package) -> void
	{
		FTrackedPackageState& State = EnsurePackageState(Package);
		State.SavedRevision = State.CurrentRevision;
		State.bCheckpointValid = true;
		SynchronizeDirtyState(State);
	}

	auto FTransactionManager::InvalidateSavedState(DPackage& Package) -> void
	{
		FTrackedPackageState& State = EnsurePackageState(Package);
		State.SavedRevision = 0;
		State.bCheckpointValid = false;
		SynchronizeDirtyState(State);
	}

	auto FTransactionManager::GetPackageRevisionState(const DPackage& Package) const -> std::optional<FPackageRevisionState>
	{
		const FTrackedPackageState* State = FindPackageState(Package);
		if (!State) return std::nullopt;
		return FPackageRevisionState{
			.CurrentRevision = State->CurrentRevision,
			.SavedRevision = State->SavedRevision,
			.bCheckpointValid = State->bCheckpointValid,
		};
	}

	auto FTransactionManager::ForgetPackage(DPackage& Package) -> void
	{
		RemovePackageHistory(Package);
		PackageStates.erase(&Package);
	}

	auto FTransactionManager::Clear() -> void
	{
		if (PendingTransactionId != 0)
		{
			FEntry& Entry = PendingOperation == ETransactionOperation::Undo
				? UndoStack.back() : RedoStack.back();
			Entry.Transaction->SetDeferredOperationCompletion({});
		}
		PendingTransactionId = 0;
		PendingOperation = ETransactionOperation::Execute;
		UndoStack.clear();
		RedoStack.clear();
		PendingEvents.clear();
		PackageStates.clear();
	}

	auto FTransactionManager::PrepareEntry(std::unique_ptr<ITransaction> Transaction) -> FEntry
	{
		FEntry Entry{
			.Transaction = std::move(Transaction),
		};
		std::unordered_set<DPackage*> AddedPackages;
		for (DPackage* Package : Entry.Transaction->GetAffectedPackages())
		{
			if (!IsValid(Package) || !Package->IsAssetPackage() || !AddedPackages.insert(Package).second) continue;

			const FTrackedPackageState* State = FindPackageState(*Package);
			const FRevisionId BeforeRevision = State ? State->CurrentRevision : AllocateRevision();
			Entry.PackageTransitions.push_back({
				.Package = Package,
				.BeforeRevision = BeforeRevision,
				.AfterRevision = AllocateRevision(),
				.InitialSavedRevision = State ? State->SavedRevision : (Package->IsDirty() ? 0 : BeforeRevision),
				.bInitialCheckpointValid = State ? State->bCheckpointValid : !Package->IsDirty(),
			});
		}
		return Entry;
	}

	auto FTransactionManager::AllocateRevision() -> FRevisionId
	{
		check(NextRevision != 0);
		return NextRevision++;
	}

	auto FTransactionManager::FindPackageState(const DPackage& Package) -> FTrackedPackageState*
	{
		const auto It = PackageStates.find(const_cast<DPackage*>(&Package));
		return It == PackageStates.end() ? nullptr : It->second.get();
	}

	auto FTransactionManager::FindPackageState(const DPackage& Package) const -> const FTrackedPackageState*
	{
		const auto It = PackageStates.find(const_cast<DPackage*>(&Package));
		return It == PackageStates.end() ? nullptr : It->second.get();
	}

	auto FTransactionManager::EnsurePackageState(DPackage& Package) -> FTrackedPackageState&
	{
		if (FTrackedPackageState* State = FindPackageState(Package)) return *State;
		const FRevisionId Revision = AllocateRevision();
		const bool bCheckpointValid = !Package.IsDirty();
		auto State = std::make_unique<FTrackedPackageState>(
			Package, Revision, bCheckpointValid ? Revision : 0, bCheckpointValid
		);
		FTrackedPackageState* Result = State.get();
		PackageStates.emplace(&Package, std::move(State));
		return *Result;
	}

	auto FTransactionManager::ApplyPackageTransitions(const FEntry& Entry, bool bForward) -> void
	{
		for (const FPackageRevisionTransition& Transition : Entry.PackageTransitions)
		{
			FTrackedPackageState* State = FindPackageState(*Transition.Package);
			if (!State)
			{
				auto NewState = std::make_unique<FTrackedPackageState>(
					*Transition.Package,
					Transition.BeforeRevision,
					Transition.InitialSavedRevision,
					Transition.bInitialCheckpointValid
				);
				State = NewState.get();
				PackageStates.emplace(Transition.Package, std::move(NewState));
			}
			State->CurrentRevision = bForward ? Transition.AfterRevision : Transition.BeforeRevision;
			SynchronizeDirtyState(*State);
		}
	}

	auto FTransactionManager::SynchronizeDirtyState(FTrackedPackageState& State) -> void
	{
		if (!State.bCheckpointValid || State.CurrentRevision != State.SavedRevision)
		{
			State.Package->MarkDirty();
		}
		else
		{
			State.Package->ClearDirty();
		}
	}

	auto FTransactionManager::RemovePackageHistory(const DPackage& Package) -> void
	{
		const auto ReferencesPackage = [&Package](const FEntry& Entry) {
			return std::ranges::any_of(Entry.PackageTransitions, [&Package](const FPackageRevisionTransition& Transition) {
				return Transition.Package == &Package;
			});
		};
		std::erase_if(UndoStack, ReferencesPackage);
		std::erase_if(RedoStack, ReferencesPackage);
	}

	auto FTransactionManager::RecordFailure(ETransactionOperation Operation, FTransactionId Id, std::string_view Description, std::string_view Details) -> void
	{
		PendingEvents.push_back({ETransactionEventType::Failed, Operation, Id, std::string(Description), std::string(Details)});
	}
}
