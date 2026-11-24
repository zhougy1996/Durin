#include "Transactions/Transactor.h"

#include "Editor/EditorEngine.h"
#include "Asset/PackageRemoval.h"
#include "Components/SceneComponent.h"

#include "PropertyEditor/PropertyEditing.h"
#include "DObject/Archive.h"
#include "DObject/DObjectArray.h"
#include "DObject/Class.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "DObject/Property.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/TransactionArray.h"
#include "DObject/ObjectGraphReplacement.h"
#include "CoreGlobals.h"
#include "Logging/LogMacros.h"
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

		struct FTransactionBusyScope
		{
			bool& Busy;
			const bool Previous;
			explicit FTransactionBusyScope(bool& Value) : Busy(Value), Previous(std::exchange(Value, true)) {}
			~FTransactionBusyScope() { Busy = Previous; }
		};
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

	struct DTransBuffer::FInspectionScope
	{
		EHistoryGuard& Guard;
		EHistoryGuard Previous;
		explicit FInspectionScope(EHistoryGuard& InGuard)
			: Guard(InGuard), Previous(std::exchange(InGuard, EHistoryGuard::Inspecting)) {}
		~FInspectionScope() { Guard = Previous; }
	};

	FTransaction::FObjectRecord::FObjectRecord(std::unique_ptr<FCommandChange> Change, DObject* Object)
		: Data(std::move(Change)), CustomObject(FPersistentObjectRef::FromKey(FObjectKey(Object)))
	{
		if (Object) ModifiedObjects.Add(FPersistentObjectRef::FromKey(FObjectKey(Object)));
		const auto& Custom = std::get<std::unique_ptr<FCommandChange>>(Data);
		if (Custom)
		{
			for (DObject* Participant : Custom->GetModifiedObjects())
				ModifiedObjects.Add(FPersistentObjectRef::FromKey(FObjectKey(Participant)));
			for (DPackage* Package : Custom->GetAffectedPackages())
				DeclaredPackages.Add(FPersistentObjectRef::FromKey(FObjectKey(Package)));
		}
	}

	auto FTransaction::FObjectRecord::HasReplacementReferences(const FObjectReplacementMap& Map) const -> bool
	{
		if (const auto* Record = std::get_if<FTransaction::FArrayData>(&Data)) return Record->HasReplacementReferences(Map);
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
		FReinstancingReferenceCollector Collector(Map);
		ModifiedObjects.AddReferencedObjects(Collector);
		if (auto* Array = std::get_if<FArrayData>(&Data)) return Array->Reinstance(Map);
		if (auto* Object = std::get_if<FWholeObjectData>(&Data)) return Object->Reinstance(Map);
		auto& Command = std::get<std::unique_ptr<FCommandChange>>(Data);
		if (!Command) return std::unexpected("A missing custom command cannot be reinstanced.");
		CustomObject.AddReferencedObjects(Collector);
		DeclaredPackages.AddReferencedObjects(Collector);
		Command->AddReferencedObjects(Collector);
		for (auto* Participant : Command->GetModifiedObjects())
			if (Map.Find(Participant)) return std::unexpected("Command retained a replaced participant.");
		for (auto* Package : Command->GetAffectedPackages())
			if (Map.Find(Package)) return std::unexpected("Command retained a replaced package.");
		return {};
	}

	auto FTransaction::FObjectRecord::GetModifiedObjects() const -> std::vector<DObject*>
	{
		if (const auto* Record = std::get_if<FTransaction::FArrayData>(&Data)) return {Record->GetTarget().Get()};
		if (const auto* Record = std::get_if<FTransaction::FWholeObjectData>(&Data))
			return {Record->GetTarget().Get()};
		std::vector<DObject*> Result;
		for (const auto& Reference : ModifiedObjects.GetReferences())
			if (auto* Object = Reference.Get()) Result.push_back(Object);
		return Result;
	}

	auto FTransaction::FObjectRecord::HasExpired() const noexcept -> bool
	{
		return std::visit([this](const auto& Record) -> bool {
			using T = std::decay_t<decltype(Record)>;
			if constexpr (std::is_same_v<T, std::unique_ptr<FCommandChange>>)
			{
				const auto& Target = CustomObject.IsNull() && ModifiedObjects.Num() == 1
					? ModifiedObjects.GetReferences().front() : CustomObject;
				return Record && Record->HasExpired(Target.Resolve());
			}
			else
				return Record.HasExpired();
		}, Data);
	}

	auto FTransaction::FObjectRecord::Validate() const -> FTransactionCaptureResult
	{
		if (const auto* Record = std::get_if<FTransaction::FArrayData>(&Data)) return Record->Validate();
		return std::visit([&](const auto& Record) -> FTransactionCaptureResult {
			using T = std::decay_t<decltype(Record)>;
			if constexpr (std::is_same_v<T, FTransaction::FWholeObjectData>)
			{
				for (const auto& Reference : ModifiedObjects.GetReferences())
					if (!Reference.Get()) return std::unexpected("A modified transaction participant is unavailable.");
				const auto Result = Record.Validate();
				if (!Result) return std::unexpected(FormatTransactionObjectRecordError(Result.error()));
			}
			else if constexpr (std::is_same_v<T, std::unique_ptr<FCommandChange>>)
				if (!Record) return std::unexpected(std::string("The custom transaction change is unavailable."));
			return {};
		}, Data);
	}

	auto FTransaction::FObjectRecord::PrepareReplay() -> FTransactionCaptureResult
	{
		if (auto* Object = std::get_if<FTransaction::FWholeObjectData>(&Data))
			if (auto Prepared = Object->PrepareReplay(); !Prepared)
				return std::unexpected(FormatTransactionObjectRecordError(Prepared.error()));
		return {};
	}
	auto FTransaction::FObjectRecord::ClearPreparedReplay() -> void
	{
		if (auto* Array = std::get_if<FTransaction::FArrayData>(&Data)) Array->ClearPreparedReplay();
		if (auto* Object = std::get_if<FTransaction::FWholeObjectData>(&Data)) Object->ClearPreparedReplay();
	}
	auto FTransaction::FObjectRecord::CommitReplay(bool bBefore) -> void
	{
		if (auto* Array = std::get_if<FTransaction::FArrayData>(&Data)) Array->CommitReplay();
		if (auto* Object = std::get_if<FTransaction::FWholeObjectData>(&Data)) Object->CommitReplay(bBefore);
	}

	auto FTransaction::FObjectRecord::CaptureAfter() -> FTransactionCaptureResult
	{
		if (auto* Record = std::get_if<FTransaction::FWholeObjectData>(&Data))
			if (auto Result = Record->CaptureAfter(); !Result)
				return std::unexpected(FormatTransactionObjectRecordError(Result.error()));
		return {};
	}

	auto FTransaction::FObjectRecord::IsNoOp() const -> bool
	{
		if (const auto* Array = std::get_if<FTransaction::FArrayData>(&Data)) return Array->IsNoOp();
		if (const auto* Record = std::get_if<FTransaction::FWholeObjectData>(&Data))
			return Record->IsNoOp();
		return false;
	}
	auto FTransaction::FObjectRecord::GetReplayAnnotation() const -> std::pair<DObject*, std::shared_ptr<ITransactionObjectAnnotation>>
	{
		if (const auto* Array = std::get_if<FTransaction::FArrayData>(&Data); Array && Array->HasReplayCompletion())
			return {Array->GetTarget().Get(), Array->GetReplayAnnotation()};
		if (const auto* Object = std::get_if<FTransaction::FWholeObjectData>(&Data))
			return {Object->GetTarget().Get(), Object->GetReplayAnnotation()};
		return {};
	}

	auto FTransaction::FObjectRecord::Apply(bool bBefore, EPropertyChangeOrigin Origin) -> FTransactionCaptureResult
	{
		return std::visit([&](auto& Record) -> FTransactionCaptureResult {
			using T = std::decay_t<decltype(Record)>;
			if constexpr (std::is_same_v<T, FTransaction::FWholeObjectData>)
			{
				try
				{
					const auto Result = Record.Apply(bBefore, Origin);
					if (!Result)
						return std::unexpected(FormatTransactionObjectRecordError(Result.error()));
				}
				catch (const std::exception& Error)
				{
					return std::unexpected(std::format("Object transaction replay threw: {}", Error.what()));
				}
				catch (...)
				{
					return std::unexpected(std::string("Object transaction replay threw an unknown exception."));
				}
			}
			else if constexpr (std::is_same_v<T, FTransaction::FArrayData>)
			{
				try { return Record.Apply(Origin); }
				catch (const std::exception& Error) { return std::unexpected(std::format("Array transaction replay threw: {}", Error.what())); }
				catch (...) { return std::unexpected("Array transaction replay threw an unknown exception."); }
			}
			else
			{
				if (!Record) return std::unexpected(std::string("The custom transaction change is unavailable."));
				try
				{
					if (const auto Applied = bBefore ? Record->Revert() : Record->Apply(); !Applied)
						return std::unexpected(Applied.error().Message);
				}
				catch (const std::exception& Error)
				{
					return std::unexpected(std::format("Custom transaction replay threw: {}", Error.what()));
				}
				catch (...)
				{
					return std::unexpected(std::string("Custom transaction replay threw an unknown exception."));
				}
			}
			return {};
		}, Data);
	}

	auto FTransaction::FObjectRecord::AddReferencedObjects(FReferenceCollector& Collector) const -> void
	{
		ModifiedObjects.AddReferencedObjects(Collector);
		std::visit([&](const auto& Record) {
			using T = std::decay_t<decltype(Record)>;
			if constexpr (std::is_same_v<T, std::unique_ptr<FCommandChange>>)
			{
				DeclaredPackages.AddReferencedObjects(Collector);
				if (Record) Record->AddReferencedObjects(Collector);
			}
			else Record.AddReferencedObjects(Collector);
		}, Data);
	}

	auto FTransaction::FObjectRecord::TryGetAllocatedSize(size_t& OutBytes) const -> bool
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
				size_t PackageBytes = 0;
				if (!DeclaredPackages.TryGetAllocatedSize(PackageBytes) || !AddSize(OutBytes, PackageBytes)) return false;
				if (!AddSize(OutBytes, CustomObject.GetAllocatedSize())) return false;
				return true;
			}
			else return Record.TryGetAllocatedSize(OutBytes);
		}, Data);
	}

	auto FTransaction::FObjectRecord::GetDetails(ETransactionOperation Operation) const -> std::string
	{
		if (const auto* Change = std::get_if<std::unique_ptr<FCommandChange>>(&Data);
			Change && *Change)
		{
			if (Operation == ETransactionOperation::Execute) return (*Change)->GetDetails(Operation);
			try { return (*Change)->GetDetails(Operation); }
			catch (const std::exception& Error) { DURIN_WARN("Transaction replay details unavailable: {}", Error.what()); }
			catch (...) { DURIN_WARN("Transaction replay details unavailable."); }
		}
		return {};
	}

	auto FTransaction::FObjectRecord::GetAffectedPackages() const -> std::vector<DPackage*>
	{
		if (const auto* Array = std::get_if<FTransaction::FArrayData>(&Data)) return Array->GetAffectedPackages();
		if (const auto* Object = std::get_if<FTransaction::FWholeObjectData>(&Data)) return Object->GetAffectedPackages();
		std::vector<DPackage*> Packages;
		for (const auto& Reference : DeclaredPackages.GetReferences())
			if (auto* Package = Cast<DPackage>(Reference.Get())) Packages.push_back(Package);
		return Packages;
	}

	auto FTransaction::FObjectRecord::IsOwnedByModule(std::string_view ModuleName) const -> bool
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
	auto FTransaction::StoreUndo(DObject* Object, std::unique_ptr<FCommandChange> Change) -> FTransactionCaptureResult
	{
		if (bBusy || bFinalized) return std::unexpected("Transaction records are closed to capture.");
		if (!IsValid(Object) || !Change) return std::unexpected("Object-bound custom capture requires a live object and change.");
		if (RecordingOwner)
		{
			if (GUndo != this) return std::unexpected("The transaction is not the active recording.");
			std::string Error;
			if (!RecordingOwner->StoreUndo(Object, std::move(Change), &Error)) return std::unexpected(std::move(Error));
		}
		else AddRecord(std::move(Change), Object);
		return {};
	}
	auto FTransaction::SnapshotObject(DObject* Object, std::span<const FProperty* const> Properties) -> FTransactionCaptureResult
	{
		if (bFinalized && !bBusy) return {};
		if (bBusy || OperationId == 0 || !IsValid(Object)
			|| (RecordingOwner && (GUndo != this || !RecordingOwner->CanTransact())))
			return std::unexpected("Object snapshots require an active recording outside transaction callbacks.");
		FTransactionCaptureResult Result;
		{
			if (GIsGameThreadIdInitialized) CheckGameThread();
			FTransactionBusyScope BusyScope(bBusy);
			if (const size_t Position = FindWholeRecord(Object))
				Result = DispatchObjectEvents(ETransactionObjectEventType::Snapshot, ETransactionOperation::Execute,
					&std::get<FTransaction::FWholeObjectData>(Records[Position - 1].Data), Properties);
		}
		if (!Result && bObjectEventFailed && RecordingOwner)
			TransactionFault(Id, "Snapshot", Result.error());
		return Result;
	}
	auto FTransaction::SaveArray(const FTransactionArrayCapture& Capture) -> FTransactionCaptureResult
	{
		if (GIsGameThreadIdInitialized && !IsInGameThread()) return std::unexpected("Array capture requires the GameThread.");
		if (bBusy || bFinalized || OperationId == 0 || (RecordingOwner && (GUndo != this || !RecordingOwner->CanTransact())))
			return std::unexpected("Array capture requires an active recording outside callbacks.");
		if (!Capture.Object || !CanTransactObject(*Capture.Object)) return std::unexpected("Array owner is unavailable.");
		if (auto* Package = Capture.Object->GetPackage(); Package && Package->IsPlayInEditor())
			return std::unexpected("Array capture rejects Play-in-Editor owners.");
		if (!ResolveTransactionArray(Capture)) return std::unexpected("Array capture requires a valid reflected member path.");
		if (!Capture.Object->HasAnyObjectFlags(EObjectFlags::Transactional)) return {};
		if (FindWholeRecord(Capture.Object)) return {};
		if (GIsGameThreadIdInitialized) CheckGameThread();
		FTransactionBusyScope BusyScope(bBusy);
		try
		{
			if (Capture.Count) SavePackage(Capture.Object->GetPackage());
			auto Record = FTransaction::FArrayData::Capture(Capture);
			if (!Record) return std::unexpected(Record.error());
			if (Capture.Count == 0) return {};
			AppendRecord(FTransaction::FObjectRecord(std::move(*Record)));
			return {};
		}
		catch (const std::exception& Error) { return std::unexpected(std::format("Array capture failed: {}", Error.what())); }
		catch (...) { return std::unexpected("Array capture failed."); }
	}
	auto FTransaction::ContainsPieObjects() const -> std::expected<bool, std::string>
	{
		if (bBusy) return std::unexpected("Transaction references are already being inspected or changed.");
		if (GIsGameThreadIdInitialized) CheckGameThread();
		FTransactionBusyScope BusyScope(bBusy);
		return InspectPieReferences(*this);
	}

	auto FTransaction::IndexRecord(FObjectIndex& Index, const FTransaction::FObjectRecord& Record, size_t Position) -> void
	{
		if (const auto* Whole = std::get_if<FTransaction::FWholeObjectData>(&Record.Data))
			Index[Whole->GetTarget()].WholeRecord = Position;
		else if (const auto* Array = std::get_if<FTransaction::FArrayData>(&Record.Data))
			Index.try_emplace(Array->GetTarget());
		else
		{
			for (const auto& Participant : Record.ModifiedObjects.GetReferences())
				Index.try_emplace(Participant);
			if (!Record.CustomObject.IsNull()) Index[Record.CustomObject].bCustomOwner = true;
		}
	}

	auto FTransaction::GetCaptureIndex() -> FObjectIndex&
	{
		if (!CaptureIndex)
		{
			auto Index = std::make_unique<FObjectIndex>();
			for (size_t Slot = 0; Slot < Records.size(); ++Slot)
				IndexRecord(*Index, Records[Slot], Slot + 1);
			CaptureIndex = std::move(Index);
		}
		return *CaptureIndex;
	}

	auto FTransaction::AppendRecord(FTransaction::FObjectRecord&& Record) -> size_t
	{
		auto& Index = GetCaptureIndex();
		try
		{
			IndexRecord(Index, Record, Records.size() + 1);
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

	auto FTransaction::FindWholeRecord(DObject* Object) -> size_t
	{
		if (Records.empty()) return 0;
		const auto& Index = GetCaptureIndex();
		const auto It = Index.find(FPersistentObjectRef(Object));
		return It != Index.end() && It->first.Resolve() == Object ? It->second.WholeRecord : 0;
	}

	auto FTransaction::AddRecord(std::unique_ptr<FCommandChange> Change, DObject* Object) -> uint64
	{
		if (bBusy || bFinalized) return 0;
		FTransactionBusyScope BusyScope(bBusy);
		return AppendRecord(FTransaction::FObjectRecord(std::move(Change), Object));
	}

	auto FTransaction::ContainsObject(const DObject* Object) const -> bool
	{
		if (!IsValid(Object)) return false;
		if (CaptureIndex)
		{
			if (CaptureIndex->contains(FPersistentObjectRef::FromKey(FObjectKey(Object)))) return true;
			const auto It = CaptureIndex->find(FPersistentObjectRef(const_cast<DObject*>(Object)));
			return It != CaptureIndex->end() && It->first.Resolve() == Object;
		}
		return std::ranges::any_of(Records, [&](const auto& Record) { return std::ranges::contains(Record.GetModifiedObjects(), Object); });
	}

	auto FTransaction::SaveObject(DObject* Object) -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (bBusy || bFinalized || (RecordingOwner && (GUndo != this || !RecordingOwner->CanTransact())))
			return;
		if (RecordingFailure) return;
		if (!IsValid(Object)) { RecordingFailure = "Transaction object is unavailable for capture."; return; }
		auto* Package = Object->GetPackage();
		FTransactionBusyScope BusyScope(bBusy);
		SavePackage(Package);
		const auto& Index = GetCaptureIndex();
		const auto It = Index.find(FPersistentObjectRef::FromKey(FObjectKey(Object)));
		if (It != Index.end() && It->second.bCustomOwner) return;
		if (FindWholeRecord(Object)) return;
		auto Record = FTransaction::FWholeObjectData::CaptureWhole(*Object);
		if (!Record) { RecordingFailure = FormatTransactionObjectRecordError(Record.error()); return; }
		AppendRecord(FTransaction::FObjectRecord(std::move(*Record)));
	}

	auto FTransaction::Finalize() -> FTransactionCaptureResult try
	{
		if (RecordingFailure) TransactionFault(Id, "Finalize", *RecordingFailure);
		if (bBusy || bFinalized)
			return std::unexpected("The transaction is busy or already finalized.");
		if (GIsGameThreadIdInitialized) CheckGameThread();
		FTransactionBusyScope BusyScope(bBusy);
		std::vector<DObject*> Deleted;
		for (const auto& Record : Records)
		{
			const auto* Whole = std::get_if<FTransaction::FWholeObjectData>(&Record.Data);
			if (!Whole || Whole->GetTarget().GetReferenceType() != FPersistentObjectRef::EReferenceType::RootObject) continue;
			auto* Object = GDObjectArray.Resolve(Whole->GetTarget().GetKey());
			if (Object && Object->IsGarbage() && !Object->IsTemplateObject()) Deleted.push_back(Object);
		}
		const FScopedTransactionHistoryReplay Access(FScopedTransactionHistoryReplay::GetHistoryId(), Deleted);
		for (size_t Index = 0; Index < Records.size(); ++Index)
			if (auto Result = Records[Index].CaptureAfter(); !Result)
				TransactionRecordFault(Id, "Finalize", Index, Result.error());
		if (auto Result = DispatchObjectEvents(ETransactionObjectEventType::Finalized, ETransactionOperation::Execute); !Result)
			TransactionFault(Id, "Finalize", Result.error());
		for (auto& Record : Records) if (auto* ObjectRecord = std::get_if<FTransaction::FWholeObjectData>(&Record.Data)) ObjectRecord->ClearSnapshot();
		bFinalized = true;
		CaptureIndex.reset();
		return {};
	}
	catch (const std::exception& Error) { TransactionFault(Id, "Finalize", Error.what()); }
	catch (...) { TransactionFault(Id, "Finalize", "unknown exception"); }

	auto FTransaction::DispatchObjectEvents(ETransactionObjectEventType Type, ETransactionOperation Operation,
		FTransaction::FWholeObjectData* SnapshotRecord, std::span<const FProperty* const> Selected,
		const FReplayResult* Replay) -> FTransactionCaptureResult
	{
		bool bDispatched = false;
		try
		{
			const auto EventRecords = std::views::iota(size_t{0}, Replay ? Replay->AppliedRecords.size() : Records.size())
				| std::views::transform([&](size_t Index) -> const FTransaction::FObjectRecord& {
					return Records[Replay ? Replay->AppliedRecords[Index] : Index];
				});
			std::vector<const FTransaction::FWholeObjectData*> ObjectRecords;
			if (SnapshotRecord) ObjectRecords.push_back(SnapshotRecord);
			else
				for (const auto& Record : EventRecords)
					if (const auto* ObjectRecord = std::get_if<FTransaction::FWholeObjectData>(&Record.Data)) ObjectRecords.push_back(ObjectRecord);
			auto Depth = [](const auto* Record) { size_t Result = 0; for (auto* Object = Record->GetTarget().Get(); Object && Result < 256; Object = Object->GetOuter()) ++Result; return Result; };
			std::stable_sort(ObjectRecords.begin(), ObjectRecords.end(), [&](const auto* Left, const auto* Right) { return Depth(Left) > Depth(Right); });
			std::vector<std::pair<FPersistentObjectRef, FTransactionObjectEvent>> Events;
			for (const auto* ObjectRecord : ObjectRecords)
			{
				auto Built = SnapshotRecord ? SnapshotRecord->Snapshot(GetContext(), Selected) : ObjectRecord->BuildObjectEvents(GetContext(), Type, Operation);
				if (!Built) return std::unexpected(Built.error());
				for (auto& Event : *Built) Events.push_back(std::move(Event));
			}
			if (!SnapshotRecord)
				for (const auto& Record : EventRecords)
					if (const auto* Array = std::get_if<FTransaction::FArrayData>(&Record.Data); Array && !Array->IsNoOp())
					{
						auto Built = Array->BuildObjectEvents(GetContext(), Type, Operation);
						if (!Built) return std::unexpected(Built.error());
						for (auto& [Target, Event] : *Built)
						{
							auto Existing = std::ranges::find_if(Events, [&](const auto& Entry) { return Entry.first == Target && Entry.second.CreatedBy == Event.CreatedBy; });
							if (Existing == Events.end()) Events.emplace_back(std::move(Target), std::move(Event));
							else
							{
								for (FName Name : Event.Change.ChangedProperties)
									if (!std::ranges::contains(Existing->second.Change.ChangedProperties, Name)) Existing->second.Change.ChangedProperties.push_back(Name);
								Existing->second.Change.bNameChanged |= Event.Change.bNameChanged;
								Existing->second.Change.bOuterChanged |= Event.Change.bOuterChanged;
								Existing->second.Change.bNonPropertyDataChanged |= Event.Change.bNonPropertyDataChanged;
								Existing->second.Change.bHasPendingKillChange |= Event.Change.bHasPendingKillChange;
									Existing->second.Change.bEditorPresenceChanged |= Event.Change.bEditorPresenceChanged;
								if (Operation != ETransactionOperation::Undo) Existing->second.Annotation = std::move(Event.Annotation);
							}
						}
					}
			std::stable_sort(Events.begin(), Events.end(), [](const auto& Left, const auto& Right) {
				auto Depth = [](const auto& Target) { size_t Result = 0; for (auto* Object = Target.Get(); Object && Result < 256; Object = Object->GetOuter()) ++Result; return Result; };
				return Depth(Left.first) > Depth(Right.first);
			});
			for (auto& [Target, Event] : Events)
			{
				auto* Object = Target.Get();
				if (!Object) { bObjectEventFailed |= bDispatched; return std::unexpected("Transaction event target became unavailable."); }
				if (Replay && Event.CreatedBy == ETransactionObjectChangeCreatedBy::TransactionRecord)
					if (const auto It = Replay->Annotations.find(Object); It != Replay->Annotations.end()) Event.Annotation = It->second;
				bDispatched = true;
				Object->PostTransacted(Event);
			}
			return {};
		}
		catch (const std::exception& Error) { bObjectEventFailed = true; return std::unexpected(std::format("Transaction object event failed: {}", Error.what())); }
		catch (...) { bObjectEventFailed = true; return std::unexpected("Transaction object event failed with an unknown exception."); }
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
		return !Records.empty() && std::ranges::all_of(Records, &FTransaction::FObjectRecord::HasExpired);
	}

	auto FTransaction::Validate() const -> FTransactionApplyResult
	{
		std::vector<size_t> Indices(Records.size());
		std::iota(Indices.begin(), Indices.end(), size_t{0});
		return ValidateReplayRecords(Indices);
	}

	auto FTransaction::ValidateReplayRecords(std::span<const size_t> Indices) const -> FTransactionApplyResult
	{
		for (size_t Index : Indices)
			if (const auto Result = Records[Index].Validate(); !Result)
				return std::unexpected(FTransactionApplyError{.RecordIndex = Index, .Message = Result.error()});
		return {};
	}

	auto FTransaction::Apply(bool bUndo, EPropertyChangeOrigin Origin) -> FTransactionApplyResult
	{
		FReplayResult Replay;
		return Apply(bUndo, Origin, Replay);
	}

	auto FTransaction::Apply(bool bUndo, EPropertyChangeOrigin Origin, FReplayResult& Replay) -> FTransactionApplyResult try
	{
		if (RecordingFailure) return std::unexpected(FTransactionApplyError{.Message = *RecordingFailure});
		if (bBusy) return std::unexpected(FTransactionApplyError{.Message = "A transaction operation is already in progress."});
		if (GIsGameThreadIdInitialized) CheckGameThread();
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
		// Freeze eligibility before replay. A command can change the model that
		// other expiry predicates inspect, so every record uses the same initial selection.
		auto& ReplayIndices = Replay.AppliedRecords;
		std::vector<size_t> ExpiredIndices;
		ReplayIndices.reserve(Records.size());
		for (size_t Index = 0; Index < Records.size(); ++Index)
		{
			if (Origin != EPropertyChangeOrigin::Edit && Records[Index].HasExpired()) ExpiredIndices.push_back(Index);
			else ReplayIndices.push_back(Index);
		}
		if (ReplayIndices.empty() && !ExpiredIndices.empty()) return {};
		std::vector<DObject*> LifecycleTargets;
		std::vector<bool> SourceWasGarbage;
		for (size_t Index : ReplayIndices)
		{
			const auto* Whole = std::get_if<FTransaction::FWholeObjectData>(&Records[Index].Data);
			if (!Whole || !Whole->BeforeState || !Whole->AfterState) continue;
			const bool Before = Whole->BeforeState->Observation.ObjectInfo.bIsPendingKill;
			const bool After = Whole->AfterState->Observation.ObjectInfo.bIsPendingKill;
			if (Before == After) continue;
			auto* Object = GDObjectArray.Resolve(Whole->GetTarget().GetKey());
			if (!Object || Object->HasAnyInternalFlags(EObjectInternalFlags::BeginDestroyed | EObjectInternalFlags::FinishDestroyed)) continue;
			if (Object->IsTemplateObject()
				|| Object->IsGarbage() != (bUndo ? After : Before))
				return std::unexpected(FTransactionApplyError{.RecordIndex = Index, .Message = "The recorded object lifecycle no longer matches the replay source."});
			LifecycleTargets.push_back(Object);
			SourceWasGarbage.push_back(Object->IsGarbage());
		}
		const FScopedTransactionHistoryReplay ReplayHistory(Context.HistoryId, LifecycleTargets);
		// A missing snapshot target contributes no loads or callbacks in this direction.
		// Unlike an expired command, it may resolve again on a subsequent replay.
		std::vector<size_t> MissingTargets;
		std::erase_if(ReplayIndices, [&](size_t Index) {
			const auto* Whole = std::get_if<FWholeObjectData>(&Records[Index].Data);
			if (!Whole || Whole->GetTarget().Get()) return false;
			MissingTargets.push_back(Index);
			return true;
		});
		constexpr auto PieRejection = "Transaction replay cannot retain Play-in-Editor references.";
		if (auto Pie = InspectPieReferences(*this); !Pie)
			TransactionFault(Id, bUndo ? "Undo" : "Redo", Pie.error());
		else if (*Pie)
			return std::unexpected(FTransactionApplyError{.Message = PieRejection});
		if (auto Result = ValidateReplayRecords(ReplayIndices); !Result) return Result;
		for (size_t Index : ReplayIndices)
		{
			const bool bFirstRecord = Index == (bUndo ? ReplayIndices.back() : ReplayIndices.front());
			if (const auto* Array = std::get_if<FArrayData>(&Records[Index].Data); Array && bFirstRecord)
				if (auto Valid = Array->ValidateSourceRange(); !Valid)
					return std::unexpected(FTransactionApplyError{.RecordIndex = Index, .Message = Valid.error()});
			if (const auto* Whole = std::get_if<FWholeObjectData>(&Records[Index].Data))
				if (auto Valid = Whole->ValidateSource(bUndo); !Valid)
					return std::unexpected(FTransactionApplyError{.RecordIndex = Index, .Message = Valid.error()});
		}
		// Validate the complete destination graph before any replay callback can observe a write.
		std::unordered_map<DSceneComponent*, DSceneComponent*> Parents;
		for (size_t Index : ReplayIndices)
		{
			const auto* Whole = std::get_if<FWholeObjectData>(&Records[Index].Data);
			auto* Scene = Whole ? Cast<DSceneComponent>(Whole->GetTarget().Get()) : nullptr;
			if (!Scene) continue;
			const auto& Saved = (bUndo ? Whole->BeforeState : Whole->AfterState)->Observation;
			for (const auto& Tag : Saved.SerializedTaggedData)
			{
				if (!Tag.bReflected || Tag.Name != FName("AttachParent")) continue;
				const auto& Slots = Saved.SerializationContext->ReferenceSlots;
				const auto Slot = std::ranges::find_if(Slots, [&](const auto& Item) {
					return !Item.bWeak && Item.Offset >= Tag.Begin && Item.Offset < Tag.End;
				});
				if (Slot == Slots.end()) continue;
				FObjectMemoryReader Reader(FByteView(Saved.SerializedObject->Bytes).subspan(Slot->Offset, Tag.End - Slot->Offset), EArchivePurpose::Transaction);
				FObjectKey Key;
				Key.SerializeForSnapshot(Reader);
				const auto& References = Saved.SerializedObject->HardReferences;
				const auto Ref = std::ranges::find(References, Key, &FPersistentObjectRef::GetKey);
				Parents[Scene] = Ref == References.end() ? nullptr : Cast<DSceneComponent>(Ref->Get());
			}
		}
		for (const auto& Edge : Parents)
		{
			std::unordered_set<DSceneComponent*> Visited;
			for (auto* Node = Edge.first; Node;)
			{
				if (!Visited.insert(Node).second)
					return std::unexpected(FTransactionApplyError{.Message = "Restoring the recorded attachment would create a cycle."});
				const auto It = Parents.find(Node);
				Node = It == Parents.end() ? Node->GetAttachParent() : It->second;
			}
		}
		std::vector<FPersistentObjectRef> ReplayTargets;
		if (Origin != EPropertyChangeOrigin::Edit)
		{
			for (size_t Index : ReplayIndices)
			{
				for (DObject* Object : Records[Index].GetModifiedObjects())
				{
					if (!Object) continue;
					const FPersistentObjectRef Target(Object);
					if (!std::ranges::contains(ReplayTargets, Target)) ReplayTargets.push_back(Target);
				}
			}
		}
		struct FPreparedReplayScope
		{
			std::vector<FTransaction::FObjectRecord>& Records;
			~FPreparedReplayScope() { for (auto& Record : Records) Record.ClearPreparedReplay(); }
		} PreparedReplayScope{Records};
		for (const auto& Target : ReplayTargets)
			if (auto* Object = Target.Get()) Object->PreEditUndo();
		// Whole-object inverses precede every load. Array inverses use their ordered
		// splice step, since later operations can change the meaning of a range.
		for (size_t Step = 0; Step < ReplayIndices.size(); ++Step)
		{
			const size_t Index = ReplayIndices[Step];
			const auto* ObjectRecord = std::get_if<FTransaction::FWholeObjectData>(&Records[Index].Data);
			const auto Earlier = std::span(ReplayIndices).first(Step);
			const bool bOrderedInverse = ObjectRecord && std::ranges::any_of(Earlier, [&](size_t EarlierIndex) {
				const auto* Array = std::get_if<FTransaction::FArrayData>(&Records[EarlierIndex].Data);
				return Array && Array->GetTarget() == ObjectRecord->GetTarget();
			});
			if (ObjectRecord && Cast<DSceneComponent>(ObjectRecord->GetTarget().Get()))
			{
				const auto Prior = bUndo ? std::span(ReplayIndices).subspan(Step + 1) : Earlier;
				if (std::ranges::any_of(Prior, [&](size_t Other) {
					return std::holds_alternative<std::unique_ptr<FCommandChange>>(Records[Other].Data)
						&& std::ranges::contains(Records[Other].GetModifiedObjects(), ObjectRecord->GetTarget().Get());
				})) continue;
			}
			// A whole baseline captured after range edits belongs to that intermediate
			// state. Redo must prepare it after those edits, not from the entry start.
			if (bOrderedInverse) continue;
			if (auto Prepared = Records[Index].PrepareReplay(); !Prepared)
			{
				TransactionRecordFault(Id, bUndo ? "Undo" : "Redo", Index, Prepared.error());
			}
		}
		auto CheckPieReferences = [&](const auto& Source) -> FTransactionCaptureResult {
			const auto Pie = InspectPieReferences(Source);
			if (!Pie) return std::unexpected(Pie.error());
			if (*Pie) return std::unexpected(PieRejection);
			return {};
		};
		// Prepared whole-object inverses are history data too. Reject them before
		// loading; ordered array/whole inverses are checked at their actual step.
		if (auto Pie = CheckPieReferences(*this); !Pie)
		{
			TransactionFault(Id, bUndo ? "Undo" : "Redo", Pie.error());
		}
		auto SetLiveness = [](DObject& Object, bool bGarbage) -> bool {
			if (Object.IsGarbage() == bGarbage) return true;
			if (bGarbage) Object.MarkAsGarbage();
			else Object.ClearGarbage();
			return Object.IsGarbage() == bGarbage;
		};
		for (size_t Index = 0; Index < LifecycleTargets.size(); ++Index)
		{
			if (!SetLiveness(*LifecycleTargets[Index], !SourceWasGarbage[Index]))
			{
				TransactionFault(Id, bUndo ? "Undo" : "Redo", "Unable to apply the recorded object lifecycle.");
			}
		}
		for (size_t Step = 0; Step < ReplayIndices.size(); ++Step)
		{
			const size_t Index = ReplayIndices[bUndo ? ReplayIndices.size() - 1 - Step : Step];
			auto Application = Records[Index].Apply(bUndo, Origin);
			if (Application)
			{
				// Only this record gained an ordered inverse: avoid rescanning all
				// other records at every step (quadratic in transaction size).
				Application = CheckPieReferences(Records[Index]);
				if (Application && Step + 1 == ReplayIndices.size()) Application = CheckPieReferences(*this);
			}
			if (!Application)
				TransactionRecordFault(Id, bUndo ? "Undo" : "Redo", Index, Application.error());
		}
		for (size_t Index : ReplayIndices)
		{
			auto [Object, Annotation] = Records[Index].GetReplayAnnotation();
			if (!Object) continue;
			if (bUndo) Replay.Annotations.try_emplace(Object, std::move(Annotation));
			else Replay.Annotations.insert_or_assign(Object, std::move(Annotation));
		}
		auto Depth = [](const FPersistentObjectRef& Target) {
			size_t Result = 0;
			for (auto* Object = Target.Get(); Object && Result < 256; Object = Object->GetOuter()) ++Result;
			return Result;
		};
		// Complete inners before their owners so Actor reconstruction observes
		// settled component transforms and attachment reverse links.
		std::stable_sort(ReplayTargets.begin(), ReplayTargets.end(), [&](const auto& Left, const auto& Right) { return Depth(Left) > Depth(Right); });
		for (const auto& Target : ReplayTargets)
			if (DObject* Object = Target.Get())
			{
				const auto It = Replay.Annotations.find(Object);
				if (It != Replay.Annotations.end()) Object->PostEditUndo(It->second);
				else Object->PostEditUndo();
			}
		// UE Load flips eagerly because it has no checked failure result. Retain
		// the same prepared inverse, but publish it only after checked replay and
		// native completion succeed.
		for (size_t Index : ReplayIndices) Records[Index].CommitReplay(bUndo);
		if (Origin != EPropertyChangeOrigin::Edit)
			if (auto Result = DispatchObjectEvents(ETransactionObjectEventType::UndoRedo,
				bUndo ? ETransactionOperation::Undo : ETransactionOperation::Redo, nullptr, {}, &Replay); !Result)
			{
				TransactionFault(Id, bUndo ? "Undo" : "Redo", Result.error());
			}
		auto VisitPackages = [](const FTransaction::FObjectRecord& Record, const auto& Visit) {
			for (auto* Package : Record.GetAffectedPackages()) if (Package) Visit(Package);
			for (auto* Object : Record.GetModifiedObjects())
				if (auto* Package = Object ? Object->GetPackage() : nullptr) Visit(Package);
		};
		auto NoteSkippedPackages = [&](size_t Index) {
			VisitPackages(Records[Index], [&](DPackage* Package) { Replay.PackagesWithSkippedRecords.try_emplace(Package, false); });
		};
		for (size_t Index : ExpiredIndices) NoteSkippedPackages(Index);
		for (size_t Index : MissingTargets) NoteSkippedPackages(Index);
		const auto Operation = bUndo ? ETransactionOperation::Undo : ETransactionOperation::Redo;
		for (size_t Index : ReplayIndices)
		{
			const auto& Record = Records[Index];
			if (!Replay.PackagesWithSkippedRecords.empty())
				VisitPackages(Record, [&](DPackage* Package) {
					if (const auto It = Replay.PackagesWithSkippedRecords.find(Package); It != Replay.PackagesWithSkippedRecords.end()) It->second = true;
				});
			if (auto Details = Record.GetDetails(Operation); !Details.empty())
			{
				if (!Replay.Details.empty()) Replay.Details += " ";
				Replay.Details += std::move(Details);
			}
		}
		return {};
	}

	catch (const std::exception& Error) { TransactionFault(Id, bUndo ? "Undo" : "Redo", Error.what()); }
	catch (...) { TransactionFault(Id, bUndo ? "Undo" : "Redo", "unknown exception"); }

	auto FTransaction::GetDetails(ETransactionOperation Operation) const -> std::string
	{
		std::string Details;
		for (const FTransaction::FObjectRecord& Record : Records)
		{
			if (Operation != ETransactionOperation::Execute && Record.HasExpired()) continue;
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
		for (const FTransaction::FObjectRecord& Record : Records)
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
			if (StartIndex >= 0 && GUndo)
			{
				RecordingOwner = GEditor->GetTransactor();
				RecordingId = GUndo->GetContext().TransactionId;
			}
		}
	}
	auto FScopedTransaction::IsRecordingCurrent() const -> bool
	{
		return IsOutstanding() && GEditor && GUndo && RecordingOwner.Get() == GEditor->GetTransactor()
			&& GUndo->GetContext().TransactionId == RecordingId;
	}
	FScopedTransaction::~FScopedTransaction()
	{
		if (IsRecordingCurrent() && GEditor) GEditor->EndTransaction();
	}
	auto FScopedTransaction::Cancel() -> void
	{
		if (!IsOutstanding()) return;
		if (IsRecordingCurrent() && GEditor)
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
		const size_t RedoCount = History.size() - Cursor;
		const size_t First = RedoCount ? Cursor : 0;
		size_t Count = RedoCount;
		if (!RedoCount && MemoryBudget == 0) Count = Cursor;
		else if (!RedoCount)
		{
			FInspectionScope Inspect(Guard);
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
		RemovedTransactions.reserve(Count);
		for (size_t Index = First; Index < First + Count; ++Index)
			RemovedTransactions.push_back(std::move(History[Index]));
		History.erase(History.begin() + First, History.begin() + First + Count);
		PreviousUndoCount = RedoCount;
		History.push_back(std::move(Candidate));
		Pending = History.back().get();
		Cursor = History.size();

		ActiveCount = 1;
		GUndo = Pending;
		const auto Id = Pending->GetId();
		try
		{
			OnTransactionStateChanged().Broadcast(Pending->GetContext(), ETransactionStateEventType::TransactionStarted);
			if (!Pending || Pending->GetId() != Id) return -1;
			OnUndoBufferChanged().Broadcast();
		}
		catch (const std::exception& Error) { TransactionFault(Id, "Begin", Error.what()); }
		catch (...) { TransactionFault(Id, "Begin", "unknown exception"); }
		return Pending && Pending->GetId() == Id ? 0 : -1;
	}
	auto DTransBuffer::ReleaseRecording() -> void
	{
		if (Pending && Pending->OperationId != 0) (void)Pending->EndOperation();
		if (Pending) Pending->CaptureIndex.reset();
		if (Pending && GUndo == Pending) GUndo = nullptr;
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

	auto DTransBuffer::StoreUndo(DObject* Object, std::unique_ptr<FCommandChange> Change, std::string* OutError) -> bool
	try
	{
		CheckThread();
		if (!IsActive() || !CanTransact()) return FailWithReason(OutError, "Storing a custom change requires an active recording available for capture.");
		if (!Change) return FailWithReason(OutError, "The custom transaction change is unavailable.");
		FTransactionBusyScope BusyScope(Pending->bBusy);
		// Native participant and package callbacks may throw after mutating the model.
		FTransaction::FObjectRecord Record(std::move(Change), Object);
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
		if (Pending->RecordingFailure) TransactionFault(Id, "End", *Pending->RecordingFailure);
		if (GIsGameThreadIdInitialized) CheckGameThread();
		try
		{
			if (OnPreTransactionFinalized().IsBound() || OnTransactionStateChanged().IsBound())
			{
				Pending->bNotifyingPreFinalize = true;
				OnTransactionStateChanged().Broadcast(Pending->GetContext(), ETransactionStateEventType::PreTransactionFinalized);
				if (!Pending || Pending->GetId() != Id) return Result;
				OnPreTransactionFinalized().Broadcast(Pending->GetContext());
				// Cancel or Reset can retire the recording and a listener can start another.
				if (!Pending || Pending->GetId() != Id) return Result;
				Pending->bNotifyingPreFinalize = false;
			}
			if (--ActiveCount > 0) return Result;
			if (auto Captured = Pending->Finalize(); !Captured) TransactionFault(Id, "Finalize", Captured.error());
			Guard = EHistoryGuard::Notifying;
			OnTransactionStateChanged().Broadcast(Pending->GetContext(), ETransactionStateEventType::TransactionFinalized);
			Guard = EHistoryGuard::Finalizing;
			if (auto Closed = Pending->EndOperation(); !Closed) TransactionFault(Id, "EndOperation", Closed.error());
			Pending->CaptureIndex.reset();
			FinalizePending();
			FTransactionContext Context;
			Context.TransactionId = Id;
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
		if (Pending->RecordingFailure) TransactionFault(Id, "Cancel", *Pending->RecordingFailure);
		Guard = EHistoryGuard::Notifying;
		try
		{
			OnTransactionStateChanged().Broadcast(Pending->GetContext(), ETransactionStateEventType::TransactionCanceled);
			Guard = EHistoryGuard::Mutating;
			QueueEvent(ETransactionEventType::Discarded, *Pending, "Transaction recording canceled; applied edits remain.");
			if (auto Closed = Pending->EndOperation(); !Closed) TransactionFault(Id, "EndOperation", Closed.error());
			GUndo = nullptr;
			RestoreRecordingHistory();
			ActiveCount = 0;
		}
		catch (const std::exception& Error) { TransactionFault(Id, "Cancel", Error.what()); }
		catch (...) { TransactionFault(Id, "Cancel", "unknown exception"); }
	}

	auto DTransBuffer::RestoreRecordingHistory() -> void
	{
		auto Retired = std::move(History.back());
		History.pop_back();
		auto At = PreviousUndoCount ? History.end() : History.begin();
		History.insert(At, std::make_move_iterator(RemovedTransactions.begin()), std::make_move_iterator(RemovedTransactions.end()));
		RemovedTransactions.clear();
		Cursor = History.size() - PreviousUndoCount;
		PreviousUndoCount = 0;
		Guard = EHistoryGuard::Notifying;
		try { OnUndoBufferChanged().Broadcast(); }
		catch (const std::exception& Error) { TransactionFault(Retired->GetId(), "RestoreRecordingHistory", Error.what()); }
		catch (...) { TransactionFault(Retired->GetId(), "RestoreRecordingHistory", "unknown exception"); }
		Guard = EHistoryGuard::Finalizing;
		Retired->bBusy = true;
		if (GUndo == Retired.get()) GUndo = nullptr;
		Pending = nullptr;
		Retired.reset();
		Guard = EHistoryGuard::None;
	}

	auto DTransBuffer::FinalizePending() -> void
	{
		const FTransactionId TransactionId = Pending->GetId();
		const auto Pie = Pending->ContainsPieObjects();
		if (!Pie) TransactionFault(TransactionId, "Finalize", Pie.error());
		if (*Pie)
		{
			QueueEvent(ETransactionEventType::Discarded, *Pending, "Play-in-Editor references cannot enter persistent editor history.");
			return RestoreRecordingHistory();
		}
		Pending->RemoveNoOpRecords();
		if (Pending->GetRecordCount() == 0)
		{
			QueueEvent(ETransactionEventType::Discarded, *Pending, "The transaction contained no records.");
			return RestoreRecordingHistory();
		}
		Pending->PreparePackageRecords();
		FTransactionEvent Executed{.Type = ETransactionEventType::Executed, .Operation = ETransactionOperation::Execute,
			.Id = TransactionId, .Description = Pending->Context.Description, .Details = Pending->GetDetails(ETransactionOperation::Execute)};
		std::vector<FTransactionEvent> PreparedEvents;
		for (const auto& Removed : RemovedTransactions)
		{
			PreparedEvents.push_back({.Type = PreviousUndoCount ? ETransactionEventType::Discarded : ETransactionEventType::Evicted,
				.Operation = ETransactionOperation::Execute, .Id = Removed->GetId(), .Description = Removed->Context.Description,
				.Details = PreviousUndoCount ? "A new commit replaced the redo branch." : ""});
		}
		PreparedEvents.push_back(std::move(Executed));
		if (PreparedEvents.size() > Events.max_size() - Events.size()) throw std::length_error("Transaction event capacity exceeded.");
		Events.reserve(Events.size() + PreparedEvents.size());
		if (PreviousUndoCount)
			for (auto& Barrier : UndoBarriers) Barrier = std::min(Barrier, Cursor - 1);
		else RemapUndoBarriers(0, RemovedTransactions.size());
		PreviousUndoCount = 0;
		ReleaseRecording();
		RemovedTransactions.clear();
		Pending->RecordingOwner = nullptr;
		for (const auto& Record : History.back()->PackageRecords)
			if (auto* Package = Cast<DPackage>(Record.Package.Resolve())) Package->MarkDirty();
		for (auto& Event : PreparedEvents) Events.push_back(std::move(Event));
		Pending = nullptr;
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
		if (!CanReplayInHost(OutError)) return false;
		if (!CanAccessReplayHistory())
			return FailWithReason(OutError, bUndo ? "Undo requires an idle transactor." : "Redo requires an idle transactor.");
		const size_t UndoLimit = UndoBarriers.empty() ? 0 : UndoBarriers.back();
		if (bUndo && Cursor != 0 && Cursor <= UndoLimit)
			return FailWithReason(OutError, "An undo barrier blocks earlier history.");
		if (bUndo ? Cursor == 0 : Cursor == History.size())
			return FailWithReason(OutError, bUndo ? "Nothing to undo." : "Nothing to redo.");
		auto ReplayId = History[bUndo ? Cursor - 1 : Cursor]->GetId();
		if (ExpectedId && *ExpectedId != ReplayId)
			return FailWithReason(OutError, bUndo ? "The expected transaction is not the Undo head." : "The expected transaction is not the Redo head.");

		Guard = EHistoryGuard::Mutating;
		const auto Operation = bUndo ? ETransactionOperation::Undo : ETransactionOperation::Redo;
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
					Guard = EHistoryGuard::None;
					return FailWithReason(OutError, "The expected transaction has expired.");
				}
				if (!bExpired)
				{
					FTransaction::FReplayResult Outcome;
					auto InversePackages = Transaction.PackageRecords;
					for (auto& Inverse : InversePackages)
						if (auto* Package = Cast<DPackage>(Inverse.Package.Resolve()))
						{
							Inverse.bWasDirty = Package->IsDirty();
							Inverse.SaveCount = GetTransactionPackageSaveCount(*Package);
						}
					if (auto Applied = Transaction.Apply(bUndo, bUndo ? EPropertyChangeOrigin::Undo : EPropertyChangeOrigin::Redo, Outcome); !Applied)
					{
						auto& Error = Applied.error();
						Cursor = PreviousCursor;
						QueueEvent(ETransactionEventType::Failed, Transaction, Error.Message, Operation);
						Guard = EHistoryGuard::None;
						return FailWithReason(OutError, Error.Message);
					}
					for (size_t Slot = 0; Slot < Transaction.PackageRecords.size(); ++Slot)
					{
						auto& Record = Transaction.PackageRecords[Slot];
						auto* Package = Cast<DPackage>(Record.Package.Resolve());
						if (!Package || Outcome.AppliedRecords.empty()) continue;
						if (const auto It = Outcome.PackagesWithSkippedRecords.find(Package); It != Outcome.PackagesWithSkippedRecords.end())
						{
							if (It->second)
							{
								Record.bWasDirty = true;
								Package->MarkDirty();
							}
							continue;
						}
						const bool bDirty = Record.bWasDirty || Record.SaveCount != GetTransactionPackageSaveCount(*Package);
						Record = InversePackages[Slot];
						if (bDirty) Package->MarkDirty();
						else Package->ClearDirty();
					}
					QueueEvent(bUndo ? ETransactionEventType::Undone : ETransactionEventType::Redone, Transaction, std::move(Outcome.Details), Operation);
				}
				if (!bCanRedo)
				{
					for (size_t Slot = Cursor; Slot < History.size(); ++Slot)
						QueueEvent(ETransactionEventType::Discarded, *History[Slot], "Undo without Redo.");
					Guard = EHistoryGuard::Finalizing;
					History.erase(History.begin() + Cursor, History.end());
					for (auto& Barrier : UndoBarriers) Barrier = std::min(Barrier, Cursor);
				}
				if (!bExpired)
				{
					Guard = EHistoryGuard::None;
					return true;
				}
			} while (bUndo ? Cursor > UndoLimit : Cursor < History.size());
			Guard = EHistoryGuard::None;
			return false;
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
		if (Pending && Pending->RecordingFailure) TransactionFault(Pending->GetId(), "Reset", *Pending->RecordingFailure);
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
		Events.clear();
		Pending = nullptr; PreviousUndoCount = 0;
		RemovedTransactions.clear(); RetiredHistory.clear();
		ResetReason = std::move(Reason);
		Guard = EHistoryGuard::None;
	}

	auto DTransBuffer::DiscardCustomChangesByModule(std::string_view ModuleName) -> bool
	{
		CheckThread();
		if (ModuleName.empty() || !IsHistoryIdle()) return false;
		FInspectionScope Inspect(Guard);
		for (size_t Index = History.size(); Index-- > 0;)
		{
			if (!History[Index]->IsOwnedByModule(ModuleName)) continue;
			for (auto* Package : History[Index]->GetAffectedPackages())
				if (IsValid(Package) && Package->IsAssetPackage()) Package->MarkDirty();
			QueueEvent(ETransactionEventType::Discarded, *History[Index], "Owning module is unloading.");
			RemapUndoBarriers(Index, 1);
			History.erase(History.begin() + Index);
			if (Index < Cursor) --Cursor;
		}
		return true;
	}

	auto DTransBuffer::DispatchEvents() -> void
	{
		CheckThread();
		if (!IsHistoryIdle()) return;
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
		FInspectionScope Inspect(Guard);
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
		if (Pending && (History.empty() || History.back().get() != Pending)) Pending->AddReferencedObjects(Collector);
		for (const auto& Transaction : RemovedTransactions) Transaction->AddReferencedObjects(Collector);
		for (const auto& Transaction : History) Transaction->AddReferencedObjects(Collector);
		Collector.AllowEliminatingReferences(true);
	}

	auto DTransBuffer::BeginDestroy() -> void
	{
		CheckThread();
		Guard = EHistoryGuard::Mutating;
		ReleaseRecording();
		Pending = nullptr;
		RemovedTransactions.clear();
		History.clear();
		Events.clear();
		Cursor = 0;
		DTransactor::BeginDestroy();
	}

	auto DTransBuffer::GetUndoSize() const -> size_t
	{
		CheckThread();
		if (History.empty()) return 0;
		if ((Guard != EHistoryGuard::None && Guard != EHistoryGuard::Notifying) || (Pending && Pending->IsBusy()))
			return std::numeric_limits<size_t>::max();
		FInspectionScope Inspect(Guard);
		std::optional<FTransactionBusyScope> Busy;
		if (Pending) Busy.emplace(Pending->bBusy);
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
		if (!bCheckWhetherUndoPossible && Pending && !History.empty() && History.back().get() == Pending)
			return Pending->GetContext();
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
		FInspectionScope Inspect(Guard);
		for (size_t Index = History.size(); Index-- > 0;)
		{
			if (!History[Index]->ReferencesPackage(Package)) continue;
			for (auto* Affected : History[Index]->GetAffectedPackages())
				if (IsValid(Affected) && Affected != &Package && Affected->IsAssetPackage()) Affected->MarkDirty();
			QueueEvent(ETransactionEventType::Discarded, *History[Index], "Referenced package is being retired.");
			RemapUndoBarriers(Index, 1);
			History.erase(History.begin() + Index);
			if (Index < Cursor) --Cursor;
		}
	}

	class FTransactorReloadParticipant final : public IPackageReloadParticipant
	{
	public:
		FTransactorReloadParticipant(DTransBuffer& InTransactor, std::span<DPackage* const> InPackages)
			: Transactor(InTransactor), Packages(InPackages.begin(), InPackages.end()) {}

		auto Prepare(const FObjectReplacementMap& InMap) -> std::expected<void, FObjectReplacementError> override
		{
			Abort();
			if (!Transactor.IsHistoryIdle())
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
			for (auto* Object : Map->GetPreparedObjects())
				if (!Object->IsTemplateObject() || std::ranges::any_of(Map->GetEntries(), [&](const auto& Entry) {
					return Entry.Replacement == Object && Entry.Previous->HasAnyObjectFlags(EObjectFlags::Transactional);
				})) Object->SetFlags(EObjectFlags::Transactional);
			return {};
		}
		auto Validate() const -> bool override { return Map && Transactor.IsHistoryIdle(); }
		auto CoversNativeReferences(const DObject& Owner) const -> bool override { return Map && &Owner == &Transactor; }
		auto GetStrongReferenceCount(const DObject&) const -> uint32 override { return 0; }
		auto Commit() noexcept -> void override
		{
			check(Transactor.IsHistoryIdle());
			Transactor.Guard = DTransBuffer::EHistoryGuard::Mutating;
		}
		auto OnObjectsReinstanced(const FObjectReplacementMap& Mapping) noexcept -> void override
		{
			auto& History = Transactor.History;
			std::vector<DPackage*> DirtyPackages;
			for (size_t Index = 0; Index < History.size();)
			{
				auto& Entry = History[Index];
				std::string Failure;
				std::vector<DPackage*> OriginalPackages;
				try
				{
					if (!Mapping.Find(Entry->PrimaryObject.Resolve())
						&& !std::ranges::any_of(Entry->PackageRecords, [&](const auto& R) { return Mapping.Find(R.Package.Resolve()); })
						&& !std::ranges::any_of(Entry->Records, [&](const auto& R) { return R.HasReplacementReferences(Mapping); }))
					{
						++Index;
						continue;
					}
					OriginalPackages = Entry->GetAffectedPackages();
					auto Result = ReinstanceTransaction(*Entry, Mapping);
					if (Result)
					{
						++Index;
						continue;
					}
					Failure = Result.error();
				}
				catch (const std::bad_alloc&) { std::terminate(); }
				catch (const std::exception& Error) { Failure = Error.what(); }
				catch (...) { Failure = "Transaction replacement hook threw."; }

				Transactor.QueueEvent(ETransactionEventType::Discarded, *Entry, std::move(Failure));
				for (auto* Package : OriginalPackages)
					if (Package && !Mapping.Find(Package) && !std::ranges::contains(DirtyPackages, Package)) DirtyPackages.push_back(Package);
				Retired.push_back(std::move(Entry));
				History.erase(History.begin() + Index);
				Transactor.RemapUndoBarriers(Index, 1);
				if (Index < Transactor.Cursor) --Transactor.Cursor;
			}
			for (auto* Package : DirtyPackages) Package->MarkDirty();
			Transactor.Guard = DTransBuffer::EHistoryGuard::None;
		}
		auto Abort() noexcept -> void override { Map = nullptr; }
		auto CanRetire() const -> bool override
		{
			Retired.clear();
			return true;
		}
	private:
		auto ReinstanceTransaction(FTransaction& Transaction, const FObjectReplacementMap& Mapping) -> FTransactionCaptureResult
		{
			auto Primary = Transaction.PrimaryObject.CreateReplacement(Mapping);
			if (!Primary) return std::unexpected(Primary.error());
			Transaction.PrimaryObject = std::move(*Primary);
			for (auto& Transition : Transaction.PackageRecords)
			{
				auto Key = Mapping.RemapObjectKey(Transition.Package.GetKey());
				if (!Key) return std::unexpected(ToString(Key.error()));
				if (*Key != Transition.Package.GetKey()) Transition.bWasDirty = true;
				Transition.Package = FPersistentObjectRef::FromKey(*Key);
			}
			for (auto& Record : Transaction.Records)
			{
				if (Record.HasReplacementReferences(Mapping))
					if (auto Result = Record.Reinstance(Mapping); !Result) return Result;
				if (auto Valid = Record.Validate(); !Valid) return Valid;
				if (Record.HasReplacementReferences(Mapping))
					return std::unexpected("Reinstanced history retained an old-generation reference.");
			}
			return {};
		}

		DTransBuffer& Transactor;
		std::vector<DPackage*> Packages;
		const FObjectReplacementMap* Map = nullptr;
		mutable std::vector<std::unique_ptr<FTransaction>> Retired;
	};

	auto CreateTransactorReloadParticipant(DTransactor& Transactor,
		std::span<DPackage* const> Packages)
		-> std::shared_ptr<IPackageReloadParticipant>
	{
		auto* Buffer = Cast<DTransBuffer>(&Transactor);
		if (!Buffer) return {};
		return std::make_shared<FTransactorReloadParticipant>(*Buffer, Packages);
	}

	auto DTransBuffer::OnObjectsReinstanced(const FObjectReplacementMap& Map) -> FTransactionCaptureResult
	{
		CheckGameThread();
		if (Map.GetEntries().empty()) return {};
		if (!Map.ValidateIdentities()) return std::unexpected("Reinstancing map contains expired object identities.");
		auto Participant = CreateTransactorReloadParticipant(*this, {});
		try
		{
			auto Prepared = Participant->Prepare(Map);
			if (!Prepared) { Participant->Abort(); return std::unexpected(ToString(Prepared.error())); }
			if (!Map.ValidateIdentities() || !Participant->Validate())
			{ Participant->Abort(); return std::unexpected("Prepared reinstancing history became stale."); }
		}
		catch (const std::exception& Error) { Participant->Abort(); return std::unexpected(std::string(Error.what())); }
		catch (...) { Participant->Abort(); return std::unexpected("Reinstancing preparation threw."); }
		Participant->Commit();
		Participant->OnObjectsReinstanced(Map);
		verify(Participant->CanRetire());
		return {};
	}

	auto DTransBuffer::QueueEvent(
		ETransactionEventType Type,
		const FTransaction& Transaction,
		std::optional<std::string> Details,
		ETransactionOperation Operation) -> void
	{
		Events.push_back({
			.Type = Type,
			.Operation = Operation,
			.Id = Transaction.GetId(),
			.Description = Transaction.GetContext().Description,
			.Details = Details ? std::move(*Details) : Transaction.GetDetails(Operation),
		});
	}
}
