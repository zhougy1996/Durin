#include "Transactions/Transactor.h"

#include "Editor/EditorEngine.h"

#include "PropertyEditor/PropertyEditing.h"
#include "DObject/Archive.h"
#include "DObject/DObjectArray.h"
#include "DObject/Class.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "DObject/Property.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/ObjectGraphReplacement.h"
#include "CoreGlobals.h"
#include "Logging/LogMacros.h"
#include "Threading/RunnableThread.h"

namespace Durin::Editor
{
	namespace
	{
		struct FTransactionBusyScope
		{
			bool& Busy;
			explicit FTransactionBusyScope(bool& Value) : Busy(Value) { Busy = true; }
			~FTransactionBusyScope() { Busy = false; }
		};
		auto AddSize(size_t& Total, size_t Value) -> bool
		{
			if (Total > std::numeric_limits<size_t>::max() - Value) return false;
			Total += Value;
			return true;
		}
	}

	auto FormatTransactorRejection(const FTransactorRejection& Rejection) -> std::string
	{
		switch (Rejection.Reason)
		{
		case ETransactorRejectionReason::BeginState: return "Begin is unavailable during a history transition or destruction.";
		case ETransactorRejectionReason::RecordScope: return "Record requires an active transaction scope.";
		case ETransactorRejectionReason::ExecuteState: return "Custom changes are unavailable during a history transition or destruction.";
		case ETransactorRejectionReason::MissingCustom: return "The custom transaction change is unavailable.";
		case ETransactorRejectionReason::UndoState: return "Undo requires an idle transactor.";
		case ETransactorRejectionReason::UndoHead: return "The expected transaction is not the Undo head.";
		case ETransactorRejectionReason::RedoState: return "Redo requires an idle transactor.";
		case ETransactorRejectionReason::RedoHead: return "The expected transaction is not the Redo head.";
		case ETransactorRejectionReason::ResetState: return "Reset requires an idle transactor.";
		case ETransactorRejectionReason::ModuleName: return "A module name is required.";
		case ETransactorRejectionReason::ModuleState: return "Module history retirement requires an idle transactor.";
		}
		return {};
	}

	auto FormatTransactorResult(const FTransactorResult& Result) -> std::string
	{
		std::string Message;
		if (Result.RejectionCause) Message = FormatTransactorRejection(*Result.RejectionCause);
		else if (Result.ApplyCause)
		{
			Message = Result.ApplyCause->Message;
			if (Result.Code == ETransactorResultCode::RecoveryRequired)
			{
				for (const auto& Failure : Result.ApplyCause->RollbackFailures)
					Message += std::format(" Rollback record {} failed: {}", Failure.RecordIndex, Failure.Message);
				Message += " Transaction history is disabled; reload the editor session after recovering affected data.";
			}
		}
		else if (Result.FailureCause)
		{
			const auto& Error = *Result.FailureCause;
			switch (Error.Code)
			{
			case ETransactorFailure::EntryAccounting: Message = "Transaction byte accounting overflowed."; break;
			case ETransactorFailure::RetainedAccounting: Message = "Retained transaction byte accounting overflowed."; break;
			case ETransactorFailure::ModuleAccounting: Message = "Retained transaction byte accounting overflowed during module drain."; break;
			}
		}
		else switch (Result.Notice)
		{
		case ETransactorNotice::None: break;
		case ETransactorNotice::NothingUndo: Message = "Nothing to undo."; break;
		case ETransactorNotice::NothingRedo: Message = "Nothing to redo."; break;
		case ETransactorNotice::ModuleDrain: Message = std::format("Removed {} module-owned transaction(s).", Result.NoticeCount); break;
		}
		return Message;
	}

	FTransactionRecord::FTransactionRecord(std::unique_ptr<FCommandChange> Change, DObject* Object)
		: Data(std::move(Change)), CustomObject(Object)
	{
		if (Object) ModifiedObjects.Add(FPersistentObjectRef(Object));
		const auto& Custom = std::get<std::unique_ptr<FCommandChange>>(Data);
		if (Custom)
			for (DObject* Participant : Custom->GetModifiedObjects())
				ModifiedObjects.Add(FPersistentObjectRef(Participant));
		if (CustomObject.IsNull() && ModifiedObjects.Num() == 1)
			CustomObject = ModifiedObjects.GetReferences().front();
	}

	auto FTransactionRecord::HasReplacementReferences(const FObjectReplacementMap& Map) const -> bool
	{
		if (const auto* Record = std::get_if<FTransactionObjectRecord>(&Data)) return Record->HasReplacementReferences(Map);
		class FCheck final : public FReferenceCollector
		{
		public:
			explicit FCheck(const FObjectReplacementMap& InMap) : Map(InMap) {}
			auto AddReferencedObject(DObject*& Object) -> void override { bMapped |= Map.Find(Object) != nullptr; }
			const FObjectReplacementMap& Map;
			bool bMapped = false;
		} Check(Map);
		AddReferencedObjects(Check);
		return Check.bMapped;
	}

	auto FTransactionRecord::CreateReplacement(const FObjectReplacementMap& Map) const -> std::expected<FTransactionRecord, std::string>
	{
		if (const auto* Object = std::get_if<FTransactionObjectRecord>(&Data))
		{
			auto Result = Object->CreateReplacement(Map);
			if (!Result) return std::unexpected(Result.error());
			return FTransactionRecord(std::move(*Result));
		}
		const auto& Source = std::get<std::unique_ptr<FCommandChange>>(Data);
		if (!Source) return std::unexpected("A missing custom command cannot be reinstanced.");
		auto Command = Source->CreateReplacement(Map);
		if (!Command) return std::unexpected(Command.error());
		if (*Command && Command->get() == Source.get())
		{
			(void)Command->release(); // Reject an invalid alias without deleting retained history.
			return std::unexpected("Command reinstancing must return independent state.");
		}
		if (!*Command) return std::unexpected("Command reinstancing returned no state.");
		auto Object = Map.RemapObjectKey(CustomObject.GetKey());
		if (!Object) return std::unexpected(ToString(Object.error()));
		FTransactionRecord Result(std::move(*Command), ResolveObjectKey(*Object));
		std::vector<FPersistentObjectRef> Expected;
		for (const auto& Ref : ModifiedObjects.GetReferences())
		{
			auto Key = Map.RemapObjectKey(Ref.GetKey());
			if (!Key) return std::unexpected(ToString(Key.error()));
			Expected.push_back(FPersistentObjectRef::FromKey(*Key));
		}
		if (!std::ranges::is_permutation(Expected, Result.ModifiedObjects.GetReferences()))
			return std::unexpected("Command reinstancing changed its declared modified participants.");
		std::vector<DPackage*> ExpectedPackages;
		for (auto* Package : Source->GetAffectedPackages())
		{
			auto Key = Map.RemapObjectKey(FObjectKey(Package));
			if (!Key) return std::unexpected(ToString(Key.error()));
			ExpectedPackages.push_back(Cast<DPackage>(ResolveObjectKey(*Key)));
		}
		if (!std::ranges::is_permutation(ExpectedPackages, std::get<std::unique_ptr<FCommandChange>>(Result.Data)->GetAffectedPackages()))
			return std::unexpected("Command reinstancing changed its declared affected packages.");
		return Result;
	}

	auto FTransactionRecord::GetModifiedObjects() const -> std::vector<DObject*>
	{
		if (const auto* Record = std::get_if<FTransactionObjectRecord>(&Data))
			return {Record->GetTarget().Resolve()};
		std::vector<DObject*> Result;
		for (const auto& Reference : ModifiedObjects.GetReferences())
			if (auto* Object = Reference.Resolve()) Result.push_back(Object);
		return Result;
	}

	auto FTransactionRecord::Validate() const -> FTransactionRecordResult
	{
		for (const auto& Reference : ModifiedObjects.GetReferences())
			if (Reference.IsStale()) return std::unexpected("A modified transaction participant is unavailable.");
		return std::visit([&](const auto& Record) -> FTransactionRecordResult {
			using T = std::decay_t<decltype(Record)>;
			if constexpr (std::is_same_v<T, FTransactionObjectRecord>)
			{
				const auto Result = Record.Validate();
				if (!Result) return std::unexpected(FormatTransactionObjectRecordError(Result.error()));
			}
			else if (!Record) return std::unexpected(std::string("The custom transaction change is unavailable."));
			return {};
		}, Data);
	}

	auto FTransactionRecord::PrepareReplay() -> FTransactionRecordResult
	{
		if (auto* Object = std::get_if<FTransactionObjectRecord>(&Data))
			if (auto Prepared = Object->PrepareReplay(); !Prepared)
				return std::unexpected(FormatTransactionObjectRecordError(Prepared.error()));
		return {};
	}
	auto FTransactionRecord::ClearPreparedReplay() -> void
	{
		if (auto* Object = std::get_if<FTransactionObjectRecord>(&Data)) Object->ClearPreparedReplay();
	}

	auto FTransactionRecord::CaptureAfter() -> FTransactionRecordResult
	{
		if (auto* Record = std::get_if<FTransactionObjectRecord>(&Data))
			if (auto Result = Record->CaptureAfter(); !Result)
				return std::unexpected(FormatTransactionObjectRecordError(Result.error()));
		return {};
	}

	auto FTransactionRecord::IsNoOp() const -> bool
	{
		if (const auto* Record = std::get_if<FTransactionObjectRecord>(&Data))
			return Record->IsNoOp();
		return false;
	}
	auto FTransactionRecord::GetReplayAnnotation(DObject* Object) const -> std::optional<std::shared_ptr<ITransactionObjectAnnotation>>
	{
		if (const auto* Record = std::get_if<FTransactionObjectRecord>(&Data); Record && Record->GetTarget().Resolve() == Object)
			return Record->GetReplayAnnotation();
		return std::nullopt;
	}

	auto FTransactionRecord::Apply(bool bBefore, EPropertyChangeOrigin Origin, bool* OutRecoveryRequired, bool bFlipState) -> FTransactionRecordResult
	{
		if (OutRecoveryRequired) *OutRecoveryRequired = false;
		return std::visit([&](auto& Record) -> FTransactionRecordResult {
			using T = std::decay_t<decltype(Record)>;
			if constexpr (std::is_same_v<T, FTransactionObjectRecord>)
			{
				try
				{
					const auto Result = Record.Apply(bBefore, Origin, bFlipState);
					if (!Result)
					{
						if (OutRecoveryRequired) *OutRecoveryRequired = Result.error().bRecoveryRequired;
						return std::unexpected(FormatTransactionObjectRecordError(Result.error()));
					}
				}
				catch (const std::exception& Error)
				{
					if (OutRecoveryRequired) *OutRecoveryRequired = true;
					return std::unexpected(std::format("Object transaction replay threw: {}", Error.what()));
				}
				catch (...)
				{
					if (OutRecoveryRequired) *OutRecoveryRequired = true;
					return std::unexpected(std::string("Object transaction replay threw an unknown exception."));
				}
			}
			else
			{
				if (!Record) return std::unexpected(std::string("The custom transaction change is unavailable."));
				try
				{
					if (const auto Applied = bBefore ? Record->Revert() : Record->Apply(); !Applied)
					{
						if (OutRecoveryRequired) *OutRecoveryRequired = Applied.error().bRecoveryRequired;
						return std::unexpected(Applied.error().Message);
					}
				}
				catch (const std::exception& Error)
				{
					if (OutRecoveryRequired) *OutRecoveryRequired = true;
					return std::unexpected(std::format("Custom transaction replay threw: {}", Error.what()));
				}
				catch (...)
				{
					if (OutRecoveryRequired) *OutRecoveryRequired = true;
					return std::unexpected(std::string("Custom transaction replay threw an unknown exception."));
				}
			}
			return {};
		}, Data);
	}

	auto FTransactionRecord::AddReferencedObjects(FReferenceCollector& Collector) const -> void
	{
		ModifiedObjects.AddReferencedObjects(Collector);
		std::visit([&](const auto& Record) {
			using T = std::decay_t<decltype(Record)>;
			if constexpr (std::is_same_v<T, std::unique_ptr<FCommandChange>>)
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
			if constexpr (std::is_same_v<T, std::unique_ptr<FCommandChange>>)
			{
				if (!Record) return false;
				const size_t Allocated = Record->GetAllocatedSize();
				if (Allocated > std::numeric_limits<size_t>::max()
					- sizeof(FCommandChange)) return false;
				size_t ParticipantBytes = 0;
				if (!ModifiedObjects.TryGetAllocatedSize(ParticipantBytes)) return false;
				OutBytes = sizeof(FCommandChange) + Allocated;
				if (!AddSize(OutBytes, ParticipantBytes)) return false;
				return true;
			}
			else return Record.TryGetAllocatedSize(OutBytes);
		}, Data);
	}

	auto FTransactionRecord::GetDetails(ETransactionOperation Operation) const -> std::string
	{
		if (const auto* Change = std::get_if<std::unique_ptr<FCommandChange>>(&Data);
			Change && *Change)
			return (*Change)->GetDetails(Operation);
		return {};
	}

	auto FTransactionRecord::GetAffectedPackages() const -> std::vector<DPackage*>
	{
		if (const auto* Object = std::get_if<FTransactionObjectRecord>(&Data)) return Object->GetAffectedPackages();
		if (const auto* Change = std::get_if<std::unique_ptr<FCommandChange>>(&Data);
			Change && *Change)
		{
			const auto Packages = (*Change)->GetAffectedPackages();
			return std::vector<DPackage*>(Packages.begin(), Packages.end());
		}
		return {};
	}

	auto FTransactionRecord::MutatesMountedContent() const -> bool
	{
		if (const auto* Change = std::get_if<std::unique_ptr<FCommandChange>>(&Data);
			Change && *Change)
			return (*Change)->MutatesMountedContent();
		return false;
	}

	auto FTransactionRecord::IsOwnedByModule(std::string_view ModuleName) const -> bool
	{
		if (const auto* Change = std::get_if<std::unique_ptr<FCommandChange>>(&Data);
			Change && *Change)
			return (*Change)->GetOwningModule() == ModuleName;
		return false;
	}

	FTransaction::FTransaction(FTransactionId InId, FTransactionContext InContext, DTransBuffer* InRecordingOwner)
		: RecordingOwner(InRecordingOwner), Id(InId)
		, Context(std::move(InContext))
	{
		PrimaryObject = FPersistentObjectRef(Context.PrimaryObject);
		Context.PrimaryObject = nullptr;
	}

	auto FTransaction::GetContext() const -> FTransactionContext
	{
		auto Result = Context;
		Result.PrimaryObject = PrimaryObject.Resolve();
		Result.TransactionId = Id;
		Result.OperationId = OperationId;
		return Result;
	}
	auto FTransaction::SetPrimaryObject(DObject* Object) -> void
	{
		if (bBusy || bFinalized) return;
		if (PrimaryObject.IsNull()) PrimaryObject = FPersistentObjectRef(Object);
	}
	auto FTransaction::BeginOperation() -> FTransactionCaptureResult
	{
		if (OperationId != 0) return std::unexpected("A transaction operation is already active.");
		OperationId = AllocateTransactionOperationId();
		if (OperationId == 0) return std::unexpected("Transaction operation identities are exhausted.");
		return {};
	}
	auto FTransaction::EndOperation() -> FTransactionCaptureResult
	{
		if (bBusy) return std::unexpected("A transaction callback or capture is in progress.");
		if (OperationId == 0) return std::unexpected("No transaction operation is active.");
		OperationId = 0;
		return {};
	}
	auto FTransaction::StoreUndo(DObject* Object, std::unique_ptr<FCommandChange> Change) -> FTransactionCaptureResult
	{
		if (bBusy || bFinalized) return std::unexpected("Transaction records are closed to capture.");
		if (!IsValid(Object) || !Change) return std::unexpected("Object-bound custom capture requires a live object and change.");
		if (RecordingOwner)
		{
			if (GUndo != this) return std::unexpected("The transaction is not the active recording.");
			const auto Result = RecordingOwner->StoreUndo(Object, std::move(Change));
			if (!Result) return std::unexpected(FormatTransactorResult(Result));
		}
		else AddRecord(std::move(Change), Object);
		return {};
	}
	auto FTransaction::SnapshotObject(DObject* Object, std::span<const FProperty* const> Properties) -> FTransactionCaptureResult
	{
		if (bFinalized && !bBusy) return {};
		if (bBusy || OperationId == 0 || !IsValid(Object)
			|| (RecordingOwner && (GUndo != this || RecordingOwner->GetState() != ETransactorState::Recording)))
			return std::unexpected("Object snapshots require an active recording outside transaction callbacks.");
		FTransactionCaptureResult Result;
		{
			FGarbageCollectionDeferralScope DeferCollection;
			FTransactionBusyScope BusyScope(bBusy);
			for (auto& Record : Records)
				if (auto* ObjectRecord = std::get_if<FTransactionObjectRecord>(&Record.Data); ObjectRecord && ObjectRecord->GetTarget().Resolve() == Object)
				{
					Result = DispatchObjectEvents(ETransactionObjectEventType::Snapshot, ETransactionOperation::Execute, ObjectRecord, Properties);
					break;
				}
		}
		if (!Result && bObserverRecoveryRequired && RecordingOwner)
			RecordingOwner->LastResult = RecordingOwner->HandleApplyFailure(*this, {.Status = ETransactionApplyFailure::RecoveryRequired, .Message = Result.error()});
		return Result;
	}
	// Explicit migration adapters, replaced in Stages 7 and 8.
	auto FTransaction::SaveArray(const FTransactionArrayCapture&) -> FTransactionCaptureResult
	{ return std::unexpected("Transaction array operations are not implemented yet."); }
	auto FTransaction::ContainsPieObjects() const -> std::expected<bool, std::string>
	{ return std::unexpected("PIE package transaction inspection is not implemented yet."); }

	auto FTransaction::AddRecord(FTransactionObjectRecord Record) -> uint64
	{
		if (bBusy || bFinalized) return 0;
		for (size_t Index = 0; Index < Records.size(); ++Index)
		{
			auto* Existing = std::get_if<FTransactionObjectRecord>(&Records[Index].Data);
			if (Existing && Existing->GetTarget() == Record.GetTarget())
			{
				return Index + 1;
			}
		}
		Records.emplace_back(std::move(Record));
		return Records.size();
	}

	auto FTransaction::AddRecord(std::unique_ptr<FCommandChange> Change, DObject* Object) -> uint64
	{
		if (bBusy || bFinalized) return 0;
		Records.emplace_back(std::move(Change), Object);
		return Records.size();
	}

	auto FTransaction::SaveObject(DObject* Object) -> void
	{
		if (RecordingOwner) (void)RecordingOwner->CaptureObject(Object);
		else (void)CaptureObject(Object);
	}
	auto FTransaction::ContainsObject(const DObject* Object) const -> bool
	{
		if (!Object) return false;
		return std::ranges::any_of(Records, [&](const auto& Record) { return std::ranges::contains(Record.GetModifiedObjects(), Object); });
	}
	auto FTransaction::CaptureObject(DObject* Object) -> FTransactionRecordResult
	{
		if (bBusy || bFinalized) return std::unexpected("Transaction records are closed to capture.");
		FTransactionBusyScope BusyScope(bBusy);
		// A custom record already owns this object's undo representation, as in UE.
		if (std::ranges::any_of(Records, [&](const auto& Record) {
			return Record.CustomObject.Resolve() == Object;
		})) return {};
		for (auto& Record : Records)
		{
			if (auto* Existing = std::get_if<FTransactionObjectRecord>(&Record.Data);
				Existing && Existing->GetTarget().Resolve() == Object)
			{
				return {};
			}
		}
		auto Record = FTransactionObjectRecord::CaptureWhole(*Object);
		if (!Record) return std::unexpected(FormatTransactionObjectRecordError(Record.error()));
		Records.emplace_back(std::move(*Record));
		return {};
	}

	auto FTransaction::Finalize() -> FTransactionCaptureResult
	{
		if (bBusy || bFinalized) return std::unexpected("The transaction is busy or already finalized.");
		FGarbageCollectionDeferralScope DeferCollection;
		FTransactionBusyScope BusyScope(bBusy);
		for (auto& Record : Records)
			if (auto Result = Record.CaptureAfter(); !Result) return Result;
		if (auto Result = DispatchObjectEvents(ETransactionObjectEventType::Finalized, ETransactionOperation::Execute); !Result) return Result;
		for (auto& Record : Records) if (auto* ObjectRecord = std::get_if<FTransactionObjectRecord>(&Record.Data)) ObjectRecord->ClearSnapshot();
		bFinalized = true;
		return {};
	}

	auto FTransaction::DispatchObjectEvents(ETransactionObjectEventType Type, ETransactionOperation Operation,
		FTransactionObjectRecord* SnapshotRecord, std::span<const FProperty* const> Selected) -> FTransactionCaptureResult
	{
		bool bDispatched = false;
		try
		{
			std::vector<const FTransactionObjectRecord*> ObjectRecords;
			for (const auto& Record : Records)
				if (const auto* ObjectRecord = std::get_if<FTransactionObjectRecord>(&Record.Data); ObjectRecord && (!SnapshotRecord || SnapshotRecord == ObjectRecord))
					ObjectRecords.push_back(ObjectRecord);
			auto Depth = [](const auto* Record) { size_t Result = 0; for (auto* Object = Record->GetTarget().Resolve(); Object && Result < 256; Object = Object->GetOuter()) ++Result; return Result; };
			std::stable_sort(ObjectRecords.begin(), ObjectRecords.end(), [&](const auto* Left, const auto* Right) { return Depth(Left) > Depth(Right); });
			std::vector<std::pair<FPersistentObjectRef, FTransactionObjectEvent>> Events;
			for (const auto* ObjectRecord : ObjectRecords)
			{
				auto Built = SnapshotRecord ? SnapshotRecord->Snapshot(GetContext(), Selected) : ObjectRecord->BuildObjectEvents(GetContext(), Type, Operation);
				if (!Built) return std::unexpected(Built.error());
				for (auto& Event : *Built) Events.push_back(std::move(Event));
			}
			for (const auto& [Target, Event] : Events)
			{
				auto* Object = Target.Resolve();
				if (!Object) { bObserverRecoveryRequired |= bDispatched; return std::unexpected("Transaction event target became unavailable."); }
				bDispatched = true;
				Object->PostTransacted(Event);
			}
			return {};
		}
		catch (const std::exception& Error) { bObserverRecoveryRequired = true; return std::unexpected(std::format("Transaction object event failed: {}", Error.what())); }
		catch (...) { bObserverRecoveryRequired = true; return std::unexpected("Transaction object event failed with an unknown exception."); }
	}


	auto FTransaction::RemoveNoOpRecords() -> void
	{
		if (bBusy) return;
		std::erase_if(Records, [](const FTransactionRecord& Record) {
			return Record.IsNoOp();
		});
	}

	auto FTransaction::Validate() const -> FTransactionApplyResult
	{
		for (size_t Index = 0; Index < Records.size(); ++Index)
		{
			if (const auto Result = Records[Index].Validate(); !Result)
				return std::unexpected(FTransactionApplyError{.Status = ETransactionApplyFailure::ValidationFailed,
					.RecordIndex = Index, .Message = Result.error()});
		}
		return {};
	}

	auto FTransaction::Apply(bool bUndo, EPropertyChangeOrigin Origin) -> FTransactionApplyResult
	{
		if (bBusy) return std::unexpected(FTransactionApplyError{.Message = "A transaction operation is already in progress."});
		FGarbageCollectionDeferralScope DeferCollection;
		const bool bOwnOperation = OperationId == 0;
		if (bOwnOperation)
			if (const auto Result = BeginOperation(); !Result)
				return std::unexpected(FTransactionApplyError{.Message = Result.error()});
		struct FOperationScope
		{
			FTransaction& Transaction;
			bool bOwn;
			~FOperationScope() { if (bOwn) (void)Transaction.EndOperation(); }
		} OperationScope{*this, bOwnOperation};
		FTransactionBusyScope BusyScope(bBusy);
		if (auto Result = Validate(); !Result) return Result;
		std::vector<size_t> Applied;
		Applied.reserve(Records.size());
		std::vector<FPersistentObjectRef> ReplayTargets;
		if (Origin != EPropertyChangeOrigin::Edit)
		{
			for (const FTransactionRecord& Record : Records)
			{
				for (DObject* Object : Record.GetModifiedObjects())
				{
					if (!Object) continue;
					const FPersistentObjectRef Target(Object);
					if (!std::ranges::contains(ReplayTargets, Target)) ReplayTargets.push_back(Target);
				}
			}
		}
		size_t StartedTargets = 0;
		auto NotifyReplayTargets = [&]() -> std::string {
			std::string Failure;
			auto CompletionTargets = std::vector<FPersistentObjectRef>(ReplayTargets.begin(), ReplayTargets.begin() + StartedTargets);
			auto Depth = [](const FPersistentObjectRef& Target) {
				size_t Result = 0;
				for (auto* Object = Target.Resolve(); Object && Result < 256; Object = Object->GetOuter()) ++Result;
				return Result;
			};
			// Complete inners before their owners so Actor reconstruction observes
			// settled component transforms and attachment reverse links.
			std::stable_sort(CompletionTargets.begin(), CompletionTargets.end(), [&](const auto& Left, const auto& Right) { return Depth(Left) > Depth(Right); });
			for (const auto& Target : CompletionTargets)
				if (DObject* Object = Target.Resolve())
					try
					{
						std::optional<std::shared_ptr<ITransactionObjectAnnotation>> Annotation;
						for (const auto& Record : Records)
							if (auto Value = Record.GetReplayAnnotation(Object)) Annotation = std::move(Value);
						if (Annotation) Object->PostEditUndo(std::move(*Annotation));
						else Object->PostEditUndo();
					}
					catch (const std::exception& Error) { Failure += std::format(" PostEditUndo threw: {}", Error.what()); }
					catch (...) { Failure += " PostEditUndo threw an unknown exception."; }
			return Failure;
		};
		bool bRetainRecoveryState = false;
		struct FPreparedReplayScope
		{
			std::vector<FTransactionRecord>& Records;
			bool& bRetain;
			~FPreparedReplayScope() { if (!bRetain) for (auto& Record : Records) Record.ClearPreparedReplay(); }
		} PreparedReplayScope{Records, bRetainRecoveryState};
		for (const auto& Target : ReplayTargets)
		{
			++StartedTargets;
			std::string Failure;
			if (auto* Object = Target.Resolve())
				try { Object->PreEditUndo(); }
				catch (const std::exception& Error) { Failure = std::format("PreEditUndo threw: {}", Error.what()); }
				catch (...) { Failure = "PreEditUndo threw an unknown exception."; }
			if (!Failure.empty())
			{
				Failure += NotifyReplayTargets(); bRetainRecoveryState = true;
				return std::unexpected(FTransactionApplyError{.Status = ETransactionApplyFailure::RecoveryRequired,
					.Message = std::move(Failure)});
			}
		}
		// Save all inverses before any custom command or object load can mutate a peer.
		for (size_t Index = 0; Index < Records.size(); ++Index)
			if (auto Prepared = Records[Index].PrepareReplay(); !Prepared)
			{
				auto Failure = NotifyReplayTargets(); bRetainRecoveryState = !Failure.empty();
				return std::unexpected(FTransactionApplyError{.Status = bRetainRecoveryState
					? ETransactionApplyFailure::RecoveryRequired : ETransactionApplyFailure::ValidationFailed,
					.RecordIndex = Index, .Message = Prepared.error() + Failure});
			}
		for (size_t Step = 0; Step < Records.size(); ++Step)
		{
			const size_t Index = bUndo ? Records.size() - 1 - Step : Step;
			bool bRecordRecoveryRequired = false;
			if (const auto Application = Records[Index].Apply(bUndo, Origin, &bRecordRecoveryRequired); !Application)
			{
				FTransactionApplyError Error{.Status = ETransactionApplyFailure::Restored,
					.RecordIndex = Index, .Message = Application.error()};
				for (auto It = Applied.rbegin(); It != Applied.rend(); ++It)
				{
					// Compensation restores the inverse without recapturing the historical
					// side from an intermediate state whose annotations are not completed.
					if (const auto Rollback = Records[*It].Apply(!bUndo, Origin, nullptr, false); !Rollback)
						Error.RollbackFailures.push_back({*It, Rollback.error()});
				}
				if (bRecordRecoveryRequired || !Error.RollbackFailures.empty())
				{ Error.Status = ETransactionApplyFailure::RecoveryRequired; bRetainRecoveryState = true; }
				if (auto Failure = NotifyReplayTargets(); !Failure.empty())
				{ Error.Message += Failure; Error.Status = ETransactionApplyFailure::RecoveryRequired; bRetainRecoveryState = true; }
				return std::unexpected(std::move(Error));
			}
			Applied.push_back(Index);
		}
		if (auto Failure = NotifyReplayTargets(); !Failure.empty())
		{
			bRetainRecoveryState = true;
			return std::unexpected(FTransactionApplyError{.Status = ETransactionApplyFailure::RecoveryRequired,
				.Message = std::move(Failure)});
		}
		if (Origin != EPropertyChangeOrigin::Edit)
			if (auto Result = DispatchObjectEvents(ETransactionObjectEventType::UndoRedo, bUndo ? ETransactionOperation::Undo : ETransactionOperation::Redo); !Result)
			{
				bRetainRecoveryState = true;
				return std::unexpected(FTransactionApplyError{.Status = ETransactionApplyFailure::RecoveryRequired, .Message = Result.error()});
			}
		return {};
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

	auto FTransaction::GetTransactionObjects() const -> std::vector<DObject*>
	{
		std::vector<DObject*> Objects;
		for (const auto& Record : Records)
			for (auto* Object : Record.GetModifiedObjects())
				if (Object && !std::ranges::contains(Objects, Object)) Objects.push_back(Object);
		return Objects;
	}

	auto FTransaction::GetAffectedPackages() const -> std::vector<DPackage*>
	{
		std::vector<DPackage*> Packages;
		for (const FTransactionRecord& Record : Records)
		{
			for (DObject* Object : Record.GetModifiedObjects())
			{
				if (!Object) continue;
				DPackage* Package = Object->GetPackage();
				if (Package && !std::ranges::contains(Packages, Package))
					Packages.push_back(Package);
			}
			for (DPackage* Package : Record.GetAffectedPackages())
				if (Package && !std::ranges::contains(Packages, Package))
					Packages.push_back(Package);
		}
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
		// Lifetime dependencies include snapshot values and custom participants,
		// even when the transaction does not modify their package.
		class FPackageReferenceCollector final : public FReferenceCollector
		{
		public:
			explicit FPackageReferenceCollector(const DPackage& InPackage) : Package(InPackage) {}
			auto AddReferencedObject(DObject*& Object) -> void override
			{
				if (Object && (Object == &Package || Object->GetPackage() == &Package))
					bFound = true;
			}
			const DPackage& Package;
			bool bFound = false;
		};
		FPackageReferenceCollector Collector(Package);
		AddReferencedObjects(Collector);
		return Collector.bFound;
	}

	auto FTransaction::AddReferencedObjects(FReferenceCollector& Collector) const -> void
	{
		PrimaryObject.AddReferencedObjects(Collector);
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

	auto ResolveEditorTransactor(ETransactionMode Mode) -> std::expected<DTransactor*, std::string>
	{
		if (!IsInGameThread()) return std::unexpected("Editor edits require the game thread.");
		if (Mode == ETransactionMode::NotTransactable)
		{
			if (GUndo) return std::unexpected("Unrecorded edits cannot join an active transaction.");
			return nullptr;
		}
		if (!GEditor || !GEditor->CanTransact())
			return std::unexpected("The active editor transaction service is unavailable or busy.");
		return GEditor->GetTransactor();
	}

	FScopedTransaction::FScopedTransaction(std::string_view Description, bool bShouldActuallyTransact)
		: FScopedTransaction(GEditor && GEditor->CanTransact() ? GEditor->GetTransactor() : nullptr,
			{.Description = std::string(Description)}, bShouldActuallyTransact) {}
	FScopedTransaction::FScopedTransaction(std::string_view Context, std::string_view Description, DObject* PrimaryObject, bool bShouldActuallyTransact)
		: FScopedTransaction(GEditor && GEditor->CanTransact() ? GEditor->GetTransactor() : nullptr,
			{std::string(Context), std::string(Description), PrimaryObject}, bShouldActuallyTransact) {}
	FScopedTransaction::FScopedTransaction(DTransactor* InTransactor, FTransactionContext Context, bool bShouldActuallyTransact)
	{
		if (bShouldActuallyTransact && InTransactor)
		{
			StartIndex = InTransactor->Begin(Context.Name, Context.Description);
			if (StartIndex >= 0)
			{
				Transactor = InTransactor;
				RecordingId = InTransactor->GetRecordingId();
				InTransactor->SetPrimaryUndoObject(Context.PrimaryObject);
			}
		}
	}
	FScopedTransaction::~FScopedTransaction() { (void)End(); }
	FScopedTransaction::FScopedTransaction(FScopedTransaction&& Other) noexcept
		: Transactor(std::move(Other.Transactor)), RecordingId(std::exchange(Other.RecordingId, 0)), StartIndex(std::exchange(Other.StartIndex, -1)) {}
	auto FScopedTransaction::operator=(FScopedTransaction&& Other) noexcept -> FScopedTransaction&
	{
		if (this != &Other)
		{
			(void)End(); Transactor = std::move(Other.Transactor);
			RecordingId = std::exchange(Other.RecordingId, 0);
			StartIndex = std::exchange(Other.StartIndex, -1);
		}
		return *this;
	}
	auto FScopedTransaction::IsOutstanding() const -> bool
	{
		return StartIndex >= 0 && Transactor && RecordingId && Transactor->GetRecordingId() == RecordingId;
	}
	auto FScopedTransaction::End() -> int32
	{
		const bool Valid = IsOutstanding();
		StartIndex = -1; RecordingId = 0;
		auto Owner = std::move(Transactor);
		return Valid ? Owner->End() : 0;
	}
	auto FScopedTransaction::Cancel() -> void
	{
		const bool Valid = IsOutstanding();
		const auto Index = std::exchange(StartIndex, -1); RecordingId = 0;
		auto Owner = std::move(Transactor);
		if (Valid) Owner->Cancel(Index);
	}

}

namespace Durin
{
	using namespace Editor;

	DTransactor::DTransactor(const FObjectInitializer& ObjectInitializer)
		: DObject(ObjectInitializer)
	{
	}


	auto DTransactor::AddTransactionListener(FTransactionListener Listener) -> uint64
	{ if (!Listener) return 0; const auto Id = NextListener++; Listeners.emplace(Id, std::move(Listener)); return Id; }
	auto DTransactor::RemoveTransactionListener(uint64 Handle) -> void { Listeners.erase(Handle); }

	DTransBuffer::DTransBuffer(const FObjectInitializer& ObjectInitializer)
		: DTransactor(ObjectInitializer)
	{
	}

	auto DTransBuffer::CheckThread() const -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
	}

	auto DTransBuffer::Reject(ETransactorRejectionReason Reason, uint64 RequestedId,
		std::string_view ModuleName) const -> FTransactorResult
	{
		FTransactorRejection Rejection{.Reason = Reason, .State = State,
			.RequestedId = RequestedId, .PendingId = Pending ? Pending->GetId() : 0,
			.UndoHead = GetUndoId(), .RedoHead = GetRedoId(),
			.HistoryCount = History.size(), .OwnedBytes = OwnedBytes,
			.ModuleName = std::string(ModuleName)};
		return {.Code = ETransactorResultCode::Rejected,
			.RejectionCause = std::move(Rejection)};
	}

	auto DTransBuffer::Begin(std::string_view Context, std::string_view Description) -> int32
	{
		CheckThread();
		if (!CanTransact() || (GUndo && (!Pending || GUndo != &*Pending)))
		{ LastResult = Reject(ETransactorRejectionReason::BeginState); return -1; }
		const int32 Index = ActiveCount++;
		if (Index == 0)
		{
			check(HistoryRevision != std::numeric_limits<uint64>::max());
			++HistoryRevision;
			RefreshOutsideEdits();
			Pending.emplace(NextTransactionId++, FTransactionContext{std::string(Context), std::string(Description)}, this);
			if (const auto Operation = Pending->BeginOperation(); !Operation)
			{
				Pending.reset(); ActiveCount = 0;
				LastResult = {.Code = ETransactorResultCode::Failed,
					.ApplyCause = std::make_shared<FTransactionApplyError>(FTransactionApplyError{.Message = Operation.error()})};
				return -1;
			}
			GUndo = &*Pending;
			State = ETransactorState::Recording;
		}
		LastResult = {.Code = ETransactorResultCode::Succeeded, .TransactionId = Pending->GetId()};
		return Index;
	}
	auto DTransBuffer::ReleaseRecording() -> void
	{
		if (Pending && Pending->GetContext().OperationId != 0) (void)Pending->EndOperation();
		if (Pending && GUndo == &*Pending) GUndo = nullptr;
		ActiveCount = 0;
	}
	auto DTransBuffer::SetPrimaryUndoObject(DObject* Object) -> void
	{ if (Pending) Pending->SetPrimaryObject(Object); }
	auto DTransBuffer::CaptureObject(DObject* Object) -> bool
	{
		CheckThread();
		if (State != ETransactorState::Recording || !Pending || Pending->IsBusy() || !IsValid(Object)) return false;
		if (auto* Package = Object->GetPackage(); Package && Package->IsAssetPackage()) EnsurePackageState(*Package);
		if (auto Result = Pending->CaptureObject(Object); !Result)
		{
			LastResult = {.Code = ETransactorResultCode::Failed, .TransactionId = Pending->GetId(),
				.ApplyCause = std::make_shared<FTransactionApplyError>(FTransactionApplyError{.Message = Result.error()})};
			DURIN_ERROR("Unable to capture transaction object: {}", Result.error());
			return false;
		}
		LastResult = {.Code = ETransactorResultCode::Succeeded, .TransactionId = Pending->GetId()};
		return true;
	}
	auto DTransBuffer::Record(FTransactionObjectRecord Record) -> std::expected<uint64, FTransactorResult>
	{
		CheckThread();
		if (State != ETransactorState::Recording || !Pending || Pending->IsBusy()) return std::unexpected(Reject(ETransactorRejectionReason::RecordScope));
		if (auto* Object = Record.GetTarget().Resolve())
			if (auto* Package = Object->GetPackage(); Package && Package->IsAssetPackage()) EnsurePackageState(*Package);
		const auto Id = Pending->AddRecord(std::move(Record));
		return Id;
	}
	auto DTransBuffer::ReportChangeFailure(std::unique_ptr<FCommandChange> Change, FCommandChangeError Error) -> FTransactorResult
	{
		if (!Pending) return LastResult = FTransactorResult{.Code = ETransactorResultCode::Failed};
		if (Error.bRecoveryRequired && Change) Pending->AddRecord(std::move(Change));
		PreparePackageTransitions(*Pending);
		const auto Failure = HandleApplyFailure(*Pending, {.Status = Error.bRecoveryRequired
			? ETransactionApplyFailure::RecoveryRequired : ETransactionApplyFailure::Restored, .Message = std::move(Error.Message)});
		if (!Error.bRecoveryRequired) State = ETransactorState::Recording;
		return LastResult = Failure;
	}

	auto DTransBuffer::StoreUndo(DObject* Object, std::unique_ptr<FCommandChange> Change) -> FTransactorResult
	{
		CheckThread();
		if (!CanTransact()) return LastResult = Reject(ETransactorRejectionReason::ExecuteState);
		if (!Change) return LastResult = Reject(ETransactorRejectionReason::MissingCustom);
		const bool bStandalone = !IsActive();
		if (bStandalone)
		{
			// These already-applied writes are represented by the incoming change.
			auto Acknowledge = [&](DPackage* Package) {
				if (Package) if (auto* Tracked = FindPackageState(*Package))
					Tracked->ObservedEditRevision = Package->GetEditRevision();
			};
			if (Object) Acknowledge(Object->GetPackage());
			for (DPackage* Package : Change->GetAffectedPackages()) Acknowledge(Package);
		}
		if (bStandalone && Begin("CustomChange", Change->GetDescription()) < 0) return LastResult;
		for (DPackage* Package : Change->GetAffectedPackages())
			if (IsValid(Package) && Package->IsAssetPackage()) EnsurePackageState(*Package);
		Pending->AddRecord(std::move(Change), Object);
		LastResult = {.Code = ETransactorResultCode::Succeeded, .TransactionId = Pending->GetId()};
		if (bStandalone) (void)End();
		return LastResult;
	}
	auto DTransBuffer::End() -> int32
	{
		CheckThread();
		const int32 Result = ActiveCount;
		if (ActiveCount == 0) return 0;
		if (State != ETransactorState::Recording || !Pending || Pending->IsBusy()) return 0;
		if (--ActiveCount > 0) return Result;
		if (auto Captured = Pending->Finalize(); !Captured)
		{
			ReleaseRecording();
			if (Pending->bObserverRecoveryRequired)
			{
				LastResult = HandleApplyFailure(*Pending, {.Status = ETransactionApplyFailure::RecoveryRequired, .Message = Captured.error()});
				return Result;
			}
			LastResult = {.Code = ETransactorResultCode::Failed, .TransactionId = Pending->GetId(),
				.ApplyCause = std::make_shared<FTransactionApplyError>(FTransactionApplyError{.Message = Captured.error()})};
			QueueEvent(ETransactionEventType::Failed, *Pending, Captured.error());
			Pending.reset(); State = ETransactorState::Idle;
			RefreshOutsideEdits();
		}
		else { ReleaseRecording(); LastResult = FinalizePending(); }
		return Result;
	}
	auto DTransBuffer::Cancel(int32 StartIndex) -> void
	{
		CheckThread();
		(void)StartIndex; // UE 5.8 cancels the entire recording, including inner Cancel.
		if (State != ETransactorState::Recording || !Pending || Pending->IsBusy()) return;
		LastResult = {.Code = ETransactorResultCode::Discarded, .TransactionId = Pending->GetId()};
		QueueEvent(ETransactionEventType::Discarded, *Pending, "Transaction recording canceled; applied edits remain.");
		ReleaseRecording(); Pending.reset(); State = ETransactorState::Idle;
		RefreshOutsideEdits();
	}

	auto DTransBuffer::FinalizePending() -> FTransactorResult
	{
		const FTransactionId TransactionId = Pending->GetId();
		Pending->RemoveNoOpRecords();
		if (Pending->GetRecordCount() == 0)
		{
			QueueEvent(ETransactionEventType::Discarded, *Pending, "The transaction contained no records.");
			Pending.reset();
			State = ETransactorState::Idle;
			RefreshOutsideEdits();
			return {.Code = ETransactorResultCode::NoOp, .TransactionId = TransactionId};
		}
		PreparePackageTransitions(*Pending);
		auto RejectFinalization = [&](FTransactorFailure Cause) -> FTransactorResult
		{
			FTransactorResult Result{.Code = ETransactorResultCode::Failed,
				.TransactionId = TransactionId, .FailureCause = std::move(Cause)};

			for (DPackage* Package : Pending->GetAffectedPackages())
				if (IsValid(Package) && Package->IsAssetPackage()) InvalidateSavedState(*Package);
			QueueEvent(ETransactionEventType::Failed, *Pending, FormatTransactorResult(Result));
			Pending.reset();
			State = ETransactorState::Idle;
			RefreshOutsideEdits();
			return Result;
		};
		size_t EntryBytes = 0;
		if (!Pending->TryGetOwnedSize(EntryBytes))
			return RejectFinalization({.Code = ETransactorFailure::EntryAccounting});
		// Validate retained accounting before replacing the redo branch.
		size_t RetainedBytes = 0;
		for (size_t Index = 0; Index < Cursor; ++Index)
		{
			size_t Bytes = 0;
			if (!History[Index].TryGetOwnedSize(Bytes) || RetainedBytes > std::numeric_limits<size_t>::max() - Bytes)
				return RejectFinalization({.Code = ETransactorFailure::RetainedAccounting, .Actual = RetainedBytes});
			RetainedBytes += Bytes;
		}
		if (RetainedBytes > std::numeric_limits<size_t>::max() - EntryBytes)
			return RejectFinalization({.Code = ETransactorFailure::RetainedAccounting, .Actual = RetainedBytes});
		if (Cursor < History.size())
		{
			for (size_t Index = Cursor; Index < History.size(); ++Index)
				QueueEvent(ETransactionEventType::Discarded, History[Index], "A new commit replaced the redo branch.");
			History.erase(History.begin() + Cursor, History.end());
		}
		else
		{
			OwnedBytes = RetainedBytes;
			TrimHistoryToBudget();
		}
		Pending->RecordingOwner = nullptr;
		History.push_back(std::move(*Pending));
		Pending.reset();
		Cursor = History.size();
		ApplyPackageTransitions(History.back(), true);
		if (History.back().MutatesMountedContent()) NotifyMountedContentMutation();
		QueueEvent(ETransactionEventType::Executed, History.back());
		RecalculateOwnedBytes();
		State = ETransactorState::Idle;
		return {.Code = ETransactorResultCode::Succeeded, .TransactionId = TransactionId};
	}

	auto DTransBuffer::Undo(bool bCanRedo) -> bool
	{
		LastResult = UndoExpected(0);
		if (LastResult.IsSuccess() && !bCanRedo)
		{
			for (size_t Index = Cursor; Index < History.size(); ++Index) QueueEvent(ETransactionEventType::Discarded, History[Index], "Undo without Redo.");
			History.erase(History.begin() + Cursor, History.end());
			(void)RecalculateOwnedBytes();
		}
		return LastResult.IsSuccess();
	}

	auto DTransBuffer::UndoExpected(FTransactionId ExpectedId) -> FTransactorResult
	{
		CheckThread();
		if (State != ETransactorState::Idle || GUndo) return Reject(ETransactorRejectionReason::UndoState, ExpectedId);
		if (Cursor == 0) return {.Code = ETransactorResultCode::NoOp, .Notice = ETransactorNotice::NothingUndo};
		RefreshOutsideEdits();
		FTransaction& Transaction = History[Cursor - 1];
		if (ExpectedId != 0 && Transaction.GetId() != ExpectedId)
			return Reject(ETransactorRejectionReason::UndoHead, ExpectedId);
		check(HistoryRevision != std::numeric_limits<uint64>::max());
		++HistoryRevision;
		State = ETransactorState::Undoing;
		auto ApplyResult = Transaction.Apply(true, EPropertyChangeOrigin::Undo);
		if (!ApplyResult)
		{
			return HandleApplyFailure(Transaction, std::move(ApplyResult.error()));
		}
		if (!RecalculateOwnedBytes())
			return HandleApplyFailure(Transaction, {.Status = ETransactionApplyFailure::RecoveryRequired,
				.Message = "Transaction replay made retained byte accounting invalid."});
		ApplyPackageTransitions(Transaction, false);
		if (Transaction.MutatesMountedContent()) NotifyMountedContentMutation();
		--Cursor;
		QueueEvent(ETransactionEventType::Undone, Transaction);
		State = ETransactorState::Idle;
		return {.Code = ETransactorResultCode::Succeeded,
			.TransactionId = Transaction.GetId()};
	}

	auto DTransBuffer::Redo() -> bool { LastResult = RedoExpected(0); return LastResult.IsSuccess(); }

	auto DTransBuffer::RedoExpected(FTransactionId ExpectedId) -> FTransactorResult
	{
		CheckThread();
		if (State != ETransactorState::Idle || GUndo) return Reject(ETransactorRejectionReason::RedoState, ExpectedId);
		if (Cursor >= History.size())
			return {.Code = ETransactorResultCode::NoOp, .Notice = ETransactorNotice::NothingRedo};
		RefreshOutsideEdits();
		FTransaction& Transaction = History[Cursor];
		if (ExpectedId != 0 && Transaction.GetId() != ExpectedId)
			return Reject(ETransactorRejectionReason::RedoHead, ExpectedId);
		check(HistoryRevision != std::numeric_limits<uint64>::max());
		++HistoryRevision;
		State = ETransactorState::Redoing;
		auto ApplyResult = Transaction.Apply(false, EPropertyChangeOrigin::Redo);
		if (!ApplyResult)
		{
			return HandleApplyFailure(Transaction, std::move(ApplyResult.error()));
		}
		if (!RecalculateOwnedBytes())
			return HandleApplyFailure(Transaction, {.Status = ETransactionApplyFailure::RecoveryRequired,
				.Message = "Transaction replay made retained byte accounting invalid."});
		ApplyPackageTransitions(Transaction, true);
		if (Transaction.MutatesMountedContent()) NotifyMountedContentMutation();
		++Cursor;
		QueueEvent(ETransactionEventType::Redone, Transaction);
		State = ETransactorState::Idle;
		return {.Code = ETransactorResultCode::Succeeded,
			.TransactionId = Transaction.GetId()};
	}

	auto DTransBuffer::HandleApplyFailure(const FTransaction& Transaction, FTransactionApplyError Error) -> FTransactorResult
	{
		const auto Operation = State == ETransactorState::Undoing ? ETransactionOperation::Undo
			: State == ETransactorState::Redoing ? ETransactionOperation::Redo : ETransactionOperation::Execute;
		if (!RecalculateOwnedBytes())
		{
			Error.Status = ETransactionApplyFailure::RecoveryRequired;
			Error.Message += " Retained transaction accounting overflowed.";
		}
		const bool bRecoveryRequired = Error.Status == ETransactionApplyFailure::RecoveryRequired;
		State = bRecoveryRequired ? ETransactorState::RecoveryRequired : ETransactorState::Idle;
		FTransactorResult Failure{.Code = bRecoveryRequired ? ETransactorResultCode::RecoveryRequired : ETransactorResultCode::Failed,
			.TransactionId = Transaction.GetId(),
			.ApplyCause = std::make_shared<FTransactionApplyError>(std::move(Error))};
		const std::string Message = FormatTransactorResult(Failure);
		if (bRecoveryRequired)
		{
			ReleaseRecording();
			for (const auto& Transition : Transaction.GetPackageTransitions())
				if (auto* Package = Cast<DPackage>(Transition.Package.Resolve())) InvalidateSavedState(*Package);
			DURIN_ERROR("Transaction {} requires recovery: {}", Transaction.GetId(), Message);
		}
		QueueEvent(ETransactionEventType::Failed, Transaction, Message);
		Events.back().Operation = Operation;
		return Failure;
	}

	auto DTransBuffer::Reset(std::string_view Reason) -> FTransactorResult
	{
		CheckThread();
		if (!CanTransact()) return LastResult = Reject(ETransactorRejectionReason::ResetState);
		Cancel(); History.clear(); Cursor = 0; OwnedBytes = 0;
		Events.clear(); PackageStates.clear(); ResetReason = Reason;
		return LastResult = FTransactorResult{.Code = ETransactorResultCode::Succeeded};
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
		if (ModuleName.empty()) return Reject(ETransactorRejectionReason::ModuleName, 0, ModuleName);
		if (State != ETransactorState::Idle)
			return Reject(ETransactorRejectionReason::ModuleState, 0, ModuleName);
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
				.FailureCause = FTransactorFailure{.Code = ETransactorFailure::ModuleAccounting, .Module = std::string(ModuleName)}};
		}
		return {.Code = Removed == 0 ? ETransactorResultCode::NoOp
			: ETransactorResultCode::Succeeded,
			.Notice = ETransactorNotice::ModuleDrain, .NoticeCount = Removed};
	}

	auto DTransBuffer::DispatchEvents() -> void
	{
		CheckThread();
		if (State != ETransactorState::Idle && State != ETransactorState::RecoveryRequired) return;
		const auto PendingEvents = std::exchange(Events, {});
		for (const auto& Event : PendingEvents)
		{
			const auto Snapshot = Listeners;
			for (const auto& [Id, Listener] : Snapshot)
				if (Listeners.contains(Id)) Listener(Event);
		}
	}

	auto DTransBuffer::CanUndo(std::string* Reason) const -> bool
	{
		if (Reason) *Reason = State != ETransactorState::Idle ? "A transaction is active." : Cursor == 0 ? (ResetReason.empty() ? "Nothing to undo." : ResetReason) : "";
		return State == ETransactorState::Idle && Cursor != 0;
	}

	auto DTransBuffer::CanRedo(std::string* Reason) const -> bool
	{
		if (Reason) *Reason = State != ETransactorState::Idle ? "A transaction is active." : Cursor >= History.size() ? "Nothing to redo." : "";
		return State == ETransactorState::Idle && Cursor < History.size();
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
		ReleaseRecording();
		Pending.reset();
		History.clear();
		Events.clear();
		Cursor = 0;
		OwnedBytes = 0;
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

	auto DTransBuffer::GetUndoContext(bool bCheckWhetherUndoPossible) const -> FTransactionContext
	{
		if (Cursor == 0 || (bCheckWhetherUndoPossible && !CanUndo())) return {};
		return History[Cursor - 1].GetContext();
	}

	auto DTransBuffer::GetRedoContext() const -> FTransactionContext
	{
		return CanRedo() ? History[Cursor].GetContext() : FTransactionContext{};
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
			.ObservedEditRevision = Package.GetEditRevision(),
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

	auto DTransBuffer::RefreshOutsideEdits() -> void
	{
		for (auto& [Package, State] : PackageStates)
		{
			if (State.Package.Resolve() != Package || State.ObservedEditRevision == Package->GetEditRevision()) continue;
			State.CurrentRevision = AllocateRevision();
			State.SavedRevision = 0;
			State.bCheckpointValid = false;
			State.ObservedEditRevision = Package->GetEditRevision();
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
		State.ObservedEditRevision = Package->GetEditRevision();
	}

	auto DTransBuffer::GetMountedContentMutationRevision() const -> uint64
	{
		return MountedContentMutationRevision;
	}

	auto DTransBuffer::NotifyMountedContentMutation() -> void
	{
		NotifyMountedContentMutation({.bFullRefresh = true});
	}

	auto DTransBuffer::CaptureMountedContentChanges(uint64 FromRevision) const -> FContentChangeBatch
	{
		check(IsInGameThread());
		return MountedContentChanges.Read(FromRevision, MountedContentMutationRevision);
	}

	auto DTransBuffer::NotifyMountedContentMutation(FContentChangeBatch Changes) -> void
	{
		check(IsInGameThread());
		check(MountedContentMutationRevision != std::numeric_limits<uint64>::max());
		Changes.FromRevision = MountedContentMutationRevision;
		Changes.ToRevision = MountedContentMutationRevision + 1;
		MountedContentChanges.Append(std::move(Changes));
		++MountedContentMutationRevision;
	}

	auto DTransBuffer::EstablishSavedState(DPackage& Package) -> void
	{
		std::vector<DObject*> Objects{&Package};
		for (size_t Index = 0; Index < Objects.size(); ++Index)
		{
			Objects[Index]->SetFlags(EObjectFlags::Transactional);
			for (auto* Child : GDObjectArray.GetObjectsWithOuter(Objects[Index], EObjectQueryScope::LiveOnly)) Objects.push_back(Child);
		}

		ForgetPackage(Package);
		const FRevisionId Revision = AllocateRevision();
		PackageStates.emplace(&Package, FTrackedPackageState{
			.Package = FPersistentObjectRef(&Package),
			.CurrentRevision = Revision,
			.SavedRevision = Revision,
			.bCheckpointValid = true,
			.ObservedEditRevision = Package.GetEditRevision(),
		});
		Package.ClearDirty();
	}

	auto DTransBuffer::MarkSaved(DPackage& Package) -> void
	{
		RefreshOutsideEdits();
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

	class FTransactorReloadParticipant final : public IPackageReloadParticipant
	{
		struct FSourceRecord
		{
			std::optional<Editor::FTransactionObjectRecord> Object;
			const FCommandChange* Command = nullptr;
			FPersistentObjectRef CustomObject;
			std::vector<FPersistentObjectRef> Modified;
		};
		struct FSourceTransaction
		{
			Editor::FTransactionId Id = 0;
			FPersistentObjectRef Primary;
			std::vector<Editor::FTransactionPackageRevisionTransition> Transitions;
			std::vector<FSourceRecord> Records;
		};
		struct FPreparedTransaction
		{
			FPreparedTransaction(Editor::FTransaction Value, size_t Count) : Transaction(std::move(Value)), Records(Count) {}
			Editor::FTransaction Transaction;
			std::vector<std::optional<Editor::FTransactionRecord>> Records;
		};
		class FReferences final : public FReferenceCollector
		{
		public:
			auto AddReferencedObject(DObject*& Object) -> void override { if (Object) Keys.push_back(FObjectKey(Object)); }
			std::vector<FObjectKey> Keys;
		};
	public:
		FTransactorReloadParticipant(DTransBuffer& InTransactor, std::span<DPackage* const> InPackages)
			: Transactor(InTransactor), Packages(InPackages.begin(), InPackages.end()) {}

		auto Prepare(const FObjectReplacementMap& InMap) -> std::expected<void, FObjectReplacementError> override
		{
			Abort();
			if (Transactor.State != Editor::ETransactorState::Idle || Transactor.Pending || Transactor.ActiveCount)
				return std::unexpected(FObjectReplacementError{.Code = EObjectReplacementError::Busy, .Reason = EObjectReplacementReason::ParticipantBusy});
			Map = &InMap;
			for (auto* Package : Packages)
			{
				const auto* Entry = Map->Find(Package);
				if (!Entry || !Entry->Replacement)
					return std::unexpected(FObjectReplacementError{.Code = EObjectReplacementError::UnmappedReference,
						.Reason = EObjectReplacementReason::ParticipantUnmappedPackage});
			}
			// Candidate-only editor admission; no live authored fields are loaded.
			for (auto* Object : Map->GetPreparedObjects()) Object->SetFlags(EObjectFlags::Transactional);
			BeforeHistoryRevision = Transactor.HistoryRevision;
			if (BeforeHistoryRevision == std::numeric_limits<uint64>::max())
				return std::unexpected(FObjectReplacementError{.Code = EObjectReplacementError::BudgetExceeded});
			BeforeCursor = Transactor.Cursor;
			BeforeBytes = Transactor.OwnedBytes;
			BeforeRevision = Transactor.NextRevision;
			BeforeBudget = Transactor.MemoryBudget;
			BeforePackages = Transactor.PackageStates;
			BeforeEvents = Transactor.Events;
			AfterEvents = BeforeEvents;
			FReferences References;
			Transactor.AddReferencedObjects(References);
			BeforeReferences = std::move(References.Keys);
			AfterHistory.reserve(Transactor.History.size());
			Replacements.resize(Transactor.History.size());
			Keep.assign(Transactor.History.size(), true);
			std::vector<DPackage*> Invalidated;
			for (size_t Index = 0; Index < Transactor.History.size(); ++Index)
			{
				const auto& Transaction = Transactor.History[Index];
				FSourceTransaction Source{Transaction.Id, Transaction.PrimaryObject, Transaction.PackageTransitions, {}};
				for (const auto& Record : Transaction.Records)
				{
					FSourceRecord Snapshot;
					if (const auto* Object = std::get_if<Editor::FTransactionObjectRecord>(&Record.Data)) Snapshot.Object = *Object;
					else Snapshot.Command = std::get<std::unique_ptr<FCommandChange>>(Record.Data).get();
					Snapshot.CustomObject = Record.CustomObject;
					Snapshot.Modified.assign(Record.ModifiedObjects.GetReferences().begin(), Record.ModifiedObjects.GetReferences().end());
					Source.Records.push_back(std::move(Snapshot));
				}
				Sources.push_back(std::move(Source));
				const bool Affected = Map->Find(Transaction.PrimaryObject.Resolve())
					|| std::ranges::any_of(Transaction.PackageTransitions, [&](const auto& Transition) { return Map->Find(Transition.Package.Resolve()) != nullptr; })
					|| std::ranges::any_of(Transaction.Records, [&](const auto& Record) { return Record.HasReplacementReferences(*Map); });
				if (Affected)
				{
					auto Replacement = PrepareTransaction(Transaction, *Map);
					if (Replacement) Replacements[Index] = std::move(*Replacement);
					else
					{
						Keep[Index] = false;
						AfterEvents.push_back({.Type = Editor::ETransactionEventType::Discarded, .Id = Transaction.Id,
							.Description = Transaction.Context.Description, .Details = Replacement.error()});
						for (auto* Package : Transaction.GetAffectedPackages())
							if (Package && !Map->Find(Package) && !std::ranges::contains(Invalidated, Package)) Invalidated.push_back(Package);
					}
				}
				if (Keep[Index] && Index < BeforeCursor) ++AfterCursor;
			}
			AfterRevision = BeforeRevision;
			for (const auto& [Package, State] : BeforePackages)
			{
				auto Key = Map->RemapObjectKey(State.Package.GetKey());
				if (!Key) return std::unexpected(Key.error());
				auto* ReplacementPackage = Cast<DPackage>(ResolveObjectKey(*Key));
				if (!ReplacementPackage) return std::unexpected(FObjectReplacementError{.Code = EObjectReplacementError::IncompatibleType});
				auto NewState = State;
				NewState.Package = FPersistentObjectRef(ReplacementPackage);
				if (ReplacementPackage != Package)
				{
					if (!AfterRevision || AfterRevision == std::numeric_limits<Editor::FRevisionId>::max())
						return std::unexpected(FObjectReplacementError{.Code = EObjectReplacementError::BudgetExceeded});
					const auto Revision = AfterRevision++;
					NewState.CurrentRevision = Revision;
					NewState.SavedRevision = ReplacementPackage->IsDirty() ? 0 : Revision;
					NewState.bCheckpointValid = !ReplacementPackage->IsDirty();
					NewState.ObservedEditRevision = ReplacementPackage->GetEditRevision();
					for (auto& Transaction : Replacements) if (Transaction)
						for (auto& Transition : Transaction->Transaction.PackageTransitions)
							if (Transition.Package.Resolve() == ReplacementPackage)
							{
								if (Transition.BeforeRevision == State.CurrentRevision) Transition.BeforeRevision = Revision;
								if (Transition.AfterRevision == State.CurrentRevision) Transition.AfterRevision = Revision;
								Transition.InitialSavedRevision = NewState.SavedRevision;
								Transition.bInitialCheckpointValid = NewState.bCheckpointValid;
							}
				}
				else if (std::ranges::contains(Invalidated, Package))
				{
					NewState.SavedRevision = 0;
					NewState.bCheckpointValid = false;
					DirtyPackages.push_back(Package);
				}
				if (!AfterPackages.emplace(ReplacementPackage, NewState).second)
					return std::unexpected(FObjectReplacementError{.Code = EObjectReplacementError::MapCollision});
			}
			// A newly tracked reload package also needs a saved baseline, without
			// a post-publication EstablishSavedState that would erase retained history.
			for (const auto& Entry : Map->GetEntries())
				if (Cast<DPackage>(Entry.Previous))
					if (auto* Package = Cast<DPackage>(Entry.Replacement); Package && !AfterPackages.contains(Package))
					{
						if (!AfterRevision || AfterRevision == std::numeric_limits<Editor::FRevisionId>::max())
							return std::unexpected(FObjectReplacementError{.Code = EObjectReplacementError::BudgetExceeded});
						const auto Revision = AfterRevision++;
						AfterPackages.emplace(Package, DTransBuffer::FTrackedPackageState{
							.Package = FPersistentObjectRef(Package), .CurrentRevision = Revision,
							.SavedRevision = Package->IsDirty() ? 0 : Revision, .bCheckpointValid = !Package->IsDirty(),
							.ObservedEditRevision = Package->GetEditRevision()});
					}
			for (size_t Index = 0; Index < Keep.size(); ++Index) if (Keep[Index])
			{
				size_t Bytes = 0;
				const auto& Transaction = Replacements[Index] ? Replacements[Index]->Transaction : Transactor.History[Index];
				if (!Transaction.TryGetOwnedSize(Bytes))
					return std::unexpected(FObjectReplacementError{.Code = EObjectReplacementError::BudgetExceeded});
				if (Replacements[Index])
					for (size_t RecordIndex = 0; RecordIndex < Replacements[Index]->Records.size(); ++RecordIndex)
					{
						const auto& Prepared = Replacements[Index]->Records[RecordIndex];
						const auto& Record = Prepared ? *Prepared : Transactor.History[Index].Records[RecordIndex];
						size_t RecordBytes = 0;
						if (!Record.TryGetAllocatedSize(RecordBytes) || RecordBytes > std::numeric_limits<size_t>::max() - Bytes)
							return std::unexpected(FObjectReplacementError{.Code = EObjectReplacementError::BudgetExceeded});
						Bytes += RecordBytes;
					}
				if (Bytes > std::numeric_limits<size_t>::max() - AfterBytes)
					return std::unexpected(FObjectReplacementError{.Code = EObjectReplacementError::BudgetExceeded});
				AfterBytes += Bytes;
			}
			bPrepared = true;
			return {};
		}

		auto Validate() const -> bool override
		{
			if (!bPrepared || Transactor.State != Editor::ETransactorState::Idle || Transactor.Pending || Transactor.ActiveCount
				|| Transactor.HistoryRevision != BeforeHistoryRevision || Transactor.History.size() != Sources.size() || Transactor.Cursor != BeforeCursor
				|| Transactor.OwnedBytes != BeforeBytes || Transactor.NextRevision != BeforeRevision || Transactor.MemoryBudget != BeforeBudget
				|| Transactor.PackageStates != BeforePackages || Transactor.Events != BeforeEvents) return false;
			FReferences Edges;
			Transactor.AddReferencedObjects(Edges);
			if (Edges.Keys != BeforeReferences) return false;
			for (size_t Index = 0; Index < Sources.size(); ++Index)
			{
				const auto& Source = Sources[Index];
				const auto& Transaction = Transactor.History[Index];
				if (Transaction.bBusy || Transaction.Id != Source.Id || Transaction.PrimaryObject != Source.Primary
					|| Transaction.PackageTransitions != Source.Transitions || Transaction.Records.size() != Source.Records.size()) return false;
				for (size_t RecordIndex = 0; RecordIndex < Source.Records.size(); ++RecordIndex)
				{
					const auto& Before = Source.Records[RecordIndex];
					const auto& Current = Transaction.Records[RecordIndex];
					if (Current.CustomObject != Before.CustomObject || !std::ranges::equal(Current.ModifiedObjects.GetReferences(), Before.Modified)) return false;
					if (Before.Object)
					{
						const auto* Object = std::get_if<Editor::FTransactionObjectRecord>(&Current.Data);
						if (!Object || !Object->HasSameState(*Before.Object)) return false;
					}
					else if (const auto* Command = std::get_if<std::unique_ptr<FCommandChange>>(&Current.Data); !Command || Command->get() != Before.Command) return false;
				}
				if (Replacements[Index])
					for (size_t RecordIndex = 0; RecordIndex < Replacements[Index]->Records.size(); ++RecordIndex)
					{
						const auto& Prepared = Replacements[Index]->Records[RecordIndex];
						const auto& Record = Prepared ? *Prepared : Transaction.Records[RecordIndex];
						if (!Record.Validate() || Record.HasReplacementReferences(*Map)) return false;
					}
			}
			return true;
		}
		auto CoversNativeReferences(const DObject& Owner) const -> bool override { return bPrepared && &Owner == &Transactor; }
		auto GetStrongReferenceCount(const DObject&) const -> uint32 override { return 0; }
		auto Commit() noexcept -> void override
		{
			for (size_t Index = 0; Index < Keep.size(); ++Index) if (Keep[Index])
			{
				if (Replacements[Index])
				{
					auto& Prepared = *Replacements[Index];
					for (size_t RecordIndex = 0; RecordIndex < Prepared.Records.size(); ++RecordIndex)
						Prepared.Transaction.Records.push_back(Prepared.Records[RecordIndex]
							? std::move(*Prepared.Records[RecordIndex]) : std::move(Transactor.History[Index].Records[RecordIndex]));
					AfterHistory.push_back(std::move(Prepared.Transaction));
				}
				else AfterHistory.push_back(std::move(Transactor.History[Index]));
			}
			Transactor.History.swap(AfterHistory);
			++Transactor.HistoryRevision;
			Transactor.PackageStates.swap(AfterPackages);
			Transactor.Events.swap(AfterEvents);
			Transactor.Cursor = AfterCursor;
			Transactor.OwnedBytes = AfterBytes;
			Transactor.NextRevision = AfterRevision;
			for (auto* Package : DirtyPackages)
				if (auto* State = Transactor.FindPackageState(*Package)) Transactor.SynchronizeDirtyState(*State);
			bCommitted = true;
			// Native destructors run during retirement while old objects are
			// still pinned, never inside the non-failing publication.
		}
		auto Abort() noexcept -> void override
		{
			bPrepared = false; bCommitted = false; Map = nullptr;
			Sources.clear(); Replacements.clear(); Keep.clear(); AfterHistory.clear();
			BeforePackages.clear(); AfterPackages.clear(); BeforeEvents.clear(); AfterEvents.clear();
			BeforeReferences.clear(); DirtyPackages.clear(); AfterCursor = 0; AfterBytes = 0;
		}
		auto CanRetire() const -> bool override
		{
			if (bCommitted)
			{
				FGarbageCollectionDeferralScope DeferCollection;
				Sources.clear();
				AfterHistory.clear();
			}
			return true;
		}
	private:
		auto PrepareTransaction(const Editor::FTransaction& Source, const FObjectReplacementMap& Mapping)
			-> std::expected<FPreparedTransaction, std::string>
		{
			if (Source.bBusy || Source.RecordingOwner) return std::unexpected("Only settled history can be reinstanced.");
			Editor::FTransaction Transaction(Source.Id, Source.Context);
			Transaction.bFinalized = Source.bFinalized;
			Transaction.OperationId = Source.OperationId;
			Transaction.bObserverRecoveryRequired = Source.bObserverRecoveryRequired;
			auto Primary = Mapping.RemapObjectKey(Source.PrimaryObject.GetKey());
			if (!Primary) return std::unexpected(ToString(Primary.error()));
			Transaction.PrimaryObject = FPersistentObjectRef::FromKey(*Primary);
			Transaction.PackageTransitions = Source.PackageTransitions;
			for (auto& Transition : Transaction.PackageTransitions)
			{
				auto Key = Mapping.RemapObjectKey(Transition.Package.GetKey());
				if (!Key) return std::unexpected(ToString(Key.error()));
				Transition.Package = FPersistentObjectRef::FromKey(*Key);
			}
			Transaction.Records.reserve(Source.Records.size());
			FPreparedTransaction Result(std::move(Transaction), Source.Records.size());
			for (size_t Index = 0; Index < Source.Records.size(); ++Index)
			{
				const auto& Record = Source.Records[Index];
				if (Record.HasReplacementReferences(Mapping))
				{
					auto Replacement = Record.CreateReplacement(Mapping);
					if (!Replacement) return std::unexpected(Replacement.error());
					Result.Records[Index] = std::move(*Replacement);
				}
				const auto& Prepared = Result.Records[Index] ? *Result.Records[Index] : Record;
				if (auto Valid = Prepared.Validate(); !Valid) return std::unexpected(Valid.error());
				if (Prepared.HasReplacementReferences(Mapping))
					return std::unexpected("Prepared history still contains old-generation replacement references.");
			}
			return Result;
		}
		DTransBuffer& Transactor;
		std::vector<DPackage*> Packages;
		const FObjectReplacementMap* Map = nullptr;
		mutable std::vector<FSourceTransaction> Sources;
		std::vector<std::optional<FPreparedTransaction>> Replacements;
		std::vector<bool> Keep;
		mutable std::vector<Editor::FTransaction> AfterHistory;
		decltype(DTransBuffer::PackageStates) BeforePackages, AfterPackages;
		std::vector<Editor::FTransactionEvent> BeforeEvents, AfterEvents;
		std::vector<FObjectKey> BeforeReferences;
		std::vector<DPackage*> DirtyPackages;
		size_t BeforeCursor = 0, AfterCursor = 0, BeforeBytes = 0, AfterBytes = 0, BeforeBudget = 0;
		Editor::FRevisionId BeforeRevision = 0, AfterRevision = 0;
		uint64 BeforeHistoryRevision = 0;
		bool bPrepared = false, bCommitted = false;
	};

	auto CreateTransactorReloadParticipant(DTransactor& Transactor,
		std::span<DPackage* const> Packages)
		-> std::shared_ptr<IPackageReloadParticipant>
	{
		auto* Buffer = Cast<DTransBuffer>(&Transactor);
		if (!Buffer) return {};
		return std::make_shared<FTransactorReloadParticipant>(*Buffer, Packages);
	}

	auto DTransBuffer::TrimHistoryToBudget() -> void
	{
		// Only the old history participates; the incoming transaction may exceed the budget.
		size_t RemovalCount = 0;
		while (RemovalCount < History.size() && OwnedBytes >= MemoryBudget)
		{
			const auto& Transaction = History[RemovalCount];
			size_t EntryBytes = 0;
			verify(Transaction.TryGetOwnedSize(EntryBytes));
			check(EntryBytes <= OwnedBytes);
			OwnedBytes -= EntryBytes;
			Events.push_back({.Type = ETransactionEventType::Evicted,
				.Operation = ETransactionOperation::Execute, .Id = Transaction.GetId(),
				.Description = Transaction.GetContext().Description});
			++RemovalCount;
		}
		History.erase(History.begin(), History.begin() + RemovalCount);
		Cursor -= RemovalCount;
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
