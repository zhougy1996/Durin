#include "Editor/EditorTransaction.h"

#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"

namespace Durin
{
	struct FEditorTransactionManager::FTrackedPackageState
	{
		FTrackedPackageState(DPackage& InPackage, FEditorRevisionId InCurrentRevision, FEditorRevisionId InSavedRevision, bool bInCheckpointValid)
			: Package(&InPackage)
			, Root(&InPackage)
			, CurrentRevision(InCurrentRevision)
			, SavedRevision(InSavedRevision)
			, bCheckpointValid(bInCheckpointValid)
		{
		}

		DPackage* Package = nullptr;
		FScopedObjectRoot Root;
		FEditorRevisionId CurrentRevision = 0;
		FEditorRevisionId SavedRevision = 0;
		bool bCheckpointValid = false;
	};

	FEditorTransactionManager::FEditorTransactionManager() = default;

	FEditorTransactionManager::~FEditorTransactionManager()
	{
		Clear();
	}

	auto FEditorTransactionManager::Execute(std::unique_ptr<IEditorTransaction> Transaction) -> bool
	{
		if (!Transaction) return false;
		FEntry Entry = PrepareEntry(std::move(Transaction));
		IEditorTransaction& Operation = *Entry.Transaction;
		const std::string Description(Operation.GetDescription());
		if (!Operation.Redo())
		{
			RecordFailure(EEditorTransactionOperation::Execute, 0, Description, Operation.GetDetails(EEditorTransactionOperation::Execute));
			return false;
		}
		Entry.Id = NextId++;
		const FEditorTransactionId Id = Entry.Id;
		const std::string Details(Operation.GetDetails(EEditorTransactionOperation::Execute));
		RedoStack.clear();
		UndoStack.emplace_back(std::move(Entry));
		ApplyPackageTransitions(UndoStack.back(), true);
		if (UndoStack.back().Transaction->MutatesMountedContent()) NotifyMountedContentMutation();
		if (UndoStack.size() > MaxHistory) UndoStack.erase(UndoStack.begin());
		PendingEvents.push_back({EEditorTransactionEventType::Executed, EEditorTransactionOperation::Execute, Id, Description, Details});
		return true;
	}

	auto FEditorTransactionManager::CommitApplied(std::unique_ptr<IEditorTransaction> Transaction) -> bool
	{
		if (!Transaction) return false;
		FEntry Entry = PrepareEntry(std::move(Transaction));
		Entry.Id = NextId++;
		const FEditorTransactionId Id = Entry.Id;
		const std::string Description(Entry.Transaction->GetDescription());
		const std::string Details(Entry.Transaction->GetDetails(EEditorTransactionOperation::Execute));
		RedoStack.clear();
		UndoStack.emplace_back(std::move(Entry));
		ApplyPackageTransitions(UndoStack.back(), true);
		if (UndoStack.back().Transaction->MutatesMountedContent()) NotifyMountedContentMutation();
		if (UndoStack.size() > MaxHistory) UndoStack.erase(UndoStack.begin());
		PendingEvents.push_back({EEditorTransactionEventType::Executed, EEditorTransactionOperation::Execute, Id, Description, Details});
		return true;
	}

	auto FEditorTransactionManager::Undo() -> bool
	{
		return !UndoStack.empty() && Undo(UndoStack.back().Id);
	}

	auto FEditorTransactionManager::Undo(FEditorTransactionId ExpectedId) -> bool
	{
		if (!IsUndoHead(ExpectedId)) return false;
		FEntry& Entry = UndoStack.back();
		const std::string Description(Entry.Transaction->GetDescription());
		if (!Entry.Transaction->Undo())
		{
			RecordFailure(EEditorTransactionOperation::Undo, Entry.Id, Description, Entry.Transaction->GetDetails(EEditorTransactionOperation::Undo));
			return false;
		}
		const std::string Details(Entry.Transaction->GetDetails(EEditorTransactionOperation::Undo));
		ApplyPackageTransitions(Entry, false);
		if (Entry.Transaction->MutatesMountedContent()) NotifyMountedContentMutation();
		FEntry Applied = std::move(Entry);
		UndoStack.pop_back();
		RedoStack.emplace_back(std::move(Applied));
		PendingEvents.push_back({EEditorTransactionEventType::Undone, EEditorTransactionOperation::Undo, ExpectedId, Description, Details});
		return true;
	}

	auto FEditorTransactionManager::Redo() -> bool
	{
		return !RedoStack.empty() && Redo(RedoStack.back().Id);
	}

	auto FEditorTransactionManager::Redo(FEditorTransactionId ExpectedId) -> bool
	{
		if (!IsRedoHead(ExpectedId)) return false;
		FEntry& Entry = RedoStack.back();
		const std::string Description(Entry.Transaction->GetDescription());
		if (!Entry.Transaction->Redo())
		{
			RecordFailure(EEditorTransactionOperation::Redo, Entry.Id, Description, Entry.Transaction->GetDetails(EEditorTransactionOperation::Redo));
			return false;
		}
		const std::string Details(Entry.Transaction->GetDetails(EEditorTransactionOperation::Redo));
		ApplyPackageTransitions(Entry, true);
		if (Entry.Transaction->MutatesMountedContent()) NotifyMountedContentMutation();
		FEntry Applied = std::move(Entry);
		RedoStack.pop_back();
		UndoStack.emplace_back(std::move(Applied));
		PendingEvents.push_back({EEditorTransactionEventType::Redone, EEditorTransactionOperation::Redo, ExpectedId, Description, Details});
		return true;
	}

	auto FEditorTransactionManager::IsUndoHead(FEditorTransactionId Id) const -> bool
	{
		return Id != 0 && !UndoStack.empty() && UndoStack.back().Id == Id;
	}

	auto FEditorTransactionManager::IsRedoHead(FEditorTransactionId Id) const -> bool
	{
		return Id != 0 && !RedoStack.empty() && RedoStack.back().Id == Id;
	}

	auto FEditorTransactionManager::GetUndoDescription() const -> std::string_view
	{
		return UndoStack.empty() ? std::string_view{} : UndoStack.back().Transaction->GetDescription();
	}

	auto FEditorTransactionManager::GetRedoDescription() const -> std::string_view
	{
		return RedoStack.empty() ? std::string_view{} : RedoStack.back().Transaction->GetDescription();
	}

	auto FEditorTransactionManager::GetUndoId() const -> FEditorTransactionId
	{
		return UndoStack.empty() ? 0 : UndoStack.back().Id;
	}

	auto FEditorTransactionManager::GetRedoId() const -> FEditorTransactionId
	{
		return RedoStack.empty() ? 0 : RedoStack.back().Id;
	}

	auto FEditorTransactionManager::ConsumeEvents() -> std::vector<FEditorTransactionEvent>
	{
		std::vector<FEditorTransactionEvent> Events;
		Events.swap(PendingEvents);
		return Events;
	}

	auto FEditorTransactionManager::NotifyMountedContentMutation() -> void
	{
		check(MountedContentMutationRevision != std::numeric_limits<uint64>::max());
		++MountedContentMutationRevision;
	}

	auto FEditorTransactionManager::EstablishSavedState(DPackage& Package) -> void
	{
		ForgetPackage(Package);
		const FEditorRevisionId Revision = AllocateRevision();
		auto State = std::make_unique<FTrackedPackageState>(Package, Revision, Revision, true);
		PackageStates.emplace(&Package, std::move(State));
		Package.ClearDirty();
	}

	auto FEditorTransactionManager::MarkSaved(DPackage& Package) -> void
	{
		FTrackedPackageState& State = EnsurePackageState(Package);
		State.SavedRevision = State.CurrentRevision;
		State.bCheckpointValid = true;
		SynchronizeDirtyState(State);
	}

	auto FEditorTransactionManager::InvalidateSavedState(DPackage& Package) -> void
	{
		FTrackedPackageState& State = EnsurePackageState(Package);
		State.SavedRevision = 0;
		State.bCheckpointValid = false;
		SynchronizeDirtyState(State);
	}

	auto FEditorTransactionManager::GetPackageRevisionState(const DPackage& Package) const -> std::optional<FEditorPackageRevisionState>
	{
		const FTrackedPackageState* State = FindPackageState(Package);
		if (!State) return std::nullopt;
		return FEditorPackageRevisionState{
			.CurrentRevision = State->CurrentRevision,
			.SavedRevision = State->SavedRevision,
			.bCheckpointValid = State->bCheckpointValid,
		};
	}

	auto FEditorTransactionManager::ForgetPackage(DPackage& Package) -> void
	{
		RemovePackageHistory(Package);
		PackageStates.erase(&Package);
	}

	auto FEditorTransactionManager::Clear() -> void
	{
		UndoStack.clear();
		RedoStack.clear();
		PendingEvents.clear();
		PackageStates.clear();
	}

	auto FEditorTransactionManager::PrepareEntry(std::unique_ptr<IEditorTransaction> Transaction) -> FEntry
	{
		FEntry Entry{
			.Transaction = std::move(Transaction),
		};
		std::unordered_set<DPackage*> AddedPackages;
		for (DPackage* Package : Entry.Transaction->GetAffectedPackages())
		{
			if (!IsValid(Package) || !Package->IsAssetPackage() || !AddedPackages.insert(Package).second) continue;

			const FTrackedPackageState* State = FindPackageState(*Package);
			const FEditorRevisionId BeforeRevision = State ? State->CurrentRevision : AllocateRevision();
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

	auto FEditorTransactionManager::AllocateRevision() -> FEditorRevisionId
	{
		check(NextRevision != 0);
		return NextRevision++;
	}

	auto FEditorTransactionManager::FindPackageState(const DPackage& Package) -> FTrackedPackageState*
	{
		const auto It = PackageStates.find(const_cast<DPackage*>(&Package));
		return It == PackageStates.end() ? nullptr : It->second.get();
	}

	auto FEditorTransactionManager::FindPackageState(const DPackage& Package) const -> const FTrackedPackageState*
	{
		const auto It = PackageStates.find(const_cast<DPackage*>(&Package));
		return It == PackageStates.end() ? nullptr : It->second.get();
	}

	auto FEditorTransactionManager::EnsurePackageState(DPackage& Package) -> FTrackedPackageState&
	{
		if (FTrackedPackageState* State = FindPackageState(Package)) return *State;
		const FEditorRevisionId Revision = AllocateRevision();
		const bool bCheckpointValid = !Package.IsDirty();
		auto State = std::make_unique<FTrackedPackageState>(
			Package, Revision, bCheckpointValid ? Revision : 0, bCheckpointValid
		);
		FTrackedPackageState* Result = State.get();
		PackageStates.emplace(&Package, std::move(State));
		return *Result;
	}

	auto FEditorTransactionManager::ApplyPackageTransitions(const FEntry& Entry, bool bForward) -> void
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

	auto FEditorTransactionManager::SynchronizeDirtyState(FTrackedPackageState& State) -> void
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

	auto FEditorTransactionManager::RemovePackageHistory(const DPackage& Package) -> void
	{
		const auto ReferencesPackage = [&Package](const FEntry& Entry) {
			return std::ranges::any_of(Entry.PackageTransitions, [&Package](const FPackageRevisionTransition& Transition) {
				return Transition.Package == &Package;
			});
		};
		std::erase_if(UndoStack, ReferencesPackage);
		std::erase_if(RedoStack, ReferencesPackage);
	}

	auto FEditorTransactionManager::RecordFailure(EEditorTransactionOperation Operation, FEditorTransactionId Id, std::string_view Description, std::string_view Details) -> void
	{
		PendingEvents.push_back({EEditorTransactionEventType::Failed, Operation, Id, std::string(Description), std::string(Details)});
	}
}
