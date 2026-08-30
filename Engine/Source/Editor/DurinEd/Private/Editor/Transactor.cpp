#include "Editor/Transactor.h"

#include "DObject/Archive.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "DObject/Property.h"
#include "CoreGlobals.h"
#include "Threading/RunnableThread.h"

namespace Durin::Editor
{
	namespace
	{
		auto AddSize(size_t& Total, size_t Value) -> bool
		{
			if (Total > std::numeric_limits<size_t>::max() - Value) return false;
			Total += Value;
			return true;
		}
	}

	auto FTransactionRecord::Validate(std::string* OutError) const -> bool
	{
		return std::visit([&](const auto& Record) {
			using T = std::decay_t<decltype(Record)>;
			if constexpr (std::is_same_v<T, FFocusedTransactionObjectRecord>)
			{
				FReflectedValueStorage Storage;
				return Record.RestoreDetached(Storage, OutError);
			}
			else if constexpr (std::is_same_v<T, FTransactionObjectRecord>)
				return Record.Validate(OutError);
			else
			{
				if (Record) return true;
				if (OutError) *OutError = "The custom transaction change is unavailable.";
				return false;
			}
		}, Data);
	}

	auto FTransactionRecord::Apply(
		bool bBefore,
		EPropertyChangeOrigin Origin,
		std::string* OutError) -> bool
	{
		return std::visit([&](auto& Record) {
			using T = std::decay_t<decltype(Record)>;
			if constexpr (std::is_same_v<T, FFocusedTransactionObjectRecord>)
				return Validate(OutError);
			else if constexpr (std::is_same_v<T, FTransactionObjectRecord>)
				return Record.Apply(bBefore, Origin, OutError);
			else
			{
				if (!Record)
				{
					if (OutError) *OutError = "The custom transaction change is unavailable.";
					return false;
				}
				const bool bSucceeded = bBefore ? Record->Undo() : Record->Redo();
				if (!bSucceeded && OutError)
					*OutError = Record->GetDetails(
						bBefore ? ETransactionOperation::Undo : ETransactionOperation::Redo);
				return bSucceeded;
			}
		}, Data);
	}

	auto FTransactionRecord::AddReferencedObjects(FReferenceCollector& Collector) const -> void
	{
		std::visit([&](const auto& Record) {
			using T = std::decay_t<decltype(Record)>;
			if constexpr (std::is_same_v<T, std::unique_ptr<ITransactionCustomChange>>)
			{
				if (Record)
				{
					Record->AddReferencedObjects(Collector);
					for (DPackage* Package : Record->GetAffectedPackages())
					{
						DObject* Object = Package;
						if (Object) Collector.AddReferencedObject(Object);
					}
				}
			}
			else Record.AddReferencedObjects(Collector);
		}, Data);
	}

	auto FTransactionRecord::TryGetAllocatedSize(size_t& OutBytes) const -> bool
	{
		return std::visit([&](const auto& Record) {
			using T = std::decay_t<decltype(Record)>;
			if constexpr (std::is_same_v<T, std::unique_ptr<ITransactionCustomChange>>)
			{
				if (!Record) return false;
				const size_t Allocated = Record->GetAllocatedSize();
				if (Allocated > std::numeric_limits<size_t>::max()
					- sizeof(ITransactionCustomChange)) return false;
				OutBytes = sizeof(ITransactionCustomChange) + Allocated;
				return true;
			}
			else return Record.TryGetAllocatedSize(OutBytes);
		}, Data);
	}

	auto FTransactionRecord::IsDeferredOperationPending() const -> bool
	{
		if (const auto* Change = std::get_if<std::unique_ptr<ITransactionCustomChange>>(&Data))
			return *Change && (*Change)->IsDeferredOperationPending();
		return false;
	}

	auto FTransactionRecord::SetDeferredOperationCompletion(
		FTransactionDeferredCompletion Completion) -> void
	{
		if (auto* Change = std::get_if<std::unique_ptr<ITransactionCustomChange>>(&Data);
			Change && *Change)
			(*Change)->SetDeferredOperationCompletion(std::move(Completion));
	}

	auto FTransactionRecord::GetDetails(ETransactionOperation Operation) const -> std::string
	{
		if (const auto* Change = std::get_if<std::unique_ptr<ITransactionCustomChange>>(&Data);
			Change && *Change)
			return (*Change)->GetDetails(Operation);
		return {};
	}

	auto FTransactionRecord::GetAffectedPackages() const -> std::span<DPackage* const>
	{
		if (const auto* Change = std::get_if<std::unique_ptr<ITransactionCustomChange>>(&Data);
			Change && *Change)
			return (*Change)->GetAffectedPackages();
		return {};
	}

	auto FTransactionRecord::MutatesMountedContent() const -> bool
	{
		if (const auto* Change = std::get_if<std::unique_ptr<ITransactionCustomChange>>(&Data);
			Change && *Change)
			return (*Change)->MutatesMountedContent();
		return false;
	}

	auto FTransactionRecord::IsOwnedByModule(std::string_view ModuleName) const -> bool
	{
		if (const auto* Change = std::get_if<std::unique_ptr<ITransactionCustomChange>>(&Data);
			Change && *Change)
			return (*Change)->GetOwningModule() == ModuleName;
		return false;
	}

	FTransaction::FTransaction(FTransactionId InId, FTransactionContext InContext)
		: Id(InId)
		, Context(std::move(InContext))
	{
	}

	auto FTransaction::AddRecord(FFocusedTransactionObjectRecord Record) -> uint64
	{
		Records.emplace_back(std::move(Record));
		return Records.size();
	}

	auto FTransaction::AddRecord(FTransactionObjectRecord Record) -> uint64
	{
		Records.emplace_back(std::move(Record));
		return Records.size();
	}

	auto FTransaction::AddRecord(std::unique_ptr<ITransactionCustomChange> Change) -> uint64
	{
		Records.emplace_back(std::move(Change));
		return Records.size();
	}

	auto FTransaction::UpdateRecord(
		uint64 RecordId,
		FTransactionObjectRecord Record) -> bool
	{
		if (RecordId == 0 || RecordId > Records.size()) return false;
		Records[RecordId - 1] = FTransactionRecord(std::move(Record));
		return true;
	}

	auto FTransaction::TruncateRecords(size_t Count) -> void
	{
		if (Count < Records.size()) Records.erase(Records.begin() + Count, Records.end());
	}

	auto FTransaction::Validate(std::string* OutError) const -> bool
	{
		if (OutError) OutError->clear();
		for (const FTransactionRecord& Record : Records)
		{
			if (!Record.Validate(OutError)) return false;
		}
		return true;
	}

	auto FTransaction::Apply(
		bool bUndo,
		EPropertyChangeOrigin Origin,
		std::string* OutError) -> bool
	{
		if (!Validate(OutError)) return false;
		std::vector<size_t> Applied;
		Applied.reserve(Records.size());
		bool bSucceeded = true;
		if (bUndo)
		{
			for (size_t Index = Records.size(); Index-- > 0;)
			{
				if (!Records[Index].Apply(true, Origin, OutError))
				{
					bSucceeded = false;
					break;
				}
				Applied.push_back(Index);
			}
		}
		else
		{
			for (size_t Index = 0; Index < Records.size(); ++Index)
			{
				if (!Records[Index].Apply(false, Origin, OutError))
				{
					bSucceeded = false;
					break;
				}
				Applied.push_back(Index);
			}
		}
		if (bSucceeded) return true;

		const std::string ApplyError = OutError ? *OutError : std::string{};
		std::string RollbackError;
		bool bRollbackSucceeded = true;
		for (auto It = Applied.rbegin(); It != Applied.rend(); ++It)
		{
			if (!Records[*It].Apply(!bUndo, Origin, &RollbackError))
				bRollbackSucceeded = false;
		}
		if (OutError)
		{
			*OutError = ApplyError;
			if (!bRollbackSucceeded)
				*OutError += std::format(" Rollback also failed: {}", RollbackError);
		}
		return false;
	}

	auto FTransaction::IsDeferredOperationPending() const -> bool
	{
		return std::ranges::any_of(Records,
			&FTransactionRecord::IsDeferredOperationPending);
	}

	auto FTransaction::SetDeferredOperationCompletion(
		FTransactionDeferredCompletion Completion) -> void
	{
		for (FTransactionRecord& Record : Records)
			Record.SetDeferredOperationCompletion(Completion);
	}

	auto FTransaction::GetDetails(ETransactionOperation Operation) const -> std::string
	{
		std::string Details;
		for (const FTransactionRecord& Record : Records)
		{
			std::string RecordDetails = Record.GetDetails(Operation);
			if (RecordDetails.empty()) continue;
			if (!Details.empty()) Details += " ";
			Details += std::move(RecordDetails);
		}
		return Details;
	}

	auto FTransaction::GetAffectedPackages() const -> std::vector<DPackage*>
	{
		std::vector<DPackage*> Packages;
		if (DObject* Primary = Context.PrimaryObject.Resolve())
			if (DPackage* Package = Primary->GetPackage()) Packages.push_back(Package);
		for (const FTransactionRecord& Record : Records)
			for (DPackage* Package : Record.GetAffectedPackages())
				if (Package && std::ranges::find(Packages, Package) == Packages.end())
					Packages.push_back(Package);
		return Packages;
	}

	auto FTransaction::MutatesMountedContent() const -> bool
	{
		return std::ranges::any_of(Records, &FTransactionRecord::MutatesMountedContent);
	}

	auto FTransaction::IsOwnedByModule(std::string_view ModuleName) const -> bool
	{
		return std::ranges::any_of(Records, [&](const FTransactionRecord& Record) {
			return Record.IsOwnedByModule(ModuleName);
		});
	}

	auto FTransaction::SetPackageTransitions(
		std::vector<FTransactionPackageRevisionTransition> Transitions) -> void
	{
		PackageTransitions = std::move(Transitions);
	}

	auto FTransaction::ReferencesPackage(const DPackage& Package) const -> bool
	{
		return std::ranges::any_of(PackageTransitions,
			[&](const FTransactionPackageRevisionTransition& Transition) {
				return Transition.Package.Resolve() == &Package;
			});
	}

	auto FTransaction::AddReferencedObjects(FReferenceCollector& Collector) const -> void
	{
		Context.PrimaryObject.AddReferencedObjects(Collector);
		for (const FTransactionRecord& Record : Records)
			Record.AddReferencedObjects(Collector);
		for (const FTransactionPackageRevisionTransition& Transition : PackageTransitions)
			Transition.Package.AddReferencedObjects(Collector);
	}

	auto FTransaction::TryGetOwnedSize(size_t& OutBytes) const -> bool
	{
		size_t Total = sizeof(FTransaction);
		if (!AddSize(Total, Context.Name.capacity())
			|| !AddSize(Total, Context.Description.capacity())
			|| Records.capacity() > std::numeric_limits<size_t>::max()
				/ sizeof(FTransactionRecord)
			|| !AddSize(Total, Records.capacity() * sizeof(FTransactionRecord))
			|| PackageTransitions.capacity() > std::numeric_limits<size_t>::max()
				/ sizeof(FTransactionPackageRevisionTransition)
			|| !AddSize(Total, PackageTransitions.capacity()
				* sizeof(FTransactionPackageRevisionTransition)))
		{
			return false;
		}
		for (const FTransactionRecord& Record : Records)
		{
			size_t RecordBytes = 0;
			if (!Record.TryGetAllocatedSize(RecordBytes) || !AddSize(Total, RecordBytes))
				return false;
		}
		OutBytes = Total;
		return true;
	}

	FScopedTransaction::FScopedTransaction(
		DTransactor* InTransactor,
		FTransactionContext Context)
	{
		if (!InTransactor) return;
		const FTransactorResult Result = InTransactor->Begin(Context);
		if (!Result) return;
		Transactor = InTransactor;
		ScopeId = Result.ScopeId;
	}

	FScopedTransaction::~FScopedTransaction()
	{
		if (IsActive()) (void)End();
	}

	FScopedTransaction::FScopedTransaction(FScopedTransaction&& Other) noexcept
		: Transactor(std::exchange(Other.Transactor, nullptr))
		, ScopeId(std::exchange(Other.ScopeId, 0))
	{
	}

	auto FScopedTransaction::operator=(FScopedTransaction&& Other) noexcept
		-> FScopedTransaction&
	{
		if (this == &Other) return *this;
		if (IsActive()) (void)End();
		Transactor = std::exchange(Other.Transactor, nullptr);
		ScopeId = std::exchange(Other.ScopeId, 0);
		return *this;
	}

	auto FScopedTransaction::End() -> FTransactorResult
	{
		if (!IsActive())
			return {.Code = ETransactorResultCode::NoOp, .Message = "Transaction scope is inactive."};
		const FTransactorResult Result = Transactor->End(ScopeId);
		if (Result.Code != ETransactorResultCode::Rejected)
		{
			Transactor = nullptr;
			ScopeId = 0;
		}
		return Result;
	}

	auto FScopedTransaction::Cancel() -> FTransactorResult
	{
		if (!IsActive())
			return {.Code = ETransactorResultCode::NoOp, .Message = "Transaction scope is inactive."};
		const FTransactorResult Result = Transactor->Cancel(ScopeId);
		if (Result.Code != ETransactorResultCode::Rejected)
		{
			Transactor = nullptr;
			ScopeId = 0;
		}
		return Result;
	}
}

namespace Durin
{
	using namespace Editor;

	DTransactor::DTransactor(const FObjectInitializer& ObjectInitializer)
		: DObject(ObjectInitializer)
	{
	}

	namespace
	{
		auto Unsupported() -> FTransactorResult
		{
			return {.Code = ETransactorResultCode::Rejected,
				.Message = "The abstract transactor does not implement this operation."};
		}
	}

	auto DTransactor::Begin(const FTransactionContext&) -> FTransactorResult { return Unsupported(); }
	auto DTransactor::Record(FFocusedTransactionObjectRecord) -> FTransactorResult { return Unsupported(); }
	auto DTransactor::Record(FTransactionObjectRecord) -> FTransactorResult { return Unsupported(); }
	auto DTransactor::Execute(std::unique_ptr<ITransactionCustomChange>, bool) -> FTransactorResult { return Unsupported(); }
	auto DTransactor::CommitApplied(std::unique_ptr<ITransactionCustomChange> Change)
		-> FTransactorResult
	{
		return Execute(std::move(Change), true);
	}
	auto DTransactor::UpdateRecord(uint64, FTransactionObjectRecord) -> FTransactorResult { return Unsupported(); }
	auto DTransactor::End(FTransactionScopeId) -> FTransactorResult { return Unsupported(); }
	auto DTransactor::Cancel(FTransactionScopeId) -> FTransactorResult { return Unsupported(); }
	auto DTransactor::Undo() -> FTransactorResult { return Unsupported(); }
	auto DTransactor::Undo(FTransactionId) -> FTransactorResult { return Unsupported(); }
	auto DTransactor::Redo() -> FTransactorResult { return Unsupported(); }
	auto DTransactor::Redo(FTransactionId) -> FTransactorResult { return Unsupported(); }
	auto DTransactor::Reset() -> FTransactorResult { return Unsupported(); }
	auto DTransactor::RemoveTransaction(FTransactionId) -> FTransactorResult { return Unsupported(); }
	auto DTransactor::SetTransactionCompletion(FTransactionId, FTransactionDeferredCompletion) -> FTransactorResult { return Unsupported(); }
	auto DTransactor::IsTransactionPending(FTransactionId) const -> bool { return false; }
	auto DTransactor::GetTransactionDetails(FTransactionId, ETransactionOperation) const -> std::string { return {}; }
	auto DTransactor::DiscardCustomChangesByModule(std::string_view) -> FTransactorResult { return Unsupported(); }
	auto DTransactor::CanUndo() const -> bool { return false; }
	auto DTransactor::CanRedo() const -> bool { return false; }
	auto DTransactor::HasPendingOperation() const -> bool { return false; }
	auto DTransactor::GetUndoId() const -> FTransactionId { return 0; }
	auto DTransactor::GetRedoId() const -> FTransactionId { return 0; }
	auto DTransactor::GetUndoDescription() const -> std::string_view { return {}; }
	auto DTransactor::GetRedoDescription() const -> std::string_view { return {}; }
	auto DTransactor::ConsumeEvents() -> std::vector<FTransactionEvent> { return {}; }
	auto DTransactor::GetMountedContentMutationRevision() const -> uint64 { return 1; }
	auto DTransactor::NotifyMountedContentMutation() -> void {}
	auto DTransactor::EstablishSavedState(DPackage&) -> void {}
	auto DTransactor::MarkSaved(DPackage&) -> void {}
	auto DTransactor::InvalidateSavedState(DPackage&) -> void {}
	auto DTransactor::GetPackageRevisionState(const DPackage&) const
		-> std::optional<FPackageRevisionState> { return std::nullopt; }
	auto DTransactor::ForgetPackage(DPackage&) -> void {}

	DTransBuffer::DTransBuffer(const FObjectInitializer& ObjectInitializer)
		: DTransactor(ObjectInitializer)
	{
	}

	auto DTransBuffer::CheckThread() const -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
	}

	auto DTransBuffer::Reject(std::string Message) const -> FTransactorResult
	{
		return {.Code = ETransactorResultCode::Rejected, .Message = std::move(Message)};
	}

	auto DTransBuffer::Begin(const FTransactionContext& Context) -> FTransactorResult
	{
		CheckThread();
		if (State != ETransactorState::Idle && State != ETransactorState::Recording)
			return Reject("Begin is unavailable during a history transition or destruction.");
		if (State == ETransactorState::Idle)
		{
			Pending.emplace(NextTransactionId++, Context);
			State = ETransactorState::Recording;
		}
		const FTransactionScopeId ScopeId = NextScopeId++;
		size_t PendingBytes = 0;
		if (!Pending->TryGetOwnedSize(PendingBytes))
		{
			Pending.reset();
			Savepoints.clear();
			State = ETransactorState::Idle;
			return {.Code = ETransactorResultCode::Failed,
				.Message = "Transaction byte accounting overflowed."};
		}
		Savepoints.push_back({ScopeId, Pending->GetRecordCount(), PendingBytes});
		return {.Code = ETransactorResultCode::Succeeded,
			.TransactionId = Pending->GetId(), .ScopeId = ScopeId};
	}

	auto DTransBuffer::Record(FFocusedTransactionObjectRecord Record) -> FTransactorResult
	{
		CheckThread();
		if (State != ETransactorState::Recording || !Pending || Savepoints.empty())
			return Reject("Record requires an active transaction scope.");
		const uint64 RecordId = Pending->AddRecord(std::move(Record));
		return {.Code = ETransactorResultCode::Succeeded,
			.TransactionId = Pending->GetId(), .ScopeId = Savepoints.back().ScopeId,
			.RecordId = RecordId};
	}

	auto DTransBuffer::Record(FTransactionObjectRecord Record) -> FTransactorResult
	{
		CheckThread();
		if (State != ETransactorState::Recording || !Pending || Savepoints.empty())
			return Reject("Record requires an active transaction scope.");
		const uint64 RecordId = Pending->AddRecord(std::move(Record));
		return {.Code = ETransactorResultCode::Succeeded,
			.TransactionId = Pending->GetId(), .ScopeId = Savepoints.back().ScopeId,
			.RecordId = RecordId};
	}

	auto DTransBuffer::Execute(
		std::unique_ptr<ITransactionCustomChange> Change,
		bool bAlreadyApplied) -> FTransactorResult
	{
		CheckThread();
		if (State != ETransactorState::Idle)
			return Reject("Custom transaction execution requires an idle transactor.");
		if (!Change) return Reject("The custom transaction change is unavailable.");

		FTransaction Transaction(NextTransactionId++, {
			.Name = "CustomChange",
			.Description = std::string(Change->GetDescription()),
		});
		Transaction.AddRecord(std::move(Change));
		PreparePackageTransitions(Transaction);
		const FTransactionId TransactionId = Transaction.GetId();
		if (!bAlreadyApplied)
		{
			State = ETransactorState::Executing;
			PendingTransactionId = TransactionId;
			PendingOperation = ETransactionOperation::Execute;
			Transaction.SetDeferredOperationCompletion(
				[this, TransactionId](bool bSucceeded) {
					CompleteDeferredOperation(
						ETransactionOperation::Execute, TransactionId, bSucceeded);
				});
			std::string Error;
			if (!Transaction.Apply(false, EPropertyChangeOrigin::Edit, &Error))
			{
				Transaction.SetDeferredOperationCompletion({});
				PendingTransactionId = 0;
				State = ETransactorState::Idle;
				QueueEvent(ETransactionEventType::Failed, Transaction, std::move(Error));
				return {.Code = ETransactorResultCode::Failed,
					.TransactionId = TransactionId,
					.Message = Transaction.GetDetails(ETransactionOperation::Execute)};
			}
			if (Transaction.IsDeferredOperationPending())
			{
				Pending.emplace(std::move(Transaction));
				return {.Code = ETransactorResultCode::Succeeded,
					.TransactionId = TransactionId};
			}
			Transaction.SetDeferredOperationCompletion({});
			PendingTransactionId = 0;
			State = ETransactorState::Idle;
		}

		Pending.emplace(std::move(Transaction));
		State = ETransactorState::Recording;
		return FinalizePending();
	}

	auto DTransBuffer::UpdateRecord(
		uint64 RecordId,
		FTransactionObjectRecord Record) -> FTransactorResult
	{
		CheckThread();
		if (State != ETransactorState::Recording || !Pending || Savepoints.empty())
			return Reject("Record update requires an active transaction scope.");
		if (!Pending->UpdateRecord(RecordId, std::move(Record)))
			return Reject("The pending transaction record identifier is unavailable.");
		return {.Code = ETransactorResultCode::Succeeded,
			.TransactionId = Pending->GetId(), .ScopeId = Savepoints.back().ScopeId,
			.RecordId = RecordId};
	}

	auto DTransBuffer::End(FTransactionScopeId ScopeId) -> FTransactorResult
	{
		CheckThread();
		return CloseScope(ScopeId, false);
	}

	auto DTransBuffer::Cancel(FTransactionScopeId ScopeId) -> FTransactorResult
	{
		CheckThread();
		return CloseScope(ScopeId, true);
	}

	auto DTransBuffer::CloseScope(FTransactionScopeId ScopeId, bool bCancel)
		-> FTransactorResult
	{
		if (State != ETransactorState::Recording || !Pending || Savepoints.empty())
			return Reject("No transaction scope is active.");
		if (Savepoints.back().ScopeId != ScopeId)
			return Reject("Transaction scopes must close in reverse begin order.");

		const FTransactionId TransactionId = Pending->GetId();
		const FSavepoint Savepoint = Savepoints.back();
		Savepoints.pop_back();
		if (bCancel) Pending->TruncateRecords(Savepoint.RecordCount);
		if (!Savepoints.empty())
		{
			return {.Code = bCancel ? ETransactorResultCode::Discarded
				: ETransactorResultCode::Succeeded,
				.TransactionId = TransactionId, .ScopeId = ScopeId};
		}

		if (bCancel)
		{
			QueueEvent(ETransactionEventType::Discarded, *Pending, "The outer transaction scope was canceled.");
			Pending.reset();
			State = ETransactorState::Idle;
			return {.Code = ETransactorResultCode::Discarded,
				.TransactionId = TransactionId, .ScopeId = ScopeId};
		}
		return FinalizePending();
	}

	auto DTransBuffer::FinalizePending() -> FTransactorResult
	{
		const FTransactionId TransactionId = Pending->GetId();
		if (Pending->GetRecordCount() == 0)
		{
			QueueEvent(ETransactionEventType::Discarded, *Pending, "The transaction contained no records.");
			Pending.reset();
			State = ETransactorState::Idle;
			return {.Code = ETransactorResultCode::NoOp, .TransactionId = TransactionId};
		}
		PreparePackageTransitions(*Pending);

		size_t EntryBytes = 0;
		if (!Pending->TryGetOwnedSize(EntryBytes))
		{
			QueueEvent(ETransactionEventType::Failed, *Pending, "Transaction byte accounting overflowed.");
			Pending.reset();
			State = ETransactorState::Idle;
			return {.Code = ETransactorResultCode::Failed, .TransactionId = TransactionId,
				.Message = "Transaction byte accounting overflowed."};
		}

		if (Cursor < History.size())
		{
			for (size_t Index = Cursor; Index < History.size(); ++Index)
				QueueEvent(ETransactionEventType::Discarded, History[Index],
					"A new commit replaced the redo branch.");
			History.erase(History.begin() + Cursor, History.end());
		}
		if (!RecalculateOwnedBytes())
		{
			QueueEvent(ETransactionEventType::Failed, *Pending,
				"Retained transaction byte accounting overflowed.");
			Pending.reset();
			State = ETransactorState::Idle;
			return {.Code = ETransactorResultCode::Failed, .TransactionId = TransactionId,
				.Message = "Retained transaction byte accounting overflowed."};
		}
		if (EntryBytes > Limits.MaximumOwnedBytes)
		{
			QueueEvent(ETransactionEventType::Discarded, *Pending,
				"The transaction exceeded the owned-byte limit.");
			Pending.reset();
			RecalculateOwnedBytes();
			State = ETransactorState::Idle;
			return {.Code = ETransactorResultCode::Discarded, .TransactionId = TransactionId,
				.Message = "The transaction exceeded the owned-byte limit."};
		}
		if (OwnedBytes > std::numeric_limits<size_t>::max() - EntryBytes)
		{
			QueueEvent(ETransactionEventType::Failed, *Pending,
				"Retained transaction byte accounting overflowed.");
			Pending.reset();
			State = ETransactorState::Idle;
			return {.Code = ETransactorResultCode::Failed, .TransactionId = TransactionId,
				.Message = "Retained transaction byte accounting overflowed."};
		}

		History.push_back(std::move(*Pending));
		Pending.reset();
		Cursor = History.size();
		ApplyPackageTransitions(History.back(), true);
		if (History.back().MutatesMountedContent()) NotifyMountedContentMutation();
		QueueEvent(ETransactionEventType::Executed, History.back());
		RecalculateOwnedBytes();
		EnforceLimits();
		State = ETransactorState::Idle;
		return {.Code = ETransactorResultCode::Succeeded, .TransactionId = TransactionId};
	}

	auto DTransBuffer::Undo() -> FTransactorResult
	{
		return Undo(0);
	}

	auto DTransBuffer::Undo(FTransactionId ExpectedId) -> FTransactorResult
	{
		CheckThread();
		if (State != ETransactorState::Idle) return Reject("Undo requires an idle transactor.");
		if (Cursor == 0) return {.Code = ETransactorResultCode::NoOp, .Message = "Nothing to undo."};
		FTransaction& Transaction = History[Cursor - 1];
		if (ExpectedId != 0 && Transaction.GetId() != ExpectedId)
			return Reject("The expected transaction is not the Undo head.");
		State = ETransactorState::Undoing;
		PendingTransactionId = Transaction.GetId();
		PendingOperation = ETransactionOperation::Undo;
		Transaction.SetDeferredOperationCompletion(
			[this, TransactionId = Transaction.GetId()](bool bSucceeded) {
				CompleteDeferredOperation(
					ETransactionOperation::Undo, TransactionId, bSucceeded);
			});
		std::string Error;
		if (!Transaction.Apply(true, EPropertyChangeOrigin::Undo, &Error))
		{
			Transaction.SetDeferredOperationCompletion({});
			PendingTransactionId = 0;
			QueueEvent(ETransactionEventType::Failed, Transaction, Error);
			State = ETransactorState::Idle;
			return {.Code = ETransactorResultCode::Failed,
				.TransactionId = Transaction.GetId(), .Message = std::move(Error)};
		}
		if (Transaction.IsDeferredOperationPending())
			return {.Code = ETransactorResultCode::Succeeded,
				.TransactionId = Transaction.GetId()};
		Transaction.SetDeferredOperationCompletion({});
		PendingTransactionId = 0;
		ApplyPackageTransitions(Transaction, false);
		if (Transaction.MutatesMountedContent()) NotifyMountedContentMutation();
		--Cursor;
		QueueEvent(ETransactionEventType::Undone, Transaction);
		State = ETransactorState::Idle;
		return {.Code = ETransactorResultCode::Succeeded,
			.TransactionId = Transaction.GetId()};
	}

	auto DTransBuffer::Redo() -> FTransactorResult
	{
		return Redo(0);
	}

	auto DTransBuffer::Redo(FTransactionId ExpectedId) -> FTransactorResult
	{
		CheckThread();
		if (State != ETransactorState::Idle) return Reject("Redo requires an idle transactor.");
		if (Cursor >= History.size())
			return {.Code = ETransactorResultCode::NoOp, .Message = "Nothing to redo."};
		FTransaction& Transaction = History[Cursor];
		if (ExpectedId != 0 && Transaction.GetId() != ExpectedId)
			return Reject("The expected transaction is not the Redo head.");
		State = ETransactorState::Redoing;
		PendingTransactionId = Transaction.GetId();
		PendingOperation = ETransactionOperation::Redo;
		Transaction.SetDeferredOperationCompletion(
			[this, TransactionId = Transaction.GetId()](bool bSucceeded) {
				CompleteDeferredOperation(
					ETransactionOperation::Redo, TransactionId, bSucceeded);
			});
		std::string Error;
		if (!Transaction.Apply(false, EPropertyChangeOrigin::Redo, &Error))
		{
			Transaction.SetDeferredOperationCompletion({});
			PendingTransactionId = 0;
			QueueEvent(ETransactionEventType::Failed, Transaction, Error);
			State = ETransactorState::Idle;
			return {.Code = ETransactorResultCode::Failed,
				.TransactionId = Transaction.GetId(), .Message = std::move(Error)};
		}
		if (Transaction.IsDeferredOperationPending())
			return {.Code = ETransactorResultCode::Succeeded,
				.TransactionId = Transaction.GetId()};
		Transaction.SetDeferredOperationCompletion({});
		PendingTransactionId = 0;
		ApplyPackageTransitions(Transaction, true);
		if (Transaction.MutatesMountedContent()) NotifyMountedContentMutation();
		++Cursor;
		QueueEvent(ETransactionEventType::Redone, Transaction);
		State = ETransactorState::Idle;
		return {.Code = ETransactorResultCode::Succeeded,
			.TransactionId = Transaction.GetId()};
	}

	auto DTransBuffer::Reset() -> FTransactorResult
	{
		CheckThread();
		if (State != ETransactorState::Idle) return Reject("Reset requires an idle transactor.");
		History.clear();
		Cursor = 0;
		OwnedBytes = 0;
		Events.clear();
		PendingTransactionId = 0;
		TransactionCompletion = {};
		PackageStates.clear();
		return {.Code = ETransactorResultCode::Succeeded};
	}

	auto DTransBuffer::CompleteDeferredOperation(
		ETransactionOperation Operation,
		FTransactionId TransactionId,
		bool bSucceeded) -> void
	{
		if (PendingTransactionId != TransactionId || PendingOperation != Operation)
			return;
		FTransaction* Transaction = Operation == ETransactionOperation::Execute
			? (Pending ? &*Pending : nullptr)
			: FindTransaction(TransactionId);
		if (!Transaction) return;
		Transaction->SetDeferredOperationCompletion({});
		PendingTransactionId = 0;
		if (!bSucceeded)
		{
			QueueEvent(ETransactionEventType::Failed, *Transaction,
				Transaction->GetDetails(Operation));
			if (Operation == ETransactionOperation::Execute) Pending.reset();
			State = ETransactorState::Idle;
			if (TransactionCompletion)
				std::exchange(TransactionCompletion, {})(false);
			return;
		}
		if (Operation == ETransactionOperation::Execute)
		{
			State = ETransactorState::Recording;
			const FTransactorResult Result = FinalizePending();
			if (TransactionCompletion)
				std::exchange(TransactionCompletion, {})(Result.IsSuccess());
			return;
		}
		if (Operation == ETransactionOperation::Undo)
		{
			check(Cursor != 0 && History[Cursor - 1].GetId() == TransactionId);
			ApplyPackageTransitions(*Transaction, false);
			if (Transaction->MutatesMountedContent()) NotifyMountedContentMutation();
			--Cursor;
			QueueEvent(ETransactionEventType::Undone, History[Cursor]);
		}
		else
		{
			check(Cursor < History.size() && History[Cursor].GetId() == TransactionId);
			ApplyPackageTransitions(*Transaction, true);
			if (Transaction->MutatesMountedContent()) NotifyMountedContentMutation();
			QueueEvent(ETransactionEventType::Redone, History[Cursor]);
			++Cursor;
		}
		State = ETransactorState::Idle;
		if (TransactionCompletion)
			std::exchange(TransactionCompletion, {})(true);
	}

	auto DTransBuffer::RemoveTransaction(FTransactionId TransactionId) -> FTransactorResult
	{
		CheckThread();
		if (State != ETransactorState::Idle)
			return Reject("Transaction removal requires an idle transactor.");
		const auto It = std::ranges::find(History, TransactionId, &FTransaction::GetId);
		if (It == History.end())
			return {.Code = ETransactorResultCode::NoOp, .TransactionId = TransactionId};
		const size_t Index = static_cast<size_t>(std::distance(History.begin(), It));
		QueueEvent(ETransactionEventType::Discarded, *It,
			"The legacy property-history bridge released the transaction.");
		History.erase(It);
		if (Index < Cursor) --Cursor;
		if (!RecalculateOwnedBytes())
		{
			History.clear();
			Cursor = 0;
			OwnedBytes = 0;
			return {.Code = ETransactorResultCode::Failed,
				.TransactionId = TransactionId,
				.Message = "Retained transaction byte accounting overflowed after removal."};
		}
		return {.Code = ETransactorResultCode::Succeeded, .TransactionId = TransactionId};
	}

	auto DTransBuffer::SetTransactionCompletion(
		FTransactionId TransactionId,
		FTransactionDeferredCompletion Completion) -> FTransactorResult
	{
		CheckThread();
		if (PendingTransactionId != TransactionId)
			return Reject("The transaction is not awaiting deferred completion.");
		TransactionCompletion = std::move(Completion);
		return {.Code = ETransactorResultCode::Succeeded,
			.TransactionId = TransactionId};
	}

	auto DTransBuffer::IsTransactionPending(FTransactionId TransactionId) const -> bool
	{
		return TransactionId != 0 && PendingTransactionId == TransactionId;
	}

	auto DTransBuffer::FindTransaction(FTransactionId TransactionId) -> FTransaction*
	{
		if (Pending && Pending->GetId() == TransactionId) return &*Pending;
		const auto It = std::ranges::find(History, TransactionId, &FTransaction::GetId);
		return It == History.end() ? nullptr : &*It;
	}

	auto DTransBuffer::FindTransaction(FTransactionId TransactionId) const
		-> const FTransaction*
	{
		if (Pending && Pending->GetId() == TransactionId) return &*Pending;
		const auto It = std::ranges::find(History, TransactionId, &FTransaction::GetId);
		return It == History.end() ? nullptr : &*It;
	}

	auto DTransBuffer::GetTransactionDetails(
		FTransactionId TransactionId,
		ETransactionOperation Operation) const -> std::string
	{
		const FTransaction* Transaction = FindTransaction(TransactionId);
		return Transaction ? Transaction->GetDetails(Operation) : std::string{};
	}

	auto DTransBuffer::DiscardCustomChangesByModule(std::string_view ModuleName)
		-> FTransactorResult
	{
		CheckThread();
		if (ModuleName.empty()) return Reject("A module name is required.");
		if (PendingTransactionId != 0)
		{
			const FTransaction* Active = FindTransaction(PendingTransactionId);
			if (Active && Active->IsOwnedByModule(ModuleName))
				return Reject("A deferred custom change from the module is still pending.");
		}
		if (State == ETransactorState::Recording && Pending
			&& Pending->IsOwnedByModule(ModuleName))
			return Reject("A custom change from the module is still being recorded.");
		size_t Removed = 0;
		for (size_t Index = History.size(); Index-- > 0;)
		{
			if (!History[Index].IsOwnedByModule(ModuleName)) continue;
			History.erase(History.begin() + Index);
			if (Index < Cursor) --Cursor;
			++Removed;
		}
		if (!RecalculateOwnedBytes())
		{
			History.clear();
			Cursor = 0;
			OwnedBytes = 0;
			return {.Code = ETransactorResultCode::Failed,
				.Message = "Retained transaction byte accounting overflowed during module drain."};
		}
		return {.Code = Removed == 0 ? ETransactorResultCode::NoOp
			: ETransactorResultCode::Succeeded,
			.Message = std::format("Removed {} module-owned transaction(s).", Removed)};
	}

	auto DTransBuffer::SetLimits(FTransactionBufferLimits InLimits) -> FTransactorResult
	{
		CheckThread();
		if (State != ETransactorState::Idle) return Reject("Limits can change only while idle.");
		if (InLimits.MaximumEntries == 0 || InLimits.MaximumOwnedBytes == 0)
			return Reject("Transaction limits must be non-zero.");
		size_t ProjectedCount = History.size();
		size_t ProjectedBytes = OwnedBytes;
		size_t RemovalCount = 0;
		while ((ProjectedCount > InLimits.MaximumEntries
			|| ProjectedBytes > InLimits.MaximumOwnedBytes)
			&& RemovalCount < Cursor)
		{
			size_t EntryBytes = 0;
			if (!History[RemovalCount].TryGetOwnedSize(EntryBytes)
				|| EntryBytes > ProjectedBytes)
			{
				return {.Code = ETransactorResultCode::Failed,
					.Message = "Retained transaction byte accounting is inconsistent."};
			}
			ProjectedBytes -= EntryBytes;
			--ProjectedCount;
			++RemovalCount;
		}
		if (ProjectedCount > InLimits.MaximumEntries
			|| ProjectedBytes > InLimits.MaximumOwnedBytes)
		{
			return Reject("The requested limits cannot retain the current redo branch.");
		}
		Limits = InLimits;
		EnforceLimits();
		return {.Code = ETransactorResultCode::Succeeded};
	}

	auto DTransBuffer::ConsumeEvents() -> std::vector<FTransactionEvent>
	{
		CheckThread();
		return std::exchange(Events, {});
	}

	auto DTransBuffer::CanUndo() const -> bool
	{
		return State == ETransactorState::Idle && Cursor != 0;
	}

	auto DTransBuffer::CanRedo() const -> bool
	{
		return State == ETransactorState::Idle && Cursor < History.size();
	}

	auto DTransBuffer::HasPendingOperation() const -> bool
	{
		return PendingTransactionId != 0;
	}

	auto DTransBuffer::AddReferencedObjects(FReferenceCollector& Collector) -> void
	{
		DTransactor::AddReferencedObjects(Collector);
		if (Pending) Pending->AddReferencedObjects(Collector);
		for (const FTransaction& Transaction : History)
			Transaction.AddReferencedObjects(Collector);
		for (const auto& [Package, PackageState] : PackageStates)
		{
			(void)Package;
			PackageState.Package.AddReferencedObjects(Collector);
		}
	}

	auto DTransBuffer::BeginDestroy() -> void
	{
		CheckThread();
		State = ETransactorState::Destroying;
		if (Pending) Pending->SetDeferredOperationCompletion({});
		for (FTransaction& Transaction : History)
			Transaction.SetDeferredOperationCompletion({});
		Pending.reset();
		Savepoints.clear();
		History.clear();
		Events.clear();
		Cursor = 0;
		OwnedBytes = 0;
		PendingTransactionId = 0;
		TransactionCompletion = {};
		PackageStates.clear();
		DTransactor::BeginDestroy();
	}

	auto DTransBuffer::GetUndoId() const -> FTransactionId
	{
		return Cursor == 0 ? 0 : History[Cursor - 1].GetId();
	}

	auto DTransBuffer::GetRedoId() const -> FTransactionId
	{
		return Cursor >= History.size() ? 0 : History[Cursor].GetId();
	}

	auto DTransBuffer::GetUndoDescription() const -> std::string_view
	{
		return Cursor == 0 ? std::string_view{} : History[Cursor - 1].GetContext().Description;
	}

	auto DTransBuffer::GetRedoDescription() const -> std::string_view
	{
		return Cursor >= History.size() ? std::string_view{}
			: std::string_view(History[Cursor].GetContext().Description);
	}

	auto DTransBuffer::AllocateRevision() -> FRevisionId
	{
		check(NextRevision != 0);
		return NextRevision++;
	}

	auto DTransBuffer::FindPackageState(const DPackage& Package)
		-> FTrackedPackageState*
	{
		const auto It = PackageStates.find(const_cast<DPackage*>(&Package));
		return It == PackageStates.end() ? nullptr : &It->second;
	}

	auto DTransBuffer::FindPackageState(const DPackage& Package) const
		-> const FTrackedPackageState*
	{
		const auto It = PackageStates.find(const_cast<DPackage*>(&Package));
		return It == PackageStates.end() ? nullptr : &It->second;
	}

	auto DTransBuffer::EnsurePackageState(DPackage& Package) -> FTrackedPackageState&
	{
		if (FTrackedPackageState* State = FindPackageState(Package)) return *State;
		const FRevisionId Revision = AllocateRevision();
		const bool bCheckpointValid = !Package.IsDirty();
		auto [It, bInserted] = PackageStates.emplace(&Package, FTrackedPackageState{
			.Package = FPersistentObjectRef(&Package),
			.CurrentRevision = Revision,
			.SavedRevision = bCheckpointValid ? Revision : 0,
			.bCheckpointValid = bCheckpointValid,
		});
		check(bInserted);
		return It->second;
	}

	auto DTransBuffer::PreparePackageTransitions(FTransaction& Transaction) -> void
	{
		if (Transaction.HasPackageTransitions()) return;
		std::vector<FTransactionPackageRevisionTransition> Transitions;
		std::unordered_set<DPackage*> AddedPackages;
		for (DPackage* Package : Transaction.GetAffectedPackages())
		{
			if (!IsValid(Package) || !Package->IsAssetPackage()
				|| !AddedPackages.insert(Package).second) continue;
			const FTrackedPackageState* State = FindPackageState(*Package);
			const FRevisionId BeforeRevision = State
				? State->CurrentRevision : AllocateRevision();
			Transitions.push_back({
				.Package = FPersistentObjectRef(Package),
				.BeforeRevision = BeforeRevision,
				.AfterRevision = AllocateRevision(),
				.InitialSavedRevision = State ? State->SavedRevision
					: (Package->IsDirty() ? 0 : BeforeRevision),
				.bInitialCheckpointValid = State
					? State->bCheckpointValid : !Package->IsDirty(),
			});
		}
		Transaction.SetPackageTransitions(std::move(Transitions));
	}

	auto DTransBuffer::ApplyPackageTransitions(
		const FTransaction& Transaction,
		bool bForward) -> void
	{
		for (const FTransactionPackageRevisionTransition& Transition
			: Transaction.GetPackageTransitions())
		{
			auto* Package = Cast<DPackage>(Transition.Package.Resolve());
			if (!Package) continue;
			FTrackedPackageState* State = FindPackageState(*Package);
			if (!State)
			{
				auto [It, bInserted] = PackageStates.emplace(Package,
					FTrackedPackageState{
						.Package = FPersistentObjectRef(Package),
						.CurrentRevision = Transition.BeforeRevision,
						.SavedRevision = Transition.InitialSavedRevision,
						.bCheckpointValid = Transition.bInitialCheckpointValid,
					});
				check(bInserted);
				State = &It->second;
			}
			State->CurrentRevision = bForward
				? Transition.AfterRevision : Transition.BeforeRevision;
			SynchronizeDirtyState(*State);
		}
	}

	auto DTransBuffer::SynchronizeDirtyState(FTrackedPackageState& State) -> void
	{
		auto* Package = Cast<DPackage>(State.Package.Resolve());
		if (!Package) return;
		if (!State.bCheckpointValid || State.CurrentRevision != State.SavedRevision)
			Package->MarkDirty();
		else
			Package->ClearDirty();
	}

	auto DTransBuffer::GetMountedContentMutationRevision() const -> uint64
	{
		return MountedContentMutationRevision;
	}

	auto DTransBuffer::NotifyMountedContentMutation() -> void
	{
		check(MountedContentMutationRevision != std::numeric_limits<uint64>::max());
		++MountedContentMutationRevision;
	}

	auto DTransBuffer::EstablishSavedState(DPackage& Package) -> void
	{
		ForgetPackage(Package);
		const FRevisionId Revision = AllocateRevision();
		PackageStates.emplace(&Package, FTrackedPackageState{
			.Package = FPersistentObjectRef(&Package),
			.CurrentRevision = Revision,
			.SavedRevision = Revision,
			.bCheckpointValid = true,
		});
		Package.ClearDirty();
	}

	auto DTransBuffer::MarkSaved(DPackage& Package) -> void
	{
		FTrackedPackageState& State = EnsurePackageState(Package);
		State.SavedRevision = State.CurrentRevision;
		State.bCheckpointValid = true;
		SynchronizeDirtyState(State);
	}

	auto DTransBuffer::InvalidateSavedState(DPackage& Package) -> void
	{
		FTrackedPackageState& State = EnsurePackageState(Package);
		State.SavedRevision = 0;
		State.bCheckpointValid = false;
		SynchronizeDirtyState(State);
	}

	auto DTransBuffer::GetPackageRevisionState(const DPackage& Package) const
		-> std::optional<FPackageRevisionState>
	{
		const FTrackedPackageState* State = FindPackageState(Package);
		if (!State) return std::nullopt;
		return FPackageRevisionState{
			.CurrentRevision = State->CurrentRevision,
			.SavedRevision = State->SavedRevision,
			.bCheckpointValid = State->bCheckpointValid,
		};
	}

	auto DTransBuffer::ForgetPackage(DPackage& Package) -> void
	{
		if (State != ETransactorState::Idle) return;
		for (size_t Index = History.size(); Index-- > 0;)
		{
			if (!History[Index].ReferencesPackage(Package)) continue;
			History.erase(History.begin() + Index);
			if (Index < Cursor) --Cursor;
		}
		PackageStates.erase(&Package);
		(void)RecalculateOwnedBytes();
	}

	auto DTransBuffer::RecalculateOwnedBytes() -> bool
	{
		size_t Total = 0;
		for (const FTransaction& Transaction : History)
		{
			size_t Bytes = 0;
			if (!Transaction.TryGetOwnedSize(Bytes)
				|| Total > std::numeric_limits<size_t>::max() - Bytes) return false;
			Total += Bytes;
		}
		OwnedBytes = Total;
		return true;
	}

	auto DTransBuffer::EnforceLimits() -> void
	{
		while (!History.empty()
			&& Cursor != 0
			&& (History.size() > Limits.MaximumEntries || OwnedBytes > Limits.MaximumOwnedBytes))
		{
			FTransactionEvent Event{
				.Type = ETransactionEventType::Evicted,
				.Operation = ETransactionOperation::Execute,
				.Id = History.front().GetId(),
				.Description = History.front().GetContext().Description,
			};
			History.erase(History.begin());
			if (Cursor != 0) --Cursor;
			Events.push_back(std::move(Event));
			if (!RecalculateOwnedBytes())
			{
				History.clear();
				Cursor = 0;
				OwnedBytes = 0;
				break;
			}
		}
	}

	auto DTransBuffer::QueueEvent(
		ETransactionEventType Type,
		const FTransaction& Transaction,
		std::string Details) -> void
	{
		ETransactionOperation Operation = ETransactionOperation::Execute;
		if (Type == ETransactionEventType::Undone
			|| (Type == ETransactionEventType::Failed
				&& State == ETransactorState::Undoing))
			Operation = ETransactionOperation::Undo;
		else if (Type == ETransactionEventType::Redone
			|| (Type == ETransactionEventType::Failed
				&& State == ETransactorState::Redoing))
			Operation = ETransactionOperation::Redo;
		if (Details.empty()) Details = Transaction.GetDetails(Operation);
		Events.push_back({
			.Type = Type,
			.Operation = Operation,
			.Id = Transaction.GetId(),
			.Description = Transaction.GetContext().Description,
			.Details = std::move(Details),
		});
	}
}
