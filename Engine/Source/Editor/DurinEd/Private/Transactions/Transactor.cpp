#include "Transactions/Transactor.h"

#include "Editor/EditorEngine.h"
#include "Asset/PackageRemoval.h"
#include "Components/ActorComponent.h"

#include "PropertyEditor/PropertyEditing.h"
#include "DObject/DObjectArray.h"
#include "DObject/Class.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "DObject/Property.h"
#include "DObject/ObjectGraphReplacement.h"
#include "CoreGlobals.h"
#include "Logging/LogMacros.h"
#include "Templates/GuardValue.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace { thread_local TWeakObjectPtr<DEditorEngine> TransactionEditor; thread_local bool bTransactionEditorBound = false; }
	FScopedEditorTransactionContext::FScopedEditorTransactionContext(DEditorEngine* Editor)
		: Previous(TransactionEditor), bPreviousBound(bTransactionEditorBound)
	{ TransactionEditor = Editor; bTransactionEditorBound = true; }
	FScopedEditorTransactionContext::~FScopedEditorTransactionContext()
	{ TransactionEditor = Previous; bTransactionEditorBound = bPreviousBound; }
	auto GetTransactionEditor() -> DEditorEngine* { return bTransactionEditorBound ? TransactionEditor.Get() : GEditor; }
	namespace
	{
		[[noreturn]] void TransactionFault(FTransactionId Id, const char* Phase, std::string_view Reason)
		{
			Private::ReportFormattedAssertionFailure("Transaction execution fault", std::source_location::current(),
				"Transaction {} during {}: {}", Id, Phase, Reason);
		}

		[[noreturn]] void TransactionRecordFault(FTransactionId Id, const char* Phase, size_t Index, std::string_view Reason)
		{
			Private::ReportFormattedAssertionFailure("Transaction execution fault", std::source_location::current(),
				"Transaction {} during {} record {}: {}", Id, Phase, Index, Reason);
		}

		class FTransactionPackageSaveCounts
		{
		public:
			FTransactionPackageSaveCounts()
			{
				OnPackageSaved().AddRaw(this, &FTransactionPackageSaveCounts::Advance);
				OnPackageDeleted().AddRaw(this, &FTransactionPackageSaveCounts::Advance);
			}

			~FTransactionPackageSaveCounts()
			{
				OnPackageSaved().RemoveAll(this);
				OnPackageDeleted().RemoveAll(this);
			}

			auto Get(const DPackage& Package) -> uint64
			{
				const uint64 Serial = GetLastGarbageCollectionStats().CollectionSerial;
				if (Serial != LastCollection)
				{
					std::erase_if(Counts, [](const auto& Entry) { return !Entry.first.ResolveObjectPtr(); });
					LastCollection = Serial;
				}
				return Counts[FObjectKey(&Package)];
			}

		private:
			auto Advance(DPackage& Package) -> void
			{
				// Saves before the first transaction capture cannot fence a retained record.
				if (auto It = Counts.find(FObjectKey(&Package)); It != Counts.end()) ++It->second;
			}

			std::unordered_map<FObjectKey, uint64> Counts;
			uint64 LastCollection = 0;
		};

		auto GetTransactionPackageSaveCount(const DPackage& Package) -> uint64
		{
			static FTransactionPackageSaveCounts Counts;
			return Counts.Get(Package);
		}

		auto FailWithReason(std::string* OutError, std::string_view Message) -> bool
		{
			if (OutError) *OutError = Message;
			return false;
		}
		auto AddSize(size_t& Total, size_t Value) -> bool
		{
			if (Total > std::numeric_limits<size_t>::max() - Value) return false;
			Total += Value;
			return true;
		}
		class FPieCollector final : public FReferenceCollector
		{
		public:
			auto AddReferencedObject(DObject*& Object) -> void override
			{
				auto* Package = Cast<DPackage>(Object);
				if (!Package && Object) Package = Object->GetPackage();
				bFound |= Package && Package->IsPlayInEditor();
			}
			bool bFound = false;
		};
		template<typename TSource>
		auto InspectPieReferences(const TSource& Source) -> std::expected<bool, std::string>
		{
			try
			{
				FPieCollector Collector;
				Source.AddReferencedObjects(Collector);
				return Collector.bFound;
			}
			catch (const std::exception& Error) { return std::unexpected(std::format("PIE inspection failed: {}", Error.what())); }
			catch (...) { return std::unexpected("PIE inspection failed."); }
		}
	}

	FTransaction::FObjectRecord::FObjectRecord(DObject& Target, std::unique_ptr<FCommandChange> Change)
		: Data(FCommandRecord{FPersistentObjectRef::FromKey(FObjectKey(&Target)), std::move(Change), {}})
	{
		auto& Custom = std::get<FCommandRecord>(Data);
		if (Custom.Change)
			for (DPackage* Package : Custom.Change->GetAffectedPackages())
				Custom.DeclaredPackages.Add(FPersistentObjectRef::FromKey(FObjectKey(Package)));
	}

	auto FTransaction::FObjectRecord::HasReplacementReferences(const FObjectReplacementMap& Map) const -> bool
	{
		if (const auto* Record = std::get_if<FTransaction::FWholeObjectData>(&Data)) return Record->HasReplacementReferences(Map);
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

	auto FTransaction::FObjectRecord::Reinstance(const FObjectReplacementMap& Map) -> FTransactionCaptureResult
	{
		if (auto* Object = std::get_if<FWholeObjectData>(&Data)) return Object->Reinstance(Map);
		auto& Custom = std::get<FCommandRecord>(Data);
		auto& Command = Custom.Change;
		if (!Command) return std::unexpected("A missing custom command cannot be reinstanced.");
		FReinstancingReferenceCollector Collector(Map);
		Custom.Target.AddReferencedObjects(Collector);
		Custom.DeclaredPackages.AddReferencedObjects(Collector);
		Command->AddReferencedObjects(Collector);
		for (auto* Package : Command->GetAffectedPackages())
			if (Map.Find(Package)) return std::unexpected("Command retained a replaced package.");
		if (HasReplacementReferences(Map)) return std::unexpected("Command retained a replaced reference.");
		return {};
	}

	auto FTransaction::FObjectRecord::GetTarget() const -> const FPersistentObjectRef&
	{
		if (const auto* Whole = std::get_if<FWholeObjectData>(&Data)) return Whole->GetTarget();
		return std::get<FCommandRecord>(Data).Target;
	}

	auto FTransaction::FObjectRecord::HasExpired() const noexcept -> bool
	{
		if (const auto* Whole = std::get_if<FWholeObjectData>(&Data)) return Whole->HasExpired();
		const auto& Custom = std::get<FCommandRecord>(Data);
		const auto* Target = Custom.Target.Get();
		return Custom.Change && Custom.Change->HasExpired(Target);
	}

	auto FTransaction::FObjectRecord::Validate() const -> FTransactionCaptureResult
	{
		if (const auto* Whole = std::get_if<FWholeObjectData>(&Data)) return Whole->Validate();
		const auto& Custom = std::get<FCommandRecord>(Data);
		if (!Custom.Change) return std::unexpected("The custom transaction change is unavailable.");
		if (!Custom.Target.Get()) return std::unexpected("The custom transaction target is unavailable.");
		return {};
	}

	auto FTransaction::FObjectRecord::CaptureInverse() -> void
	{
		if (auto* Object = std::get_if<FTransaction::FWholeObjectData>(&Data))
			Object->CaptureInverse();
	}
	auto FTransaction::FObjectRecord::IsNoOp() const -> bool
	{
		if (const auto* Record = std::get_if<FTransaction::FWholeObjectData>(&Data))
			return Record->IsNoOp();
		return false;
	}
	auto FTransaction::FObjectRecord::GetReplayAnnotation() const -> std::shared_ptr<ITransactionObjectAnnotation>
	{
		if (const auto* Object = std::get_if<FTransaction::FWholeObjectData>(&Data))
			return Object->GetReplayAnnotation();
		return {};
	}

	auto FTransaction::FObjectRecord::Apply(bool bBefore, EPropertyChangeOrigin Origin) -> void
	{
		std::visit([&](auto& Record) {
			using T = std::decay_t<decltype(Record)>;
			if constexpr (std::is_same_v<T, FCommandRecord>)
			{
				if (!Record.Change) throw std::runtime_error("The custom transaction change is unavailable.");
				auto* Target = Record.Target.Get();
				if (!Target) throw std::runtime_error("The custom transaction target is unavailable.");
				if (bBefore) Record.Change->Revert(Target);
				else Record.Change->Apply(Target);
			}
			else Record.Apply(Origin);
		}, Data);
	}

	auto FTransaction::FObjectRecord::AddReferencedObjects(FReferenceCollector& Collector) const -> void
	{
		std::visit([&](const auto& Record) {
			using T = std::decay_t<decltype(Record)>;
			if constexpr (std::is_same_v<T, FCommandRecord>)
			{
				Record.Target.AddReferencedObjects(Collector);
				Record.DeclaredPackages.AddReferencedObjects(Collector);
				if (Record.Change) Record.Change->AddReferencedObjects(Collector);
			}
			else Record.AddReferencedObjects(Collector);
		}, Data);
	}

	auto FTransaction::FObjectRecord::TryGetAllocatedSize(size_t& OutBytes) const -> bool
	{
		return std::visit([&](const auto& Record) {
			using T = std::decay_t<decltype(Record)>;
			if constexpr (std::is_same_v<T, FCommandRecord>)
			{
				if (!Record.Change) return false;
				const size_t Allocated = Record.Change->GetAllocatedSize();
				if (Allocated > std::numeric_limits<size_t>::max()
					- sizeof(FCommandChange)) return false;
				OutBytes = sizeof(FCommandChange) + Allocated;
				if (!AddSize(OutBytes, Record.Target.GetAllocatedSize())) return false;
				size_t PackageBytes = 0;
				if (!Record.DeclaredPackages.TryGetAllocatedSize(PackageBytes) || !AddSize(OutBytes, PackageBytes)) return false;
				return true;
			}
			else return Record.TryGetAllocatedSize(OutBytes);
		}, Data);
	}

	auto FTransaction::FObjectRecord::GetDetails(bool bReplay) const -> std::string
	{
		if (const auto* Custom = std::get_if<FCommandRecord>(&Data); Custom && Custom->Change)
		{
			if (!bReplay) return Custom->Change->GetDetails();
			try { return Custom->Change->GetDetails(); }
			catch (const std::exception& Error) { DURIN_WARN("Transaction replay details unavailable: {}", Error.what()); }
			catch (...) { DURIN_WARN("Transaction replay details unavailable."); }
		}
		return {};
	}

	auto FTransaction::FObjectRecord::GetAffectedPackages() const -> std::vector<DPackage*>
	{
		if (const auto* Object = std::get_if<FTransaction::FWholeObjectData>(&Data)) return Object->GetAffectedPackages();
		std::vector<DPackage*> Packages;
		const auto& Custom = std::get<FCommandRecord>(Data);
		if (auto* Object = Custom.Target.Get())
			if (auto* Package = Object->GetPackage()) Packages.push_back(Package);
		for (const auto& Reference : Custom.DeclaredPackages.GetReferences())
			if (auto* Package = Cast<DPackage>(Reference.Get()); Package && !std::ranges::contains(Packages, Package)) Packages.push_back(Package);
		return Packages;
	}

	auto FTransaction::FObjectRecord::IsOwnedByModule(std::string_view ModuleName) const -> bool
	{
		if (const auto* Custom = std::get_if<FCommandRecord>(&Data); Custom && Custom->Change)
			return Custom->Change->GetOwningModule() == ModuleName;
		return false;
	}

	FTransaction::FTransaction(FTransactionId InId, FTransactionContext InContext, DTransBuffer* InRecordingOwner)
		: RecordingOwner(InRecordingOwner), Id(InId)
		, Context(std::move(InContext))
	{
		if (RecordingOwner) Context.HistoryId = RecordingOwner->GetHistoryId();
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
		if (bBusy || (RecordingOwner && !RecordingOwner->CanTransact()))
			return std::unexpected("A transaction callback or capture is in progress.");
		if (OperationId != 0) return std::unexpected("A transaction operation is already active.");
		OperationId = AllocateTransactionOperationId();
		if (OperationId == 0) return std::unexpected("Transaction operation identities are exhausted.");
		return {};
	}
	auto FTransaction::EndOperation() -> FTransactionCaptureResult
	{
		if (bBusy)
			return std::unexpected("A transaction callback or capture is in progress.");
		if (OperationId == 0) return std::unexpected("No transaction operation is active.");
		OperationId = 0;
		return {};
	}
	auto FTransaction::StoreUndo(DObject* Target, std::unique_ptr<FCommandChange> Change) -> FTransactionCaptureResult
	{
		if (bBusy || bFinalized) return std::unexpected("Transaction records are closed to capture.");
		if (!IsValid(Target)) return std::unexpected("The custom transaction target is unavailable.");
		if (!Change) return std::unexpected("The custom transaction change is unavailable.");
		if (RecordingOwner)
		{
			if (GUndo != this) return std::unexpected("The transaction is not the active recording.");
			std::string Error;
			if (!RecordingOwner->StoreUndo(Target, std::move(Change), &Error)) return std::unexpected(std::move(Error));
		}
		else AddRecord(*Target, std::move(Change));
		return {};
	}
	auto FTransaction::ContainsPieObjects() const -> std::expected<bool, std::string>
	{
		if (bBusy) return std::unexpected("Transaction references are already being inspected or changed.");
		if (GIsGameThreadIdInitialized) CheckGameThread();
		const TGuardValue BusyScope(bBusy, true);
		return InspectPieReferences(*this);
	}

	auto FTransaction::IndexRecord(FObjectIndex& Index, const FTransaction::FObjectRecord& Record) -> void
	{
		auto& bHasSnapshot = Index[Record.GetTarget()];
		bHasSnapshot |= std::holds_alternative<FWholeObjectData>(Record.Data);
	}

	auto FTransaction::GetCaptureIndex() -> FObjectIndex&
	{
		if (!CaptureIndex)
		{
			auto Index = std::make_unique<FObjectIndex>();
			for (const auto& Record : Records)
				IndexRecord(*Index, Record);
			CaptureIndex = std::move(Index);
		}
		return *CaptureIndex;
	}

	auto FTransaction::AppendRecord(FTransaction::FObjectRecord&& Record) -> size_t
	{
		auto& Index = GetCaptureIndex();
		try
		{
			IndexRecord(Index, Record);
			Records.push_back(std::move(Record));
		}
		catch (...)
		{
			// Records remains authoritative if indexing participants or growing the array fails.
			CaptureIndex.reset();
			throw;
		}
		return Records.size();
	}

	auto FTransaction::AddRecord(DObject& Target, std::unique_ptr<FCommandChange> Change) -> uint64
	{
		if (bBusy || bFinalized) return 0;
		const TGuardValue BusyScope(bBusy, true);
		return AppendRecord(FTransaction::FObjectRecord(Target, std::move(Change)));
	}

	auto FTransaction::ContainsObject(const DObject* Object, bool bWholeObjectOnly) const -> bool
	{
		if (!IsValid(Object)) return false;
		if (CaptureIndex)
		{
			const auto Direct = CaptureIndex->find(FPersistentObjectRef::FromKey(FObjectKey(Object)));
			if (Direct != CaptureIndex->end() && (!bWholeObjectOnly || Direct->second)) return true;
			const auto It = CaptureIndex->find(FPersistentObjectRef(const_cast<DObject*>(Object)));
			return It != CaptureIndex->end() && (!bWholeObjectOnly || It->second) && It->first.Resolve() == Object;
		}
		return std::ranges::any_of(Records, [&](const auto& Record) { return Record.GetTarget().Get() == Object && (!bWholeObjectOnly || std::holds_alternative<FWholeObjectData>(Record.Data)); });
	}

	auto FTransaction::SaveObject(DObject* Object) -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (bBusy || bFinalized || (RecordingOwner && (GUndo != this || !RecordingOwner->CanTransact())))
			return;
		if (FailureReason) return;
		if (!IsValid(Object)) { FailureReason = "Transaction object is unavailable for capture."; return; }
		auto* Package = Object->GetPackage();
		const TGuardValue BusyScope(bBusy, true);
		SavePackage(Package);
		if (ContainsObject(Object)) return;
		auto Record = FTransaction::FWholeObjectData::CaptureWhole(*Object);
		if (!Record) { FailureReason = Record.error(); return; }
		AppendRecord(FTransaction::FObjectRecord(std::move(*Record)));
	}

	auto FTransaction::Finalize() -> void try
	{
		if (FailureReason) TransactionFault(Id, "Finalize", *FailureReason);
		if (bBusy && !bFinalized) return;
		bFinalized = true;
		if (GIsGameThreadIdInitialized) CheckGameThread();
		const TGuardValue BusyScope(bBusy, true);
		std::vector<DObject*> Deleted;
		for (const auto& Record : Records)
		{
			const auto* Whole = std::get_if<FTransaction::FWholeObjectData>(&Record.Data);
			if (!Whole || Whole->GetTarget().GetReferenceType() != FPersistentObjectRef::EReferenceType::RootObject) continue;
			auto* Object = GDObjectArray.Resolve(Whole->GetTarget().GetKey());
			if (Object && Object->IsGarbage() && !Object->IsTemplateObject()) Deleted.push_back(Object);
		}
		const FScopedTransactionHistoryReplay Access(FScopedTransactionHistoryReplay::GetHistoryId(), Deleted);
		std::vector<FChangedObject> ChangedObjects;
		std::unordered_set<DObject*> Seen;
		for (size_t Index = 0; Index < Records.size(); ++Index)
		{
			auto& Record = Records[Index];
			std::shared_ptr<ITransactionObjectAnnotation> Annotation;
			if (auto* Whole = std::get_if<FWholeObjectData>(&Record.Data))
			{
				const bool bCapture = !Whole->bFinalized;
				try { Whole->CaptureAfter(); }
				catch (const std::exception& Error) { TransactionRecordFault(Id, "Finalize", Index, Error.what()); }
				catch (...) { TransactionRecordFault(Id, "Finalize", Index, "Final capture threw an unknown exception."); }
				if (bCapture) Annotation = Whole->SerializedObjectFlip->Annotation;
			}
			if (auto* Object = Record.GetTarget().Get(); Object && Seen.insert(Object).second)
				ChangedObjects.push_back({FPersistentObjectRef(Object), &Record, Annotation});
		}
		NotifyRecords(ChangedObjects, ETransactionObjectEventType::Finalized);
		CaptureIndex.reset();
	}
	catch (const std::exception& Error) { TransactionFault(Id, "Finalize", Error.what()); }
	catch (...) { TransactionFault(Id, "Finalize", "unknown exception"); }

	auto FTransaction::NotifyRecords(std::vector<FChangedObject>& ChangedObjects, ETransactionObjectEventType Type) -> void
	{
		std::stable_partition(ChangedObjects.begin(), ChangedObjects.end(), [](const auto& Entry) {
			return Cast<DActorComponent>(Entry.Target.Get()) != nullptr;
		});
		for (const auto& [Target, Record, Annotation] : ChangedObjects)
		{
			auto* Object = Target.Get();
			if (!Object) continue;
			if (Type == ETransactionObjectEventType::UndoRedo)
			{
				if (Annotation)
					Object->PostEditUndo(Annotation);
				else Object->PostEditUndo();
			}
			if (const auto* Whole = std::get_if<FWholeObjectData>(&Record->Data))
				if (auto Sent = Whole->NotifyObject(GetContext(), Type, Annotation); !Sent)
					throw std::runtime_error(Sent.error());
		}
	}


	auto FTransaction::RemoveNoOpRecords() -> void
	{
		if (bBusy) return;
		// UE IsTransient tests the complete transaction. An unchanged SaveObject
		// still owns its baseline when another record changes the same domain.
		if (std::ranges::all_of(Records, [](const auto& Record) { return Record.IsNoOp(); }))
		{
			Records.clear();
			CaptureIndex.reset();
		}
	}

	auto FTransaction::HasExpired() const noexcept -> bool
	{
		return FailureReason.has_value() || std::ranges::any_of(Records, &FObjectRecord::HasExpired);
	}

	auto FTransaction::Validate() const -> std::expected<void, std::string>
	{
		FReplayResult Replay;
		return PrepareReplay(bUndoNext, Replay);
	}

	auto FTransaction::Apply() -> void
	{
		const bool bUndo = bUndoNext;
		const char* Phase = bUndo ? "Undo" : "Redo";
		if (RecordingOwner || bBusy) TransactionFault(Id, Phase, "Cannot replay an active transaction.");
		const bool bOwnOperation = OperationId == 0;
		if (bOwnOperation)
			if (auto Result = BeginOperation(); !Result) TransactionFault(Id, Phase, Result.error());
		FReplayResult Replay;
		if (auto Valid = PrepareReplay(bUndo, Replay); !Valid) TransactionFault(Id, Phase, Valid.error());
		ApplyPrepared(bUndo, Replay);
		if (bOwnOperation)
			if (auto Result = EndOperation(); !Result) TransactionFault(Id, Phase, Result.error());
	}

	auto FTransaction::PrepareReplay(bool bUndo, FReplayResult& Replay, bool bCheckExpiry) const -> FTransactionCaptureResult try
	{
		if (FailureReason) return std::unexpected(*FailureReason);
		if (bBusy) return std::unexpected("A transaction operation is already in progress.");
		if (GIsGameThreadIdInitialized) CheckGameThread();
		const TGuardValue BusyScope(bBusy, true);
		if (bCheckExpiry && HasExpired()) return std::unexpected("The transaction has expired.");
		std::vector<DObject*> LifecycleTargets;
		for (size_t Index = 0; Index < Records.size(); ++Index)
		{
			const auto* Whole = std::get_if<FTransaction::FWholeObjectData>(&Records[Index].Data);
			if (!Whole || !Whole->SerializedObject || !Whole->SerializedObjectFlip) continue;
			const bool Desired = Whole->SerializedObject->Observation.ObjectInfo.bIsPendingKill;
			const bool Source = Whole->SerializedObjectFlip->Observation.ObjectInfo.bIsPendingKill;
			if (Desired == Source) continue;
			auto* Object = GDObjectArray.Resolve(Whole->GetTarget().GetKey());
			if (!Object || Object->HasAnyInternalFlags(EObjectInternalFlags::BeginDestroyed | EObjectInternalFlags::FinishDestroyed)) continue;
			if (Object->IsTemplateObject()
				|| Object->IsGarbage() != Source)
				return std::unexpected(std::format("record {}: The recorded object lifecycle no longer matches the replay source.", Index));
			LifecycleTargets.push_back(Object);
			Replay.LifecycleSources.emplace_back(FObjectKey(Object), Object->IsGarbage());
		}
		const FScopedTransactionHistoryReplay ReplayHistory(Context.HistoryId, LifecycleTargets);
		constexpr auto PieRejection = "Transaction replay cannot retain Play-in-Editor references.";
		if (auto Pie = InspectPieReferences(*this); !Pie)
			TransactionFault(Id, bUndo ? "Undo" : "Redo", Pie.error());
		else if (*Pie)
			return std::unexpected(PieRejection);
		for (size_t Index = 0; Index < Records.size(); ++Index)
			if (auto Valid = Records[Index].Validate(); !Valid)
				return std::unexpected(std::format("record {}: {}", Index, Valid.error()));
		for (size_t Index = 0; Index < Records.size(); ++Index)
		{
			if (const auto* Whole = std::get_if<FWholeObjectData>(&Records[Index].Data))
				if (auto Valid = Whole->ValidateSource(); !Valid)
					return std::unexpected(std::format("record {}: {}", Index, Valid.error()));
		}
		return {};
	}
	catch (const std::exception& Error) { TransactionFault(Id, bUndo ? "Undo" : "Redo", Error.what()); }
	catch (...) { TransactionFault(Id, bUndo ? "Undo" : "Redo", "unknown exception"); }

	auto FTransaction::ApplyPrepared(bool bUndo, FReplayResult& Replay) -> void try
	{
		const TGuardValue BusyScope(bBusy, true);
		const auto Origin = bUndo ? EPropertyChangeOrigin::Undo : EPropertyChangeOrigin::Redo;
		std::vector<DObject*> LifecycleTargets;
		for (const auto& [Key, bWasGarbage] : Replay.LifecycleSources)
		{
			auto* Object = GDObjectArray.Resolve(Key);
			if (!Object || Object->IsGarbage() != bWasGarbage)
				TransactionFault(Id, bUndo ? "Undo" : "Redo", "Object lifecycle changed after replay admission.");
			LifecycleTargets.push_back(Object);
		}
		const FScopedTransactionHistoryReplay ReplayHistory(Context.HistoryId, LifecycleTargets);
		const auto ReplayOrder = std::views::iota(size_t{0}, Records.size())
			| std::views::transform([&](size_t Step) { return bUndo ? Records.size() - 1 - Step : Step; });
		auto ExecuteRecord = [&](size_t Index, auto&& Execute) {
			try { Execute(); }
			catch (const std::exception& Error) { TransactionRecordFault(Id, bUndo ? "Undo" : "Redo", Index, Error.what()); }
			catch (...) { TransactionRecordFault(Id, bUndo ? "Undo" : "Redo", Index, "Transaction record replay threw an unknown exception."); }
		};
		std::vector<FChangedObject> ChangedObjects;
		std::unordered_map<DObject*, size_t> Positions;
		for (size_t Index : ReplayOrder)
		{
			auto& Record = Records[Index];
			if (auto* Whole = std::get_if<FWholeObjectData>(&Record.Data); Whole && !Whole->bFinalized)
				ExecuteRecord(Index, [&] { Whole->CaptureAfter(); });
			if (auto* Object = Record.GetTarget().Get())
			{
				const auto [It, bFirst] = Positions.try_emplace(Object, ChangedObjects.size());
				if (bFirst)
				{
					ChangedObjects.push_back({FPersistentObjectRef(Object), &Record, {}});
					Object->PreEditUndo();
				}
				else ChangedObjects[It->second].Record = &Record;
			}
		}
		for (size_t Index : ReplayOrder)
			ExecuteRecord(Index, [&] { Records[Index].CaptureInverse(); });
		const auto Pie = InspectPieReferences(*this);
		if (!Pie) TransactionFault(Id, bUndo ? "Undo" : "Redo", Pie.error());
		if (*Pie) TransactionFault(Id, bUndo ? "Undo" : "Redo", "Transaction replay cannot retain Play-in-Editor references.");
		auto SetLiveness = [](DObject& Object, bool bGarbage) -> bool {
			if (Object.IsGarbage() == bGarbage) return true;
			if (bGarbage) Object.MarkAsGarbage();
			else Object.ClearGarbage();
			return Object.IsGarbage() == bGarbage;
		};
		for (size_t Index = 0; Index < LifecycleTargets.size(); ++Index)
		{
			if (!SetLiveness(*LifecycleTargets[Index], !Replay.LifecycleSources[Index].second))
			{
				TransactionFault(Id, bUndo ? "Undo" : "Redo", "Unable to apply the recorded object lifecycle.");
			}
		}
		for (size_t Index : ReplayOrder)
			ExecuteRecord(Index, [&] { Records[Index].Apply(bUndo, Origin); });
		for (auto& Entry : ChangedObjects)
			Entry.Annotation = Entry.Record->GetReplayAnnotation();
		NotifyRecords(ChangedObjects, ETransactionObjectEventType::UndoRedo);
		for (const auto& Record : Records)
		{
			if (auto Details = Record.GetDetails(true); !Details.empty())
			{
				if (!Replay.Details.empty()) Replay.Details += " ";
				Replay.Details += std::move(Details);
			}
		}
		bUndoNext = !bUndo;
	}

	catch (const std::exception& Error) { TransactionFault(Id, bUndo ? "Undo" : "Redo", Error.what()); }
	catch (...) { TransactionFault(Id, bUndo ? "Undo" : "Redo", "unknown exception"); }

	auto FTransaction::GetDetails() const -> std::string
	{
		std::string Details;
		for (const FTransaction::FObjectRecord& Record : Records)
		{
			std::string RecordDetails = Record.GetDetails(false);
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
			if (auto* Object = Record.GetTarget().Get(); Object && !std::ranges::contains(Objects, Object)) Objects.push_back(Object);
		return Objects;
	}

	auto FTransaction::GetAffectedPackages() const -> std::vector<DPackage*>
	{
		std::vector<DPackage*> Packages;
		for (const FTransaction::FObjectRecord& Record : Records)
		{
			if (auto* Object = Record.GetTarget().Get())
				if (auto* Package = Object->GetPackage(); Package && !std::ranges::contains(Packages, Package))
					Packages.push_back(Package);
			for (DPackage* Package : Record.GetAffectedPackages())
				if (Package && !std::ranges::contains(Packages, Package))
					Packages.push_back(Package);
		}
		return Packages;
	}

	auto FTransaction::IsOwnedByModule(std::string_view ModuleName) const -> bool
	{
		return std::ranges::any_of(Records, [&](const FTransaction::FObjectRecord& Record) {
			return Record.IsOwnedByModule(ModuleName);
		});
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
		for (const FTransaction::FObjectRecord& Record : Records)
			Record.AddReferencedObjects(Collector);
		for (const FTransactionPackageRecord& Transition : PackageRecords)
			Transition.Package.AddReferencedObjects(Collector);
	}

	auto FTransaction::TryGetOwnedSize(size_t& OutBytes) const -> bool
	{
		size_t Total = sizeof(FTransaction);
		if (!AddSize(Total, PrimaryObject.GetAllocatedSize()) || !AddSize(Total, Context.Name.capacity())
			|| (FailureReason && !AddSize(Total, FailureReason->capacity()))
			|| !AddSize(Total, Context.Description.capacity())
			|| Records.capacity() > std::numeric_limits<size_t>::max()
				/ sizeof(FTransaction::FObjectRecord)
			|| !AddSize(Total, Records.capacity() * sizeof(FTransaction::FObjectRecord))
			|| PackageRecords.capacity() > std::numeric_limits<size_t>::max()
				/ sizeof(FTransactionPackageRecord)
			|| !AddSize(Total, PackageRecords.capacity()
				* sizeof(FTransactionPackageRecord)))
		{
			return false;
		}
		for (const FTransaction::FObjectRecord& Record : Records)
		{
			size_t RecordBytes = 0;
			if (!Record.TryGetAllocatedSize(RecordBytes) || !AddSize(Total, RecordBytes))
				return false;
		}
		OutBytes = Total;
		return true;
	}

	auto ResolveEditorTransactor(bool bShouldTransact) -> std::expected<DTransactor*, std::string>
	{
		if (!IsInGameThread()) return std::unexpected("Editor edits require the game thread.");
		if (!bShouldTransact && !GUndo) return nullptr;
		if (!GetTransactionEditor() || !GetTransactionEditor()->CanTransact())
			return std::unexpected("The active editor transaction service is unavailable or busy.");
		return GetTransactionEditor()->GetTransactor();
	}

	FScopedTransaction::FScopedTransaction(std::string_view Description, bool bShouldActuallyTransact)
		: FScopedTransaction({}, Description, nullptr, bShouldActuallyTransact) {}
	FScopedTransaction::FScopedTransaction(std::string_view Context, std::string_view Description, DObject* PrimaryObject, bool bShouldActuallyTransact)
	{
		if (bShouldActuallyTransact && GEditor && GetTransactionEditor() == GEditor)
		{
			StartIndex = GEditor->BeginTransaction(Context, Description, PrimaryObject);
		}
	}
	FScopedTransaction::~FScopedTransaction()
	{
		if (!IsOutstanding()) return;
		if (GEditor && GUndo) GEditor->EndTransaction();
	}
	auto FScopedTransaction::Cancel() -> void
	{
		if (!IsOutstanding()) return;
		if (GEditor && GUndo)
		{
			auto* Buffer = GEditor->GetTransactor();
			if (Buffer && Buffer->IsActive() && !Buffer->CanTransact()) return;
			GEditor->CancelTransaction(StartIndex);
		}
		StartIndex = -1;
	}

}

namespace Durin
{

	DTransactor::DTransactor(const FObjectInitializer& ObjectInitializer)
		: DObject(ObjectInitializer)
	{
	}


	DTransBuffer::DTransBuffer(const FObjectInitializer& ObjectInitializer)
		: DTransactor(ObjectInitializer)
	{
	}

	auto DTransBuffer::CheckThread() const -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
	}


	auto DTransBuffer::Begin(std::string_view Context, std::string_view Description) -> int32
	{
		CheckThread();
		if (!CanTransact()) return -1;
		if (ActiveCount > 0) return ActiveCount++;
		auto Candidate = std::make_unique<FTransaction>(NextTransactionId++,
			FTransactionContext{std::string(Context), std::string(Description)}, this);
		if (const auto Operation = Candidate->BeginOperation(); !Operation)
		{
			DURIN_ERROR("Unable to begin transaction: {}", Operation.error());
			return -1;
		}
		size_t Count = 0;
		if (Cursor == History.size() && MemoryBudget == 0) Count = Cursor;
		else if (Cursor == History.size())
		{
			const TGuardValue Inspect(Guard, EHistoryGuard::Inspecting);
			try
			{
				// Keep the newest suffix whose total is strictly below the budget.
				size_t Total = 0;
				for (size_t Index = Cursor; Index > 0; --Index)
				{
					size_t Bytes = 0;
					if (!History[Index - 1]->TryGetOwnedSize(Bytes) || !AddSize(Total, Bytes))
					{
						DURIN_ERROR("Unable to begin transaction: retained transaction byte accounting overflowed.");
						return -1;
					}
					if (!Count && Total >= MemoryBudget) Count = Index;
				}
			}
			catch (const std::exception& Error)
			{
				DURIN_ERROR("Unable to measure history for Begin: {}", Error.what());
				return -1;
			}
			catch (...)
			{
				DURIN_ERROR("Unable to measure history for Begin.");
				return -1;
			}
		}
		BudgetPrefixCount = Count;
		Pending = std::move(Candidate);
		ActiveCount = 1;
		GUndo = Pending.get();
		const auto Id = Pending->GetId();
		try
		{
			OnTransactionStateChanged().Broadcast(Pending->GetContext(), ETransactionStateEventType::TransactionStarted);
		}
		catch (const std::exception& Error) { TransactionFault(Id, "Begin", Error.what()); }
		catch (...) { TransactionFault(Id, "Begin", "unknown exception"); }
		return Pending ? 0 : -1;
	}
	auto DTransBuffer::ReleaseRecording() -> void
	{
		if (Pending && Pending->OperationId != 0) (void)Pending->EndOperation();
		if (Pending) Pending->CaptureIndex.reset();
		if (Pending && GUndo == Pending.get()) GUndo = nullptr;
		ActiveCount = 0;
	}
	auto DTransBuffer::SetPrimaryUndoObject(DObject* Object) -> void
	{ if (CanTransact() && Pending) Pending->SetPrimaryObject(Object); }
	auto DTransBuffer::AppendCommandRecord(FTransaction::FObjectRecord&& Record) -> void
	{
		for (DPackage* Package : Record.GetAffectedPackages())
			Pending->SavePackage(Package);
		Pending->AppendRecord(std::move(Record));
	}

	auto DTransBuffer::StoreUndo(DObject* Target, std::unique_ptr<FCommandChange> Change, std::string* OutError) -> bool
	try
	{
		CheckThread();
		if (!IsActive() || !CanTransact()) return FailWithReason(OutError, "Storing a custom change requires an active recording available for capture.");
		if (!IsValid(Target)) return FailWithReason(OutError, "The custom transaction target is unavailable.");
		if (!Change) return FailWithReason(OutError, "The custom transaction change is unavailable.");
		const TGuardValue BusyScope(Pending->bBusy, true);
		// Native package callbacks may throw after mutating the model.
		FTransaction::FObjectRecord Record(*Target, std::move(Change));
		try { AppendCommandRecord(std::move(Record)); }
		catch (const std::bad_alloc&) { return FailWithReason(OutError, "Could not allocate the custom transaction record."); }
		catch (const std::length_error&) { return FailWithReason(OutError, "The custom record exceeds transaction capacity."); }
		return true;
	}
	catch (const std::exception& Error) { return FailWithReason(OutError, Error.what()); }
	catch (...) { return FailWithReason(OutError, "Custom transaction admission threw an unknown exception."); }

	auto DTransBuffer::End() -> int32
	{
		CheckThread();
		const int32 Result = ActiveCount;
		if (!OwnsRecording() || !CanTransact()) return 0;
		if (ActiveCount > 1)
		{
			--ActiveCount;
			return Result;
		}
		if (Pending->bNotifyingPreFinalize) return 0;
		const auto Id = Pending->GetId();
		if (Pending->FailureReason) TransactionFault(Id, "End", *Pending->FailureReason);
		if (GIsGameThreadIdInitialized) CheckGameThread();
		try
		{
			if (OnPreTransactionFinalized().IsBound() || OnTransactionStateChanged().IsBound())
			{
				Pending->bNotifyingPreFinalize = true;
				OnTransactionStateChanged().Broadcast(Pending->GetContext(), ETransactionStateEventType::PreTransactionFinalized);
				if (!Pending) return Result;
				OnPreTransactionFinalized().Broadcast(Pending->GetContext());
				if (!Pending) return Result;
				Pending->bNotifyingPreFinalize = false;
			}
			if (--ActiveCount > 0) return Result;
			Pending->Finalize();
			Guard = EHistoryGuard::Notifying;
			OnTransactionStateChanged().Broadcast(Pending->GetContext(), ETransactionStateEventType::TransactionFinalized);
			Guard = EHistoryGuard::Finalizing;
			if (auto Closed = Pending->EndOperation(); !Closed) TransactionFault(Id, "EndOperation", Closed.error());
			Pending->CaptureIndex.reset();
			FTransactionContext Context;
			Context.TransactionId = Id;
			FinalizePending(Context);
			OnTransactionStateChanged().Broadcast(Context, ETransactionStateEventType::PostTransactionFinalized);
		}
		catch (const std::exception& Error) { TransactionFault(Id, "End", Error.what()); }
		catch (...) { TransactionFault(Id, "End", "unknown exception"); }
		return Result;
	}
	auto DTransBuffer::Cancel(int32 StartIndex) -> void
	{
		CheckThread();
		(void)StartIndex; // UE 5.8 cancels the entire recording, including inner Cancel.
		if (!OwnsRecording() || !CanTransact()) return;
		const auto Id = Pending->GetId();
		if (Pending->FailureReason) TransactionFault(Id, "Cancel", *Pending->FailureReason);
		Guard = EHistoryGuard::Notifying;
		try
		{
			OnTransactionStateChanged().Broadcast(Pending->GetContext(), ETransactionStateEventType::TransactionCanceled);
			Guard = EHistoryGuard::Mutating;
			if (auto Closed = Pending->EndOperation(); !Closed) TransactionFault(Id, "EndOperation", Closed.error());
			ReleaseRecording();
			Pending->bBusy = true;
			Pending.reset();
			Guard = EHistoryGuard::None;
		}
		catch (const std::exception& Error) { TransactionFault(Id, "Cancel", Error.what()); }
		catch (...) { TransactionFault(Id, "Cancel", "unknown exception"); }
	}

	auto DTransBuffer::FinalizePending(FTransactionContext& PostContext) -> void
	{
		const FTransactionId TransactionId = Pending->GetId();
		const auto Pie = Pending->ContainsPieObjects();
		if (!Pie) TransactionFault(TransactionId, "Finalize", Pie.error());
		if (!*Pie) Pending->RemoveNoOpRecords();
		if (*Pie || Pending->GetRecordCount() == 0)
		{
			ReleaseRecording();
			Pending->bBusy = true;
			Pending.reset();
			Guard = EHistoryGuard::None;
			return;
		}
		Pending->PreparePackageRecords();
		PostContext.Details = Pending->GetDetails();
		const bool bReplaceRedo = Cursor < History.size();
		const size_t First = bReplaceRedo ? Cursor : 0;
		const size_t Count = bReplaceRedo ? History.size() - Cursor : BudgetPrefixCount;
		// Publish a consistent queue before releasing commands that can run destructor callbacks.
		std::vector<std::unique_ptr<FTransaction>> Retired;
		Retired.reserve(Count);
		for (size_t Index = First; Index < First + Count; ++Index)
			Retired.push_back(std::move(History[Index]));
		History.erase(History.begin() + First, History.begin() + First + Count);
		RemapUndoBarriers(First, Count);
		ReleaseRecording();
		Pending->RecordingOwner = nullptr;
		History.push_back(std::move(Pending));
		Cursor = History.size();
		Retired.clear();
		for (const auto& Record : History.back()->PackageRecords)
			if (auto* Package = Cast<DPackage>(Record.Package.Resolve())) Package->MarkDirty();
		Guard = EHistoryGuard::Notifying;
		OnUndoBufferChanged().Broadcast();
		Guard = EHistoryGuard::None;
	}
	auto DTransBuffer::Undo(bool bCanRedo) -> bool
	{
		return Replay(true, std::nullopt, bCanRedo);
	}

	auto DTransBuffer::UndoExpected(FTransactionId ExpectedId, std::string* OutError) -> bool
	{
		return Replay(true, ExpectedId, true, OutError);
	}

	auto DTransBuffer::Redo() -> bool { return Replay(false); }

	auto DTransBuffer::RedoExpected(FTransactionId ExpectedId, std::string* OutError) -> bool
	{
		return Replay(false, ExpectedId, true, OutError);
	}

	auto DTransBuffer::CanReplayInHost(std::string* Reason) const -> bool
	{
		const auto* Host = Cast<DEditorEngine>(GetOuter());
		if (Host && !Host->CanReplayTransactions())
			return FailWithReason(Reason, "Editor world playback or shutdown prevents history replay.");
		return true;
	}

	auto DTransBuffer::Replay(bool bUndo, std::optional<FTransactionId> ExpectedId, bool bCanRedo, std::string* OutError) -> bool
	{
		CheckThread();
		auto& Completed = bUndo ? OnUndo() : OnRedo();
		auto Reject = [&](std::string_view Reason, EHistoryGuard Mode = EHistoryGuard::Inspecting) {
			if (Guard == EHistoryGuard::None)
			{
				const TGuardValue Notify(Guard, Mode);
				Completed.Broadcast({}, false);
			}
			return FailWithReason(OutError, Reason);
		};
		std::string HostReason;
		if (!CanReplayInHost(&HostReason)) return Reject(HostReason);
		if (!CanAccessReplayHistory())
			return Reject(bUndo ? "Undo requires an idle transactor." : "Redo requires an idle transactor.");
		const size_t UndoLimit = UndoBarriers.empty() ? 0 : UndoBarriers.back();
		if (bUndo && Cursor != 0 && Cursor <= UndoLimit)
			return Reject("An undo barrier blocks earlier history.");
		if (bUndo ? Cursor == 0 : Cursor == History.size())
			return Reject(bUndo ? "Nothing to undo." : "Nothing to redo.");
		auto ReplayId = History[bUndo ? Cursor - 1 : Cursor]->GetId();
		if (ExpectedId && *ExpectedId != ReplayId)
			return Reject(bUndo ? "The expected transaction is not the Undo head." : "The expected transaction is not the Redo head.");

		GIsTransacting = true;
		Guard = EHistoryGuard::Mutating;
		auto FinishReplay = [&] {
			Guard = EHistoryGuard::None;
			GIsTransacting = false;
		};

		try
		{
			do
			{
				const auto PreviousCursor = Cursor;
				auto& Transaction = *History[bUndo ? --Cursor : Cursor++];
				ReplayId = Transaction.GetId();
				Guard = EHistoryGuard::Notifying;
				const bool bExpired = Transaction.HasExpired();
				Guard = EHistoryGuard::Mutating;
				if (ExpectedId && bExpired)
				{
					Cursor = PreviousCursor;
					FinishReplay();
					return Reject("The expected transaction has expired.");
				}
				if (!bExpired)
				{
					const bool bOwnOperation = Transaction.OperationId == 0;
					if (bOwnOperation)
						if (const auto Begin = Transaction.BeginOperation(); !Begin) TransactionFault(ReplayId, "BeginOperation", Begin.error());
					FTransaction::FReplayResult Outcome;
					if (auto Valid = Transaction.PrepareReplay(bUndo, Outcome, /*bCheckExpiry=*/false); !Valid)
					{
						Cursor = PreviousCursor;
						if (bOwnOperation) (void)Transaction.EndOperation();
						FinishReplay();
						return Reject(Valid.error(), EHistoryGuard::Notifying);
					}
					auto Context = Transaction.GetContext();
					Guard = EHistoryGuard::Notifying;
					OnTransactionStateChanged().Broadcast(Context, ETransactionStateEventType::UndoRedoStarted);
					Guard = EHistoryGuard::Mutating;
					auto InversePackages = Transaction.PackageRecords;
					for (auto& Inverse : InversePackages)
						if (auto* Package = Cast<DPackage>(Inverse.Package.Resolve()))
						{
							Inverse.bWasDirty = Package->IsDirty();
							Inverse.SaveCount = GetTransactionPackageSaveCount(*Package);
						}
					Transaction.ApplyPrepared(bUndo, Outcome);
					for (size_t Slot = 0; Slot < Transaction.PackageRecords.size(); ++Slot)
					{
						auto& Record = Transaction.PackageRecords[Slot];
						auto* Package = Cast<DPackage>(Record.Package.Resolve());
						if (!Package) continue;
						const bool bDirty = Record.bWasDirty || Record.SaveCount != GetTransactionPackageSaveCount(*Package);
						Record = InversePackages[Slot];
						if (bDirty) Package->MarkDirty();
						else Package->ClearDirty();
					}
					Guard = EHistoryGuard::Notifying;
					Context.Details = std::move(Outcome.Details);
					Completed.Broadcast(Context, true);
					OnTransactionStateChanged().Broadcast(Context, ETransactionStateEventType::UndoRedoFinalized);
					if (bOwnOperation) (void)Transaction.EndOperation();
				}
				if (!bCanRedo)
				{
					Guard = EHistoryGuard::Finalizing;
					History.erase(History.begin() + Cursor, History.end());
					for (auto& Barrier : UndoBarriers) Barrier = std::min(Barrier, Cursor);
					Guard = EHistoryGuard::Notifying;
					OnUndoBufferChanged().Broadcast();
				}
				if (!bExpired)
				{
					FinishReplay();
					return true;
				}
			} while (bUndo ? Cursor > UndoLimit : Cursor < History.size());
			FinishReplay();
			return Reject("All remaining transactions have expired.");
		}
		catch (const std::exception& Error)
		{
			TransactionFault(ReplayId, bUndo ? "Undo" : "Redo", Error.what());
		}
		catch (...)
		{
			TransactionFault(ReplayId, bUndo ? "Undo" : "Redo", "unknown exception");
		}
	}

	auto DTransBuffer::Reset(std::string_view Reason) -> void
	{
		CheckThread();
		if (!CanTransact())
		{
			DURIN_ERROR("Cannot reset transaction history: a history transition is in progress.");
			return;
		}
		if (Pending && Pending->FailureReason) TransactionFault(Pending->GetId(), "Reset", *Pending->FailureReason);
		// Own the reason before releasing entries that may contain it.
		ClearHistory(std::string(Reason));
	}

	auto DTransBuffer::ClearHistory(std::string Reason) -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		Guard = EHistoryGuard::Mutating;

		ReleaseRecording();
		if (Pending) Pending->bBusy = true;
		auto RetiredHistory = std::exchange(History, {});
		for (auto& Transaction : RetiredHistory) Transaction->bBusy = true;
		Cursor = 0; UndoBarriers.clear();
		Pending.reset();
		RetiredHistory.clear();
		ResetReason = std::move(Reason);
		Guard = EHistoryGuard::Notifying;
		OnUndoBufferChanged().Broadcast();
		Guard = EHistoryGuard::None;
	}

	auto DTransBuffer::DiscardCustomChangesByModule(std::string_view ModuleName) -> bool
	{
		CheckThread();
		if (ModuleName.empty() || !IsHistoryIdle()) return false;
		const TGuardValue Inspect(Guard, EHistoryGuard::Inspecting);
		const size_t PreviousSize = History.size();
		for (size_t Index = History.size(); Index-- > 0;)
		{
			if (!History[Index]->IsOwnedByModule(ModuleName)) continue;
			for (auto* Package : History[Index]->GetAffectedPackages())
				if (IsValid(Package) && Package->IsAssetPackage()) Package->MarkDirty();
			RemapUndoBarriers(Index, 1);
			History.erase(History.begin() + Index);
			if (Index < Cursor) --Cursor;
		}
		if (History.size() != PreviousSize)
		{
			Guard = EHistoryGuard::Notifying;
			OnUndoBufferChanged().Broadcast();
		}
		return true;
	}


	auto DTransBuffer::CanUndo(std::string* Reason) const -> bool
	{
		if (!CanInspectReplayHistory())
		{
			if (Reason) *Reason = "A transaction is active.";
			return false;
		}
		if (!CanReplayInHost(Reason)) return false;
		const bool bBarrier = !UndoBarriers.empty() && Cursor <= UndoBarriers.back();
		const bool bAvailable = Cursor != 0 && !bBarrier;
		if (Reason) *Reason = bAvailable ? "" : bBarrier ? "An undo barrier blocks earlier history." : ResetReason.empty() ? "Nothing to undo." : ResetReason;
		return bAvailable;
	}

	auto DTransBuffer::SetUndoBarrier() -> void
	{
		CheckThread();
		if (CanAccessReplayHistory()) UndoBarriers.push_back(Cursor);
	}
	auto DTransBuffer::RemoveUndoBarrier() -> void
	{
		CheckThread();
		if (CanAccessReplayHistory() && !UndoBarriers.empty()) UndoBarriers.pop_back();
	}
	auto DTransBuffer::ClearUndoBarriers() -> void
	{
		CheckThread();
		if (CanAccessReplayHistory() && !UndoBarriers.empty()) UndoBarriers.clear();
	}
	auto DTransBuffer::RemapUndoBarriers(size_t First, size_t Count) -> void
	{
		for (auto& Barrier : UndoBarriers) if (Barrier > First) Barrier -= std::min(Count, Barrier - First);
	}
	auto DTransBuffer::SetMemoryBudget(size_t Bytes) -> bool
	{
		CheckThread();
		if (!CanAccessReplayHistory()) return false;
		MemoryBudget = Bytes;
		return true;
	}
	auto DTransBuffer::ContainsPieObjects() const -> bool
	{
		CheckThread();
		if (Guard != EHistoryGuard::None
			|| (Pending && Pending->IsBusy()))
			return true;
		if (GIsGameThreadIdInitialized) CheckGameThread();
		const TGuardValue Inspect(Guard, EHistoryGuard::Inspecting);
		if (Pending && Pending->ContainsPieObjects().value_or(true)) return true;
		for (const auto& Transaction : History)
			if (Transaction->ContainsPieObjects().value_or(true)) return true;
		return false;
	}

	auto DTransBuffer::CanRedo(std::string* Reason) const -> bool
	{
		if (!CanInspectReplayHistory())
		{
			if (Reason) *Reason = "A transaction is active.";
			return false;
		}
		if (!CanReplayInHost(Reason)) return false;
		const bool bAvailable = Cursor < History.size();
		if (Reason) *Reason = bAvailable ? "" : "Nothing to redo.";
		return bAvailable;
	}

	auto DTransBuffer::AddReferencedObjects(FReferenceCollector& Collector) -> void
	{
		DTransactor::AddReferencedObjects(Collector);
		Collector.AllowEliminatingReferences(false);
		if (Pending) Pending->AddReferencedObjects(Collector);
		for (const auto& Transaction : History) Transaction->AddReferencedObjects(Collector);
		Collector.AllowEliminatingReferences(true);
	}

	auto DTransBuffer::BeginDestroy() -> void
	{
		CheckThread();
		Guard = EHistoryGuard::Mutating;
		ReleaseRecording();
		Pending.reset();
		History.clear();
		Cursor = 0;
		DTransactor::BeginDestroy();
	}

	auto DTransBuffer::GetUndoSize() const -> size_t
	{
		CheckThread();
		if (History.empty()) return 0;
		if ((Guard != EHistoryGuard::None && Guard != EHistoryGuard::Notifying) || (Pending && Pending->IsBusy()))
			return std::numeric_limits<size_t>::max();
		const TGuardValue Inspect(Guard, EHistoryGuard::Inspecting);
		std::optional<TGuardValue<bool>> Busy;
		if (Pending) Busy.emplace(Pending->bBusy, true);
		size_t Total = 0;
		for (const auto& Transaction : History)
		{
			size_t Bytes = 0;
			if (!Transaction->TryGetOwnedSize(Bytes) || !AddSize(Total, Bytes))
				return std::numeric_limits<size_t>::max();
		}
		return Total;
	}

	auto DTransBuffer::GetUndoContext(bool bCheckWhetherUndoPossible) const -> FTransactionContext
	{
		if (Guard == EHistoryGuard::Finalizing) return {};
		if (bCheckWhetherUndoPossible && !CanUndo()) return {};
		return Cursor ? History[Cursor - 1]->GetContext() : FTransactionContext{};
	}

	auto DTransBuffer::GetRedoContext() const -> FTransactionContext
	{
		return CanRedo() ? History[Cursor]->GetContext() : FTransactionContext{};
	}

	auto FTransaction::SavePackage(DPackage* Package) -> void
	{
		if (!IsValid(Package) || !Package->IsAssetPackage()) return;
		const FPersistentObjectRef Ref(Package);
		if (std::ranges::find(PackageRecords, Ref, &FTransactionPackageRecord::Package) == PackageRecords.end())
			PackageRecords.push_back({Ref, Package->IsDirty(), GetTransactionPackageSaveCount(*Package)});
	}

	auto FTransaction::PreparePackageRecords() -> void
	{
		const auto Packages = GetAffectedPackages();
		std::erase_if(PackageRecords, [&](const auto& Record) {
			return !std::ranges::contains(Packages, Record.Package.Resolve());
		});
		for (auto* Package : Packages) SavePackage(Package);
	}

	auto DTransBuffer::ForgetPackage(DPackage& Package) -> void
	{
		if (!IsHistoryIdle()) return;
		const TGuardValue Inspect(Guard, EHistoryGuard::Inspecting);
		const size_t PreviousSize = History.size();
		for (size_t Index = History.size(); Index-- > 0;)
		{
			if (!History[Index]->ReferencesPackage(Package)) continue;
			for (auto* Affected : History[Index]->GetAffectedPackages())
				if (IsValid(Affected) && Affected != &Package && Affected->IsAssetPackage()) Affected->MarkDirty();
			RemapUndoBarriers(Index, 1);
			History.erase(History.begin() + Index);
			if (Index < Cursor) --Cursor;
		}
		if (History.size() != PreviousSize)
		{
			Guard = EHistoryGuard::Notifying;
			OnUndoBufferChanged().Broadcast();
		}
	}

	auto FTransaction::Reinstance(const FObjectReplacementMap& Map, std::vector<DPackage*>* OutPreReloadPackages)
		-> FTransactionCaptureResult try
	{
		const bool bPackageReload = OutPreReloadPackages != nullptr;
		if (bPackageReload)
		{
			if (!Map.Find(PrimaryObject.Resolve())
				&& !std::ranges::any_of(PackageRecords, [&](const auto& R) { return Map.Find(R.Package.Resolve()); })
				&& !std::ranges::any_of(Records, [&](const auto& R) { return R.HasReplacementReferences(Map); }))
				return {};
			*OutPreReloadPackages = GetAffectedPackages();
		}
		FReinstancingReferenceCollector Collector(Map);
		auto Rebind = [&](FPersistentObjectRef& Reference) -> FTransactionCaptureResult {
			if (bPackageReload)
			{
				// The package publisher will retire the old graph, including unmapped objects.
				auto Replacement = Reference.CreateReplacement(Map);
				if (!Replacement) return std::unexpected(Replacement.error());
				Reference = std::move(*Replacement);
			}
			else Reference.AddReferencedObjects(Collector);
			return {};
		};
		if (auto Result = Rebind(PrimaryObject); !Result) return Result;
		for (auto& Transition : PackageRecords)
		{
			const auto Before = Transition.Package.GetKey();
			if (auto Result = Rebind(Transition.Package); !Result) return Result;
			if (Before != Transition.Package.GetKey()) Transition.bWasDirty = true;
		}
		for (auto& Record : Records)
		{
			if (Record.HasReplacementReferences(Map))
				if (auto Result = Record.Reinstance(Map); !Result) return Result;
			if (bPackageReload)
			{
				if (auto Valid = Record.Validate(); !Valid) return Valid;
				if (Record.HasReplacementReferences(Map))
					return std::unexpected("Reinstanced history retained an old-generation reference.");
			}
		}
		return {};
	}
	catch (const std::bad_alloc&) { std::terminate(); }
	catch (const std::exception& Error) { return std::unexpected(std::string(Error.what())); }
	catch (...) { return std::unexpected("Transaction replacement hook threw."); }

	class FTransactorReplacementParticipant final : public IPackageReloadParticipant
	{
	public:
		explicit FTransactorReplacementParticipant(DTransBuffer& InTransactor)
			: Transactor(InTransactor), bPackageReload(false) {}
		FTransactorReplacementParticipant(DTransBuffer& InTransactor, std::span<DPackage* const> InPackages)
			: Transactor(InTransactor), bPackageReload(true), Packages(InPackages.begin(), InPackages.end()) {}

		auto Prepare(const FObjectReplacementMap& InMap) -> std::expected<void, FObjectReplacementError> override
		{
			Abort();
			if (!Transactor.IsHistoryIdle())
				return std::unexpected(FObjectReplacementError{.Code = EObjectReplacementError::Busy, .Reason = EObjectReplacementReason::ParticipantBusy});
			if (!InMap.ValidateIdentities())
				return std::unexpected(FObjectReplacementError{.Code = EObjectReplacementError::Stale, .Reason = EObjectReplacementReason::ObjectIdentityChanged});
			for (auto* Package : Packages)
			{
				const auto* Entry = InMap.Find(Package);
				if (!Entry || !Entry->Replacement)
					return std::unexpected(FObjectReplacementError{.Code = EObjectReplacementError::UnmappedReference,
						.Reason = EObjectReplacementReason::ParticipantUnmappedPackage});
			}
			if (bPackageReload)
			{
				// Candidate-only editor admission; no live authored fields are loaded.
				for (auto* Object : InMap.GetPreparedObjects())
					if (!Object->IsTemplateObject() || std::ranges::any_of(InMap.GetEntries(), [&](const auto& Entry) {
						return Entry.Replacement == Object && Entry.Previous->HasAnyObjectFlags(EObjectFlags::Transactional);
					})) Object->SetFlags(EObjectFlags::Transactional);
			}
			Map = &InMap;
			return {};
		}
		auto Validate() const -> bool override { return Map && Transactor.IsHistoryIdle() && Map->ValidateIdentities(); }
		auto CoversNativeReferences(const DObject& Owner) const -> bool override { return Map && &Owner == &Transactor; }
		auto GetStrongReferenceCount(const DObject&) const -> uint32 override { return 0; }
		auto Commit() noexcept -> void override
		{
			check(Transactor.IsHistoryIdle());
			Transactor.Guard = DTransBuffer::EHistoryGuard::Mutating;
		}
		auto OnObjectsReinstanced(const FObjectReplacementMap& Mapping) noexcept -> void override
		{
			check(Map == &Mapping);
			Transactor.OnObjectsReinstanced(Mapping, bPackageReload, Retired);
		}
		auto Abort() noexcept -> void override { Map = nullptr; }
		auto CanRetire() const -> bool override
		{
			Retired.clear();
			return true;
		}
	private:
		DTransBuffer& Transactor;
		const bool bPackageReload;
		std::vector<DPackage*> Packages;
		const FObjectReplacementMap* Map = nullptr;
		mutable std::vector<std::unique_ptr<FTransaction>> Retired;
	};

	auto CreateTransactorReplacementParticipant(DTransBuffer& Transactor) -> std::shared_ptr<IObjectReplacementParticipant>
	{
		return std::make_shared<FTransactorReplacementParticipant>(Transactor);
	}

	auto CreateTransactorReloadParticipant(DTransactor& Transactor,
		std::span<DPackage* const> Packages)
		-> std::shared_ptr<IPackageReloadParticipant>
	{
		auto* Buffer = Cast<DTransBuffer>(&Transactor);
		if (!Buffer) return {};
		return std::make_shared<FTransactorReplacementParticipant>(*Buffer, Packages);
	}

	auto DTransBuffer::OnObjectsReinstanced(const FObjectReplacementMap& Mapping, bool bPackageReload,
		std::vector<std::unique_ptr<FTransaction>>& Retired) -> void
	{
		CheckGameThread();
		check(Guard == EHistoryGuard::Mutating);
		std::vector<DPackage*> DirtyPackages;
		for (size_t Index = 0; Index < History.size();)
		{
			auto& Entry = History[Index];
			std::vector<DPackage*> OriginalPackages;
			auto Result = Entry->Reinstance(Mapping, bPackageReload ? &OriginalPackages : nullptr);
			if (Result)
			{
				++Index;
				continue;
			}

			DURIN_ERROR("Discarding transaction {} during reinstancing: {}", Entry->GetId(), Result.error());
			for (auto* Package : OriginalPackages)
				if (Package && !Mapping.Find(Package) && !std::ranges::contains(DirtyPackages, Package)) DirtyPackages.push_back(Package);
			Retired.push_back(std::move(Entry));
			History.erase(History.begin() + Index);
			RemapUndoBarriers(Index, 1);
			if (Index < Cursor) --Cursor;
		}
		for (auto* Package : DirtyPackages) Package->MarkDirty();
		if (!Retired.empty())
		{
			Guard = EHistoryGuard::Notifying;
			OnUndoBufferChanged().Broadcast();
		}
		Guard = EHistoryGuard::None;
	}
}
