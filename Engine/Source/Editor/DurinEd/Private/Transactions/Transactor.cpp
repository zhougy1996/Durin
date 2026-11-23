#include "Transactions/Transactor.h"

#include "Editor/EditorEngine.h"
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
		constexpr char RecoveryGuidance[] = "History is disabled. Recover affected data and reload the editor session.";
		struct FTransactionBusyScope
		{
			bool& Busy;
			explicit FTransactionBusyScope(bool& Value) : Busy(Value) { Busy = true; }
			~FTransactionBusyScope() { Busy = false; }
		};
		struct FTransactionInspectionScope
		{
			ETransactorState& State;
			ETransactorState Previous;
			explicit FTransactionInspectionScope(ETransactorState& InState)
				: State(InState), Previous(std::exchange(InState, ETransactorState::Inspecting)) {}
			~FTransactionInspectionScope() { State = Previous; }
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

	auto FTransaction::FObjectRecord::CreateReplacement(const FObjectReplacementMap& Map) const -> std::expected<FTransaction::FObjectRecord, std::string>
	{
		if (const auto* Array = std::get_if<FTransaction::FArrayData>(&Data))
		{
			auto Result = Array->CreateReplacement(Map);
			if (!Result) return std::unexpected(Result.error());
			FTransaction::FObjectRecord Replacement(std::move(*Result));
			Replacement.bReplayExpired = bReplayExpired;
			return Replacement;
		}
		if (const auto* Object = std::get_if<FTransaction::FWholeObjectData>(&Data))
		{
			auto Result = Object->CreateReplacement(Map);
			if (!Result) return std::unexpected(Result.error());
			FTransaction::FObjectRecord Replacement(std::move(*Result));
			Replacement.bReplayExpired = bReplayExpired;
			return Replacement;
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
		FTransaction::FObjectRecord Result(std::move(*Command), ResolveObjectKey(*Object));
		Result.bReplayExpired = bReplayExpired;
		std::vector<FPersistentObjectRef> Expected;
		for (const auto& Ref : ModifiedObjects.GetReferences())
		{
			auto Reference = Ref.CreateReplacement(Map);
			if (!Reference) return std::unexpected(Reference.error());
			Expected.push_back(std::move(*Reference));
		}
		if (!std::ranges::is_permutation(Expected, Result.ModifiedObjects.GetReferences()))
			return std::unexpected("Command reinstancing changed its declared modified participants.");
		std::vector<DPackage*> ExpectedPackages;
		for (auto* Package : GetAffectedPackages())
		{
			auto Key = Map.RemapObjectKey(FObjectKey(Package));
			if (!Key) return std::unexpected(ToString(Key.error()));
			ExpectedPackages.push_back(Cast<DPackage>(ResolveObjectKey(*Key)));
		}
		if (!std::ranges::is_permutation(ExpectedPackages, Result.GetAffectedPackages()))
			return std::unexpected("Command reinstancing changed its declared affected packages.");
		return Result;
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
		return bReplayExpired || std::visit([this](const auto& Record) -> bool {
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
		for (const auto& Reference : ModifiedObjects.GetReferences())
			if (!Reference.Get()) return std::unexpected("A modified transaction participant is unavailable.");
		return std::visit([&](const auto& Record) -> FTransactionCaptureResult {
			using T = std::decay_t<decltype(Record)>;
			if constexpr (std::is_same_v<T, FTransaction::FWholeObjectData>)
			{
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

	auto FTransaction::FObjectRecord::Apply(bool bBefore, EPropertyChangeOrigin Origin, bool* OutRecoveryRequired, bool bUseHistoricalState) -> FTransactionCaptureResult
	{
		if (OutRecoveryRequired) *OutRecoveryRequired = false;
		return std::visit([&](auto& Record) -> FTransactionCaptureResult {
			using T = std::decay_t<decltype(Record)>;
			if constexpr (std::is_same_v<T, FTransaction::FWholeObjectData>)
			{
				try
				{
					const auto Result = Record.Apply(bBefore, Origin, bUseHistoricalState);
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
			else if constexpr (std::is_same_v<T, FTransaction::FArrayData>)
			{
				try { return Record.Apply(!bUseHistoricalState, Origin, OutRecoveryRequired); }
				catch (const std::exception& Error) { return std::unexpected(std::format("Array transaction replay threw: {}", Error.what())); }
				catch (...) { return std::unexpected("Array transaction replay threw an unknown exception."); }
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
			|| (RecordingOwner && (GUndo != this || RecordingOwner->State != ETransactorState::Recording)))
			return std::unexpected("Object snapshots require an active recording outside transaction callbacks.");
		FTransactionCaptureResult Result;
		{
			if (GIsGameThreadIdInitialized) CheckGameThread();
			FTransactionBusyScope BusyScope(bBusy);
			if (const size_t Position = FindWholeRecord(Object))
				Result = DispatchObjectEvents(ETransactionObjectEventType::Snapshot, ETransactionOperation::Execute,
					&std::get<FTransaction::FWholeObjectData>(Records[Position - 1].Data), Properties);
		}
		if (!Result && bObserverRecoveryRequired && RecordingOwner)
			(void)RecordingOwner->HandleApplyFailure(*this, {.Status = ETransactionApplyFailure::RecoveryRequired, .Message = Result.error()});
		return Result;
	}
	auto FTransaction::SaveArray(const FTransactionArrayCapture& Capture) -> FTransactionCaptureResult
	{
		if (GIsGameThreadIdInitialized && !IsInGameThread()) return std::unexpected("Array capture requires the GameThread.");
		if (bBusy || bFinalized || OperationId == 0 || (RecordingOwner && (GUndo != this
			|| RecordingOwner->State != ETransactorState::Recording)))
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
			if (RecordingOwner && Capture.Count)
				if (auto* Package = Capture.Object->GetPackage(); Package && Package->IsAssetPackage()) RecordingOwner->EnsurePackageState(*Package);
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

	auto FTransaction::InvalidateRecording(std::string Reason, DPackage* Package) -> void
	{
		if (!RecordingFailure) RecordingFailure = std::make_unique<FRecordingFailure>(std::move(Reason));
		if (Package && Package->IsAssetPackage())
		{
			const FObjectKey Key(Package);
			if (!std::ranges::contains(RecordingFailure->Packages, Key)) RecordingFailure->Packages.push_back(Key);
		}
	}

	auto FTransaction::SaveObject(DObject* Object) -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (bBusy || bFinalized || (RecordingOwner && (RecordingOwner->State != ETransactorState::Recording || GUndo != this)))
			return;
		if (!IsValid(Object)) return InvalidateRecording("Transaction object is unavailable for capture.", nullptr);
		auto* Package = Object->GetPackage();
		if (RecordingFailure) return InvalidateRecording({}, Package);
		FTransactionBusyScope BusyScope(bBusy);
		if (RecordingOwner && Package && Package->IsAssetPackage()) RecordingOwner->EnsurePackageState(*Package);
		const auto& Index = GetCaptureIndex();
		const auto It = Index.find(FPersistentObjectRef::FromKey(FObjectKey(Object)));
		if (It != Index.end() && It->second.bCustomOwner) return;
		if (FindWholeRecord(Object)) return;
		auto Record = FTransaction::FWholeObjectData::CaptureWhole(*Object);
		if (!Record) return InvalidateRecording(FormatTransactionObjectRecordError(Record.error()), Package);
		AppendRecord(FTransaction::FObjectRecord(std::move(*Record)));
	}

	auto FTransaction::Finalize() -> FTransactionCaptureResult
	{
		if (RecordingFailure) return std::unexpected(RecordingFailure->Reason);
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
		for (auto& Record : Records)
			if (auto Result = Record.CaptureAfter(); !Result)
			{
				InvalidateRecording(Result.error(), nullptr);
				return Result;
			}
		if (auto Result = DispatchObjectEvents(ETransactionObjectEventType::Finalized, ETransactionOperation::Execute); !Result) return Result;
		for (auto& Record : Records) if (auto* ObjectRecord = std::get_if<FTransaction::FWholeObjectData>(&Record.Data)) ObjectRecord->ClearSnapshot();
		bFinalized = true;
		CaptureIndex.reset();
		return {};
	}

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
				if (!Object) { bObserverRecoveryRequired |= bDispatched; return std::unexpected("Transaction event target became unavailable."); }
				if (Replay && Event.CreatedBy == ETransactionObjectChangeCreatedBy::TransactionRecord)
					if (const auto It = Replay->Annotations.find(Object); It != Replay->Annotations.end()) Event.Annotation = It->second;
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

	auto FTransaction::RetireExpiredRecords() -> void
	{
		for (auto& Record : Records) Record.bReplayExpired = true;
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
				return std::unexpected(FTransactionApplyError{.Status = ETransactionApplyFailure::ValidationFailed,
					.RecordIndex = Index, .Message = Result.error()});
		return {};
	}

	auto FTransaction::Apply(bool bUndo, EPropertyChangeOrigin Origin) -> FTransactionApplyResult
	{
		FReplayResult Replay;
		return Apply(bUndo, Origin, Replay);
	}

	auto FTransaction::Apply(bool bUndo, EPropertyChangeOrigin Origin, FReplayResult& Replay) -> FTransactionApplyResult
	{
		if (RecordingFailure) return std::unexpected(FTransactionApplyError{.Message = RecordingFailure->Reason});
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
		bool bInspectionFailed = false;
		auto CheckPieReferences = [&](const auto& Source) -> FTransactionCaptureResult {
			const auto Pie = InspectPieReferences(Source);
			if (!Pie) { bInspectionFailed = true; return std::unexpected(Pie.error()); }
			if (*Pie) return std::unexpected("Transaction replay cannot retain Play-in-Editor references.");
			return {};
		};
		// Freeze eligibility before replay. A command can change the model that
		// other expiry predicates inspect, and rollback must use the same plan.
		auto& ReplayIndices = Replay.AppliedRecords;
		std::vector<size_t> ExpiredIndices;
		ReplayIndices.reserve(Records.size());
		for (size_t Index = 0; Index < Records.size(); ++Index)
		{
			if (Origin != EPropertyChangeOrigin::Edit && Records[Index].HasExpired()) ExpiredIndices.push_back(Index);
			else ReplayIndices.push_back(Index);
		}
		if (ReplayIndices.empty() && !ExpiredIndices.empty())
		{
			RetireExpiredRecords();
			return {};
		}
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
			if (!Object || Object->IsTemplateObject() || Object->HasAnyInternalFlags(EObjectInternalFlags::BeginDestroyed)
				|| Object->IsGarbage() != (bUndo ? After : Before))
				return std::unexpected(FTransactionApplyError{.Status = ETransactionApplyFailure::ValidationFailed,
					.RecordIndex = Index, .Message = "The recorded object lifecycle no longer matches the replay source."});
			LifecycleTargets.push_back(Object);
			SourceWasGarbage.push_back(Object->IsGarbage());
		}
		const FScopedTransactionHistoryReplay ReplayHistory(Context.HistoryId, LifecycleTargets);
		if (auto Pie = CheckPieReferences(*this); !Pie)
			return std::unexpected(FTransactionApplyError{.Message = Pie.error()});
		if (auto Result = ValidateReplayRecords(ReplayIndices); !Result) return Result;
		for (size_t Index : ReplayIndices)
			if (const auto* Whole = std::get_if<FWholeObjectData>(&Records[Index].Data))
				if (auto Valid = Whole->ValidateSource(bUndo); !Valid)
					return std::unexpected(FTransactionApplyError{.Status = ETransactionApplyFailure::ValidationFailed,
						.RecordIndex = Index, .Message = Valid.error()});
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
					return std::unexpected(FTransactionApplyError{.Status = ETransactionApplyFailure::ValidationFailed,
						.Message = "Restoring the recorded attachment would create a cycle."});
				const auto It = Parents.find(Node);
				Node = It == Parents.end() ? Node->GetAttachParent() : It->second;
			}
		}
		std::vector<size_t> Applied;
		Applied.reserve(ReplayIndices.size());
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
		size_t StartedTargets = 0;
		auto NotifyReplayTargets = [&]() -> std::string {
			std::string Failure;
			for (size_t Index : ReplayIndices)
			{
				auto [Object, Annotation] = Records[Index].GetReplayAnnotation();
				if (!Object) continue;
				if (bUndo) Replay.Annotations.try_emplace(Object, std::move(Annotation));
				else Replay.Annotations.insert_or_assign(Object, std::move(Annotation));
			}
			auto CompletionTargets = std::vector<FPersistentObjectRef>(ReplayTargets.begin(), ReplayTargets.begin() + StartedTargets);
			auto Depth = [](const FPersistentObjectRef& Target) {
				size_t Result = 0;
				for (auto* Object = Target.Get(); Object && Result < 256; Object = Object->GetOuter()) ++Result;
				return Result;
			};
			// Complete inners before their owners so Actor reconstruction observes
			// settled component transforms and attachment reverse links.
			std::stable_sort(CompletionTargets.begin(), CompletionTargets.end(), [&](const auto& Left, const auto& Right) { return Depth(Left) > Depth(Right); });
			for (const auto& Target : CompletionTargets)
				if (DObject* Object = Target.Get())
					try
					{
						const auto It = Replay.Annotations.find(Object);
						if (It != Replay.Annotations.end()) Object->PostEditUndo(It->second);
						else Object->PostEditUndo();
					}
					catch (const std::exception& Error) { Failure += std::format(" PostEditUndo threw: {}", Error.what()); }
					catch (...) { Failure += " PostEditUndo threw an unknown exception."; }
			return Failure;
		};
		bool bRetainRecoveryState = false;
		struct FPreparedReplayScope
		{
			std::vector<FTransaction::FObjectRecord>& Records;
			bool& bRetain;
			~FPreparedReplayScope() { if (!bRetain) for (auto& Record : Records) Record.ClearPreparedReplay(); }
		} PreparedReplayScope{Records, bRetainRecoveryState};
		auto CompleteFailedReplay = [&](FTransactionApplyError Error) -> FTransactionApplyResult {
			if (bInspectionFailed) Error.Status = ETransactionApplyFailure::RecoveryRequired;
			if (auto Failure = NotifyReplayTargets(); !Failure.empty())
			{
				Error.Message += Failure;
				Error.Status = ETransactionApplyFailure::RecoveryRequired;
			}
			if (Error.Status != ETransactionApplyFailure::RecoveryRequired)
			{
				// A clean rollback no longer needs transient inverses. Inspect the
				// retained sides after completion, without mistaking the rejected
				// PIE inverse for a newly tainted history reference.
				for (auto& Record : Records) Record.ClearPreparedReplay();
				const auto Pie = InspectPieReferences(*this);
				if (!Pie)
				{
					Error.Message += " " + Pie.error();
					Error.Status = ETransactionApplyFailure::RecoveryRequired;
				}
				else if (*Pie)
				{
					Replay.bDiscardHistory = true;
					Error.Message += " Restored history contains Play-in-Editor references.";
				}
			}
			bRetainRecoveryState = Error.Status == ETransactionApplyFailure::RecoveryRequired;
			return std::unexpected(std::move(Error));
		};
		for (const auto& Target : ReplayTargets)
		{
			++StartedTargets;
			std::string Failure;
			if (auto* Object = Target.Get())
				try { Object->PreEditUndo(); }
				catch (const std::exception& Error) { Failure = std::format("PreEditUndo threw: {}", Error.what()); }
				catch (...) { Failure = "PreEditUndo threw an unknown exception."; }
			if (!Failure.empty())
			{
				return CompleteFailedReplay({.Status = ETransactionApplyFailure::RecoveryRequired,
					.Message = std::move(Failure)});
			}
		}
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
				return CompleteFailedReplay({.RecordIndex = Index, .Message = Prepared.error()});
			}
		}
		// Prepared whole-object inverses are history data too. Reject them before
		// loading; ordered array/whole inverses are checked at their actual step.
		if (auto Pie = CheckPieReferences(*this); !Pie)
		{
			return CompleteFailedReplay({.Message = Pie.error()});
		}
		auto SetLiveness = [](DObject& Object, bool bGarbage) -> bool {
			if (Object.IsGarbage() == bGarbage) return true;
			try
			{
				if (bGarbage) Object.MarkAsGarbage();
				else Object.ClearGarbage();
			}
			catch (...) { return false; }
			return Object.IsGarbage() == bGarbage;
		};
		auto RestoreLiveness = [&](size_t Count) {
			bool bClean = true;
			while (Count > 0)
			{
				--Count;
				bClean &= SetLiveness(*LifecycleTargets[Count], SourceWasGarbage[Count]);
			}
			return bClean;
		};
		for (size_t Index = 0; Index < LifecycleTargets.size(); ++Index)
		{
			if (!SetLiveness(*LifecycleTargets[Index], !SourceWasGarbage[Index]))
			{
				const bool bClean = RestoreLiveness(Index + 1);
				return CompleteFailedReplay({.Status = bClean ? ETransactionApplyFailure::Restored : ETransactionApplyFailure::RecoveryRequired,
					.Message = "Unable to apply the recorded object lifecycle."});
			}
		}
		for (size_t Step = 0; Step < ReplayIndices.size(); ++Step)
		{
			const size_t Index = ReplayIndices[bUndo ? ReplayIndices.size() - 1 - Step : Step];
			bool bRecordRecoveryRequired = false;
			auto Application = Records[Index].Apply(bUndo, Origin, &bRecordRecoveryRequired);
			if (Application)
			{
				Applied.push_back(Index);
				// Only this record gained an ordered inverse: avoid rescanning all
				// other records at every step (quadratic in transaction size).
				Application = CheckPieReferences(Records[Index]);
				if (Application && Step + 1 == ReplayIndices.size()) Application = CheckPieReferences(*this);
			}
			if (!Application)
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
				if (!RestoreLiveness(LifecycleTargets.size()) || bRecordRecoveryRequired || !Error.RollbackFailures.empty())
				{ Error.Status = ETransactionApplyFailure::RecoveryRequired; bRetainRecoveryState = true; }
				return CompleteFailedReplay(std::move(Error));
			}
		}
		if (auto Failure = NotifyReplayTargets(); !Failure.empty())
		{
			bRetainRecoveryState = true;
			return std::unexpected(FTransactionApplyError{.Status = ETransactionApplyFailure::RecoveryRequired,
				.Message = std::move(Failure)});
		}
		if (auto Pie = InspectPieReferences(*this); !Pie)
		{
			bRetainRecoveryState = true;
			return std::unexpected(FTransactionApplyError{.Status = ETransactionApplyFailure::RecoveryRequired, .Message = Pie.error()});
		}
		else if (*Pie) Replay.bDiscardHistory = true;
		// UE Load flips eagerly because it has no checked failure result. Retain
		// the same prepared inverse, but publish it only after checked replay and
		// native completion succeed, so clean compensation cannot rewrite history.
		for (size_t Index : ReplayIndices) Records[Index].CommitReplay(bUndo);
		for (size_t Index : ExpiredIndices) Records[Index].bReplayExpired = true;
		if (Origin != EPropertyChangeOrigin::Edit)
			if (auto Result = DispatchObjectEvents(ETransactionObjectEventType::UndoRedo,
				bUndo ? ETransactionOperation::Undo : ETransactionOperation::Redo, nullptr, {}, &Replay); !Result)
			{
				bRetainRecoveryState = true;
				return std::unexpected(FTransactionApplyError{.Status = ETransactionApplyFailure::RecoveryRequired, .Message = Result.error()});
			}
		if (auto Pie = InspectPieReferences(*this); !Pie)
		{
			bRetainRecoveryState = true;
			return std::unexpected(FTransactionApplyError{.Status = ETransactionApplyFailure::RecoveryRequired, .Message = Pie.error()});
		}
		else if (*Pie) Replay.bDiscardHistory = true;
		auto VisitPackages = [](const FTransaction::FObjectRecord& Record, const auto& Visit) {
			for (auto* Package : Record.GetAffectedPackages()) if (Package) Visit(Package);
			for (auto* Object : Record.GetModifiedObjects())
				if (auto* Package = Object ? Object->GetPackage() : nullptr) Visit(Package);
		};
		for (size_t Index : ExpiredIndices)
			VisitPackages(Records[Index], [&](DPackage* Package) { Replay.PackagesWithSkippedRecords.try_emplace(Package, false); });
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

	auto FTransaction::GetDetails(ETransactionOperation Operation) const -> std::string
	{
		std::string Details;
		for (const FTransaction::FObjectRecord& Record : Records)
		{
			if (Operation != ETransactionOperation::Execute && Record.bReplayExpired) continue;
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
		for (const FTransactionPackageRevisionTransition& Transition : PackageTransitions)
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
			|| PackageTransitions.capacity() > std::numeric_limits<size_t>::max()
				/ sizeof(FTransactionPackageRevisionTransition)
			|| !AddSize(Total, PackageTransitions.capacity()
				* sizeof(FTransactionPackageRevisionTransition)))
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
				DURIN_ERROR("Unable to begin transaction: {}", Operation.error());
				return -1;
			}
			GUndo = &*Pending;
			State = ETransactorState::Recording;
		}
		return Index;
	}
	auto DTransBuffer::ReleaseRecording() -> void
	{
		if (Pending && Pending->OperationId != 0) (void)Pending->EndOperation();
		if (Pending) Pending->CaptureIndex.reset();
		if (Pending && GUndo == &*Pending) GUndo = nullptr;
		ActiveCount = 0;
	}
	auto DTransBuffer::SetPrimaryUndoObject(DObject* Object) -> void
	{ if (CanTransact() && Pending) Pending->SetPrimaryObject(Object); }
	auto DTransBuffer::AppendCommandRecord(FTransaction::FObjectRecord&& Record) -> void
	{
		for (DPackage* Package : Record.GetAffectedPackages())
			if (IsValid(Package) && Package->IsAssetPackage()) EnsurePackageState(*Package);
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
		if (ActiveCount == 0 || State != ETransactorState::Recording || !Pending || Pending->IsBusy()) return 0;
		if (--ActiveCount > 0) return Result;
		if (GIsGameThreadIdInitialized) CheckGameThread();
		const auto Id = Pending->GetId();
		bool bPublicationStarted = false;
		struct FEndExit
		{
			ETransactorState& State;
			ITransaction* Recording;
			~FEndExit()
			{
				if (State == ETransactorState::Finalizing || State == ETransactorState::Recording)
				{
					if (GUndo == Recording) GUndo = nullptr;
					State = ETransactorState::RecoveryRequired;
				}
			}
		} Exit{State, GUndo};
		auto FailException = [&](std::string Message) {
			const bool bRecovery = bPublicationStarted || State == ETransactorState::RecoveryRequired
				|| (Pending && Pending->bObserverRecoveryRequired);
			ReleaseRecording();
			State = bRecovery ? ETransactorState::RecoveryRequired : ETransactorState::Finalizing;
			DiscardPending(Id, ETransactionEventType::Failed, std::move(Message), true);
		};
		try
		{
			const auto Captured = Pending->Finalize();
			State = ETransactorState::Finalizing;
			ReleaseRecording();
			if (Pending->RecordingFailure && !Pending->bObserverRecoveryRequired) DiscardIncompleteRecording();
			else if (!Captured)
			{
				if (Pending->bObserverRecoveryRequired) State = ETransactorState::RecoveryRequired;
				DiscardPending(Id, ETransactionEventType::Failed, Captured.error(), Pending->bObserverRecoveryRequired);
			}
			else FinalizePending(bPublicationStarted);
		}
		catch (const std::exception& Error) { FailException(std::format("Transaction finalization threw: {}", Error.what())); }
		catch (...) { FailException("Transaction finalization threw an unknown exception."); }
		return Result;
	}
	auto DTransBuffer::Cancel(int32 StartIndex) -> void
	{
		CheckThread();
		(void)StartIndex; // UE 5.8 cancels the entire recording, including inner Cancel.
		if (State != ETransactorState::Recording || !Pending || Pending->IsBusy()) return;
		if (Pending->RecordingFailure) return DiscardIncompleteRecording();
		QueueEvent(ETransactionEventType::Discarded, *Pending, "Transaction recording canceled; applied edits remain.");
		ReleaseRecording(); Pending.reset(); State = ETransactorState::Idle;
		RefreshOutsideEdits();
	}

	auto DTransBuffer::DiscardPending(FTransactionId Id, ETransactionEventType EventType, std::string Message, bool bUnknownEffects) -> void
	{
		const bool bRecoveryRequired = State == ETransactorState::RecoveryRequired;
		State = ETransactorState::Finalizing;
		auto Recovery = [&](std::string CleanupMessage) {
			State = ETransactorState::RecoveryRequired;
			DURIN_ERROR("Transaction {} finalization failed: {}. Cleanup failed: {}. {}", Id, Message, CleanupMessage, RecoveryGuidance);
		};
		try
		{
			if (bUnknownEffects)
			{
				for (auto& [Package, Tracked] : PackageStates)
				{
					Tracked.SavedRevision = 0;
					SynchronizeDirtyState(Tracked);
				}
			}
			else if (EventType == ETransactionEventType::Failed && Pending)
			{
				for (const auto& Transition : Pending->PackageTransitions)
					if (auto* Package = Cast<DPackage>(Transition.Package.Resolve())) InvalidateSavedState(*Package);
			}
			if (bRecoveryRequired) Message += std::string(" ") + RecoveryGuidance;
			std::string Description;
			if (Pending && Pending->GetId() == Id) Description = Pending->Context.Description;
			else if (const auto It = std::ranges::find(History, Id, &FTransaction::GetId); It != History.end())
				Description = It->Context.Description;
			Events.push_back({.Type = EventType, .Operation = ETransactionOperation::Execute, .Id = Id,
				.Description = std::move(Description), .Details = Message});
			if (bRecoveryRequired) State = ETransactorState::RecoveryRequired;
			else
			{
				if (Pending) { Pending->bBusy = true; Pending.reset(); }
				RefreshOutsideEdits();
				State = ETransactorState::Idle;
			}
		}
		catch (const std::exception& Cleanup) { Recovery(Cleanup.what()); }
		catch (...) { Recovery("Finalization cleanup threw an unknown exception."); }
	}

	auto DTransBuffer::FinalizePending(bool& bPublicationStarted) -> void
	{
		const FTransactionId TransactionId = Pending->GetId();
		const auto Pie = Pending->ContainsPieObjects();
		if (!Pie || *Pie)
		{
			return DiscardPending(TransactionId, Pie ? ETransactionEventType::Discarded : ETransactionEventType::Failed,
				Pie ? "Play-in-Editor references cannot enter persistent editor history." : Pie.error());
		}
		Pending->RemoveNoOpRecords();
		if (Pending->GetRecordCount() == 0)
		{
			QueueEvent(ETransactionEventType::Discarded, *Pending, "The transaction contained no records.");
			Pending->bBusy = true; Pending.reset(); RefreshOutsideEdits(); State = ETransactorState::Idle;
			return;
		}
		PreparePackageTransitions(*Pending);
		FTransactionEvent Executed{.Type = ETransactionEventType::Executed, .Operation = ETransactionOperation::Execute,
			.Id = TransactionId, .Description = Pending->Context.Description, .Details = Pending->GetDetails(ETransactionOperation::Execute)};
		auto DiscardOverflow = [&](std::string Reason) {
			Pending->InvalidateRecording(std::move(Reason), nullptr);
			DiscardIncompleteRecording();
		};
		size_t EntryBytes = 0;
		if (!Pending->TryGetOwnedSize(EntryBytes)) return DiscardOverflow("Transaction byte accounting overflowed.");
		std::vector<size_t> PrefixBytes; PrefixBytes.reserve(Cursor);
		size_t RetainedBytes = 0;
		for (size_t Index = 0; Index < Cursor; ++Index)
		{
			size_t Bytes = 0;
			if (!History[Index].TryGetOwnedSize(Bytes) || !AddSize(RetainedBytes, Bytes))
				return DiscardOverflow("Retained transaction byte accounting overflowed.");
			PrefixBytes.push_back(Bytes);
		}
		if (RetainedBytes > std::numeric_limits<size_t>::max() - EntryBytes)
			return DiscardOverflow("Retained transaction byte accounting overflowed.");
		size_t RemovalCount = 0;
		if (Cursor == History.size())
			while (RemovalCount < Cursor && RetainedBytes >= MemoryBudget) RetainedBytes -= PrefixBytes[RemovalCount++];
		std::vector<FTransactionEvent> PreparedEvents;
		for (size_t Index = 0; Index < RemovalCount; ++Index)
			PreparedEvents.push_back({.Type = ETransactionEventType::Evicted, .Operation = ETransactionOperation::Execute,
				.Id = History[Index].GetId(), .Description = History[Index].Context.Description});
		for (size_t Index = Cursor; Index < History.size(); ++Index)
			PreparedEvents.push_back({.Type = ETransactionEventType::Discarded, .Operation = ETransactionOperation::Execute,
				.Id = History[Index].GetId(), .Description = History[Index].Context.Description, .Details = "A new commit replaced the redo branch."});
		PreparedEvents.push_back(std::move(Executed));
		History.reserve(Cursor + 1);
		if (PreparedEvents.size() > Events.max_size() - Events.size()) throw std::length_error("Transaction event capacity exceeded.");
		Events.reserve(Events.size() + PreparedEvents.size());
		// Prepare metadata callbacks and history/event allocations before changing the old branch.
		bPublicationStarted = true;
		if (Cursor < History.size())
		{
			History.erase(History.begin() + Cursor, History.end());
			for (auto& Barrier : UndoBarriers) Barrier = std::min(Barrier, Cursor);
		}
		if (RemovalCount)
		{
			RemapUndoBarriers(0, RemovalCount);
			History.erase(History.begin(), History.begin() + RemovalCount);
			Cursor -= RemovalCount;
		}
		Pending->RecordingOwner = nullptr;
		History.push_back(std::move(*Pending)); Pending.reset(); Cursor = History.size();
		OwnedBytes = RetainedBytes + EntryBytes;
		ApplyPackageTransitions(History.back(), true);
		for (auto& Event : PreparedEvents) Events.push_back(std::move(Event));
		State = ETransactorState::Idle;
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

	auto DTransBuffer::FindReplayIndex(bool bUndo, bool bRespectUndoBarrier) const -> std::optional<size_t>
	{
		if (State == ETransactorState::Inspecting || State == ETransactorState::Finalizing) return std::nullopt;
		FTransactionInspectionScope Inspect(State);
		if (bUndo)
		{
			const size_t Limit = bRespectUndoBarrier && !UndoBarriers.empty() ? UndoBarriers.back() : 0;
			for (size_t Index = Cursor; Index > Limit; --Index)
				if (!History[Index - 1].HasExpired()) return Index - 1;
		}
		else
		{
			for (size_t Index = Cursor; Index < History.size(); ++Index)
				if (!History[Index].HasExpired()) return Index;
		}
		return std::nullopt;
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
		if (State != ETransactorState::Idle || GUndo)
			return FailWithReason(OutError, bUndo ? "Undo requires an idle transactor." : "Redo requires an idle transactor.");
		const size_t UndoLimit = UndoBarriers.empty() ? 0 : UndoBarriers.back();
		if (bUndo && Cursor != 0 && Cursor <= UndoLimit)
			return FailWithReason(OutError, "An undo barrier blocks earlier history.");
		const auto Index = FindReplayIndex(bUndo);
		if (ExpectedId && (*ExpectedId == 0 || !Index || History[*Index].GetId() != *ExpectedId))
			return FailWithReason(OutError, bUndo ? "The expected transaction is not the Undo head." : "The expected transaction is not the Redo head.");

		const size_t NextCursor = bUndo ? Index.value_or(UndoLimit)
			: (Index ? *Index + 1 : History.size());
		if (NextCursor == Cursor) return FailWithReason(OutError, bUndo ? "Nothing to undo." : "Nothing to redo.");
		// An expired-only transition can still retire the Redo suffix. Keep a
		// live entry for failure reporting until publication has completed.
		FTransaction& Transaction = History[Index.value_or(bUndo ? Cursor - 1 : Cursor)];
		check(HistoryRevision != std::numeric_limits<uint64>::max());
		++HistoryRevision;
		State = bUndo ? ETransactorState::Undoing : ETransactorState::Redoing;
		try
		{
			FTransaction::FReplayResult Outcome;
			if (Index)
			{
				RefreshOutsideEdits();
				if (auto Applied = Transaction.Apply(bUndo, bUndo ? EPropertyChangeOrigin::Undo : EPropertyChangeOrigin::Redo, Outcome); !Applied)
				{
					if (!Outcome.bDiscardHistory || Applied.error().Status == ETransactionApplyFailure::RecoveryRequired)
						return HandleApplyFailure(Transaction, std::move(Applied.error()), OutError);
					auto Reason = std::move(Applied.error().Message);
					ClearReplayHistory(Transaction, ETransactionEventType::Failed,
						bUndo ? ETransactionOperation::Undo : ETransactionOperation::Redo, Reason, false);
					return FailWithReason(OutError, Reason);
				}
			}
			auto Bytes = std::optional{OwnedBytes};
			if (!Outcome.bDiscardHistory && (Index || !bCanRedo)) Bytes = MeasureOwnedBytes(bCanRedo ? History.size() : NextCursor);
			if (!Bytes || Outcome.bDiscardHistory)
			{
				ClearReplayHistory(Transaction, bUndo ? ETransactionEventType::Undone : ETransactionEventType::Redone,
					bUndo ? ETransactionOperation::Undo : ETransactionOperation::Redo,
					Outcome.bDiscardHistory ? "History was cleared because it retained Play-in-Editor references."
						: "History was cleared because its retained size overflowed after replay.", Index.has_value());
				return Index.has_value();
			}
			if (Index)
			{
				ApplyPackageTransitions(Transaction, !bUndo, &Outcome);
				QueueEvent(bUndo ? ETransactionEventType::Undone : ETransactionEventType::Redone, Transaction, std::move(Outcome.Details));
			}
			for (size_t Slot = std::min(Cursor, NextCursor); Slot < std::max(Cursor, NextCursor); ++Slot)
				if (Slot != Index) History[Slot].RetireExpiredRecords();
			if (!bCanRedo)
			{
				for (size_t Slot = NextCursor; Slot < History.size(); ++Slot)
					QueueEvent(ETransactionEventType::Discarded, History[Slot], "Undo without Redo.");
				State = ETransactorState::Finalizing;
				History.erase(History.begin() + NextCursor, History.end());
				for (auto& Barrier : UndoBarriers) Barrier = std::min(Barrier, NextCursor);
			}
			Cursor = NextCursor;
			OwnedBytes = *Bytes;
			State = ETransactorState::Idle;
			return Index.has_value();
		}
		catch (const std::exception& Error)
		{
			return HandleApplyFailure(Transaction, {.Status = ETransactionApplyFailure::RecoveryRequired,
				.Message = std::format("Transaction replay threw: {}", Error.what())}, OutError, false);
		}
		catch (...)
		{
			return HandleApplyFailure(Transaction, {.Status = ETransactionApplyFailure::RecoveryRequired,
				.Message = "Transaction replay threw an unknown exception."}, OutError, false);
		}
	}

	auto DTransBuffer::ClearReplayHistory(const FTransaction& Transaction, ETransactionEventType Type,
		ETransactionOperation Operation, std::string Reason, bool bApplied) -> void
	{
		FTransactionEvent Event{.Type = Type, .Operation = Operation, .Id = Transaction.GetId(),
			.Description = Transaction.Context.Description, .Details = Reason};
		Events.reserve(1);
		if (bApplied)
			for (const auto& Transition : Transaction.PackageTransitions)
				if (auto* Package = Cast<DPackage>(Transition.Package.Resolve())) InvalidateSavedState(*Package);
		ClearHistory(std::move(Reason));
		if (bApplied || Type == ETransactionEventType::Failed) Events.push_back(std::move(Event));
	}

	auto DTransBuffer::HandleApplyFailure(const FTransaction& Transaction, FTransactionApplyError Error,
		std::string* OutError, bool bMeasureHistory) -> bool
	{
		const auto Operation = State == ETransactorState::Undoing ? ETransactionOperation::Undo
			: State == ETransactorState::Redoing ? ETransactionOperation::Redo : ETransactionOperation::Execute;
		if (Error.Status != ETransactionApplyFailure::RecoveryRequired && bMeasureHistory && !RecalculateOwnedBytes())
		{
			Error.Message += " History was cleared because its retained size overflowed.";
			ClearReplayHistory(Transaction, ETransactionEventType::Failed, Operation, Error.Message, false);
			return FailWithReason(OutError, Error.Message);
		}
		const bool bRecoveryRequired = Error.Status == ETransactionApplyFailure::RecoveryRequired;
		State = bRecoveryRequired ? ETransactorState::RecoveryRequired : ETransactorState::Idle;
		auto Message = std::move(Error.Message);
		for (const auto& Rollback : Error.RollbackFailures)
			Message += std::format(" Rollback record {} failed: {}", Rollback.RecordIndex, Rollback.Message);
		if (bRecoveryRequired) Message += std::string(" ") + RecoveryGuidance;
		if (bRecoveryRequired)
		{
			ReleaseRecording();
			for (const auto& Transition : Transaction.PackageTransitions)
				if (auto* Package = Cast<DPackage>(Transition.Package.Resolve())) InvalidateSavedState(*Package);
			DURIN_ERROR("Transaction {} requires recovery: {}", Transaction.GetId(), Message);
		}
		QueueEvent(ETransactionEventType::Failed, Transaction, Message);
		Events.back().Operation = Operation;
		return FailWithReason(OutError, Message);
	}

	auto DTransBuffer::Reset(std::string_view Reason) -> void
	{
		CheckThread();
		if (!CanTransact())
		{
			DURIN_ERROR("Cannot reset transaction history: {}", State == ETransactorState::RecoveryRequired
				? RecoveryGuidance : "A history transition is in progress.");
			return;
		}
		if (Pending && Pending->RecordingFailure) return DiscardIncompleteRecording();
		// Own the reason before releasing entries that may contain it.
		ClearHistory(std::string(Reason));
	}

	auto DTransBuffer::DiscardIncompleteRecording() -> void
	{
		State = ETransactorState::Finalizing;
		ReleaseRecording();
		const auto Id = Pending->GetId();
		try
		{
			const auto& Failure = *Pending->RecordingFailure;
			FTransactionEvent Event{.Type = ETransactionEventType::Failed, .Id = Id,
				.Description = Pending->Context.Description, .Details = Failure.Reason};
			Events.reserve(1);
			auto Packages = Pending->GetAffectedPackages();
			for (const auto& Key : Failure.Packages)
				if (auto* Package = Cast<DPackage>(ResolveObjectKey(Key)); Package && !std::ranges::contains(Packages, Package))
					Packages.push_back(Package);
			for (auto* Package : Packages)
				if (IsValid(Package) && Package->IsAssetPackage()) InvalidateSavedState(*Package);
			ClearHistory(Failure.Reason);
			Events.push_back(std::move(Event));
		}
		catch (...)
		{
			State = ETransactorState::RecoveryRequired;
			DiscardPending(Id, ETransactionEventType::Failed, "Failed to retire incomplete transaction history.", true);
		}
	}

	auto DTransBuffer::ClearHistory(std::string Reason) -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		State = ETransactorState::Resetting;
		check(HistoryRevision != std::numeric_limits<uint64>::max());
		++HistoryRevision;
		ReleaseRecording();
		if (Pending) Pending->bBusy = true;
		auto RetiredHistory = std::exchange(History, {});
		for (auto& Transaction : RetiredHistory) Transaction.bBusy = true;
		Cursor = 0; OwnedBytes = 0; UndoBarriers.clear();
		Events.clear();
		Pending.reset(); RetiredHistory.clear();
		// Clear after callbacks so busy-save notifications cannot repopulate tracking.
		PackageStates.clear();
		ResetReason = std::move(Reason);
		State = ETransactorState::Idle;
	}

	auto DTransBuffer::DiscardCustomChangesByModule(std::string_view ModuleName) -> bool
	{
		CheckThread();
		if (ModuleName.empty() || State != ETransactorState::Idle) return false;
		for (size_t Index = History.size(); Index-- > 0;)
		{
			if (!History[Index].IsOwnedByModule(ModuleName)) continue;
			for (auto* Package : History[Index].GetAffectedPackages())
				if (IsValid(Package) && Package->IsAssetPackage()) InvalidateSavedState(*Package);
			QueueEvent(ETransactionEventType::Discarded, History[Index], "Owning module is unloading.");
			RemapUndoBarriers(Index, 1);
			History.erase(History.begin() + Index);
			if (Index < Cursor) --Cursor;
		}
		if (!RecalculateOwnedBytes())
		{
			History.clear();
			Cursor = 0;
			UndoBarriers.clear();
			OwnedBytes = 0;
			DURIN_ERROR("Retained transaction byte accounting overflowed during module drain.");
			return false;
		}
		return true;
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
		if (State != ETransactorState::Idle || GUndo)
		{
			if (Reason) *Reason = State == ETransactorState::RecoveryRequired
				? RecoveryGuidance : "A transaction is active.";
			return false;
		}
		if (!CanReplayInHost(Reason)) return false;
		const bool bAvailable = FindReplayIndex(true).has_value();
		const bool bBarrier = !UndoBarriers.empty() && !bAvailable && FindReplayIndex(true, false).has_value();
		if (Reason) *Reason = bAvailable ? "" : bBarrier ? "An undo barrier blocks earlier history." : ResetReason.empty() ? "Nothing to undo." : ResetReason;
		return bAvailable;
	}

	auto DTransBuffer::SetUndoBarrier() -> void
	{
		CheckThread();
		if (State == ETransactorState::Idle && !GUndo) { UndoBarriers.push_back(Cursor); ++HistoryRevision; }
	}
	auto DTransBuffer::RemoveUndoBarrier() -> void
	{
		CheckThread();
		if (State == ETransactorState::Idle && !GUndo && !UndoBarriers.empty()) { UndoBarriers.pop_back(); ++HistoryRevision; }
	}
	auto DTransBuffer::ClearUndoBarriers() -> void
	{
		CheckThread();
		if (State == ETransactorState::Idle && !GUndo && !UndoBarriers.empty()) { UndoBarriers.clear(); ++HistoryRevision; }
	}
	auto DTransBuffer::RemapUndoBarriers(size_t First, size_t Count) -> void
	{
		for (auto& Barrier : UndoBarriers) if (Barrier > First) Barrier -= std::min(Count, Barrier - First);
	}
	auto DTransBuffer::SetMemoryBudget(size_t Bytes) -> bool
	{
		CheckThread();
		if (State != ETransactorState::Idle || GUndo) return false;
		MemoryBudget = Bytes; ++HistoryRevision; return true;
	}
	auto DTransBuffer::ContainsPieObjects() const -> bool
	{
		CheckThread();
		if ((State != ETransactorState::Idle && State != ETransactorState::Recording && State != ETransactorState::RecoveryRequired)
			|| (Pending && Pending->IsBusy()))
			return true;
		if (GIsGameThreadIdInitialized) CheckGameThread();
		FTransactionInspectionScope Inspect(State);
		if (Pending && Pending->ContainsPieObjects().value_or(true)) return true;
		for (const auto& Transaction : History)
			if (Transaction.ContainsPieObjects().value_or(true)) return true;
		return false;
	}

	auto DTransBuffer::CanRedo(std::string* Reason) const -> bool
	{
		if (State != ETransactorState::Idle || GUndo)
		{
			if (Reason) *Reason = State == ETransactorState::RecoveryRequired
				? RecoveryGuidance : "A transaction is active.";
			return false;
		}
		if (!CanReplayInHost(Reason)) return false;
		const bool bAvailable = FindReplayIndex(false).has_value();
		if (Reason) *Reason = bAvailable ? "" : "Nothing to redo.";
		return bAvailable;
	}

	auto DTransBuffer::AddReferencedObjects(FReferenceCollector& Collector) -> void
	{
		DTransactor::AddReferencedObjects(Collector);
		Collector.AllowEliminatingReferences(false);
		if (Pending) Pending->AddReferencedObjects(Collector);
		for (const FTransaction& Transaction : History)
			Transaction.AddReferencedObjects(Collector);
		Collector.AllowEliminatingReferences(true);
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

	auto DTransBuffer::GetUndoContext(bool bCheckWhetherUndoPossible) const -> FTransactionContext
	{
		if (State == ETransactorState::Finalizing) return {};
		if (bCheckWhetherUndoPossible && (State != ETransactorState::Idle || GUndo)) return {};
		const auto Index = FindReplayIndex(true, bCheckWhetherUndoPossible);
		return Index ? History[*Index].GetContext() : FTransactionContext{};
	}

	auto DTransBuffer::GetRedoContext() const -> FTransactionContext
	{
		if (State != ETransactorState::Idle || GUndo) return {};
		const auto Index = FindReplayIndex(false);
		return Index ? History[*Index].GetContext() : FTransactionContext{};
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
			.ObservedEditRevision = Package.GetEditRevision(),
		});
		check(bInserted);
		return It->second;
	}

	auto DTransBuffer::PreparePackageTransitions(FTransaction& Transaction) -> void
	{
		if (!Transaction.PackageTransitions.empty()) return;
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
			});
		}
		Transaction.PackageTransitions = std::move(Transitions);
	}

	auto DTransBuffer::ApplyPackageTransitions(
		const FTransaction& Transaction,
		bool bForward, const FTransaction::FReplayResult* Replay) -> void
	{
		if (Replay && Replay->AppliedRecords.empty()) return;
		for (const FTransactionPackageRevisionTransition& Transition
			: Transaction.PackageTransitions)
		{
			auto* Package = Cast<DPackage>(Transition.Package.Resolve());
			if (!Package) continue;
			if (Replay)
				{
				if (const auto It = Replay->PackagesWithSkippedRecords.find(Package); It != Replay->PackagesWithSkippedRecords.end())
				{
					// A partial replay cannot establish the original package checkpoint.
					if (It->second) InvalidateSavedState(*Package);
					continue;
				}
				}
			FTrackedPackageState* State = FindPackageState(*Package);
			if (!State)
			{
				auto [It, bInserted] = PackageStates.emplace(Package,
					FTrackedPackageState{
						.Package = FPersistentObjectRef(Package),
						.CurrentRevision = Transition.BeforeRevision,
						.SavedRevision = Transition.InitialSavedRevision,
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
			State.ObservedEditRevision = Package->GetEditRevision();
		}
	}

	auto DTransBuffer::SynchronizeDirtyState(FTrackedPackageState& State) -> void
	{
		auto* Package = Cast<DPackage>(State.Package.Resolve());
		if (!Package) return;
		if (State.SavedRevision == 0 || State.CurrentRevision != State.SavedRevision)
			Package->MarkDirty();
		else
			Package->ClearDirty();
		State.ObservedEditRevision = Package->GetEditRevision();
	}

	auto DTransBuffer::EstablishSavedState(DPackage& Package) -> void
	{
		CheckThread();
		if (State != ETransactorState::Idle || GUndo)
		{
			InvalidateSavedState(Package);
			return;
		}
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
			.ObservedEditRevision = Package.GetEditRevision(),
		});
		Package.ClearDirty();
	}

	auto DTransBuffer::MarkSaved(DPackage& Package) -> void
	{
		CheckThread();
		// A busy save can represent an intermediate state, not either retained
		// endpoint. Like UE's dirty fence, it must not revive a historical clean bit.
		if (State != ETransactorState::Idle || GUndo)
		{
			InvalidateSavedState(Package);
			return;
		}
		RefreshOutsideEdits();
		FTrackedPackageState& State = EnsurePackageState(Package);
		State.SavedRevision = State.CurrentRevision;
		SynchronizeDirtyState(State);
	}

	auto DTransBuffer::InvalidateSavedState(DPackage& Package) -> void
	{
		FTrackedPackageState& State = EnsurePackageState(Package);
		State.SavedRevision = 0;
		SynchronizeDirtyState(State);
	}

	auto DTransBuffer::GetPackageRevisionState(const DPackage& Package) const
		-> std::optional<FPackageRevisionState>
	{
		if (State == ETransactorState::Resetting) return std::nullopt;
		const FTrackedPackageState* State = FindPackageState(Package);
		if (!State) return std::nullopt;
		return FPackageRevisionState{
			.CurrentRevision = State->CurrentRevision,
			.SavedRevision = State->SavedRevision,
		};
	}

	auto DTransBuffer::ForgetPackage(DPackage& Package) -> void
	{
		if (State != ETransactorState::Idle) return;
		for (size_t Index = History.size(); Index-- > 0;)
		{
			if (!History[Index].ReferencesPackage(Package)) continue;
			for (auto* Affected : History[Index].GetAffectedPackages())
				if (IsValid(Affected) && Affected != &Package && Affected->IsAssetPackage()) InvalidateSavedState(*Affected);
			QueueEvent(ETransactionEventType::Discarded, History[Index], "Referenced package is being retired.");
			RemapUndoBarriers(Index, 1);
			History.erase(History.begin() + Index);
			if (Index < Cursor) --Cursor;
		}
		PackageStates.erase(&Package);
		(void)RecalculateOwnedBytes();
	}

	auto DTransBuffer::RecalculateOwnedBytes() -> bool
	{
		const auto Bytes = MeasureOwnedBytes(History.size());
		if (!Bytes) return false;
		OwnedBytes = *Bytes;
		return true;
	}

	auto DTransBuffer::MeasureOwnedBytes(size_t Count) const -> std::optional<size_t>
	{
		size_t Total = 0;
		for (const FTransaction& Transaction : std::span(History).first(Count))
		{
			size_t Bytes = 0;
			if (!Transaction.TryGetOwnedSize(Bytes)
				|| Total > std::numeric_limits<size_t>::max() - Bytes) return std::nullopt;
			Total += Bytes;
		}
		return Total;
	}

	class FTransactorReloadParticipant final : public IPackageReloadParticipant
	{
		struct FSourceRecord
		{
			std::optional<FTransaction::FWholeObjectData> Object;
			std::optional<FTransaction::FArrayData> Array;
			const FCommandChange* Command = nullptr;
			FPersistentObjectRef CustomObject;
			std::vector<FPersistentObjectRef> Modified;
		};
		struct FSourceTransaction
		{
			FTransactionId Id = 0;
			FPersistentObjectRef Primary;
			std::vector<FTransactionPackageRevisionTransition> Transitions;
			std::vector<FSourceRecord> Records;
		};
		struct FPreparedTransaction
		{
			FPreparedTransaction(FTransaction Value, size_t Count) : Transaction(std::move(Value)), Records(Count) {}
			FTransaction Transaction;
			std::vector<std::optional<FTransaction::FObjectRecord>> Records;
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
			if (Transactor.State != ETransactorState::Idle || Transactor.Pending || Transactor.ActiveCount)
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
					if (const auto* Object = std::get_if<FTransaction::FWholeObjectData>(&Record.Data)) Snapshot.Object = *Object;
					else if (const auto* Array = std::get_if<FTransaction::FArrayData>(&Record.Data)) Snapshot.Array = *Array;
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
						AfterEvents.push_back({.Type = ETransactionEventType::Discarded, .Id = Transaction.Id,
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
					if (!AfterRevision || AfterRevision == std::numeric_limits<FRevisionId>::max())
						return std::unexpected(FObjectReplacementError{.Code = EObjectReplacementError::BudgetExceeded});
					const auto Revision = AfterRevision++;
					NewState.CurrentRevision = Revision;
					NewState.SavedRevision = ReplacementPackage->IsDirty() ? 0 : Revision;
					NewState.ObservedEditRevision = ReplacementPackage->GetEditRevision();
					for (auto& Transaction : Replacements) if (Transaction)
						for (auto& Transition : Transaction->Transaction.PackageTransitions)
							if (Transition.Package.Resolve() == ReplacementPackage)
							{
								if (Transition.BeforeRevision == State.CurrentRevision) Transition.BeforeRevision = Revision;
								if (Transition.AfterRevision == State.CurrentRevision) Transition.AfterRevision = Revision;
								Transition.InitialSavedRevision = NewState.SavedRevision;
							}
				}
				else if (std::ranges::contains(Invalidated, Package))
				{
					NewState.SavedRevision = 0;
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
						if (!AfterRevision || AfterRevision == std::numeric_limits<FRevisionId>::max())
							return std::unexpected(FObjectReplacementError{.Code = EObjectReplacementError::BudgetExceeded});
						const auto Revision = AfterRevision++;
						AfterPackages.emplace(Package, DTransBuffer::FTrackedPackageState{
							.Package = FPersistentObjectRef(Package), .CurrentRevision = Revision,
							.SavedRevision = Package->IsDirty() ? 0 : Revision,
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
			if (!bPrepared || Transactor.State != ETransactorState::Idle || Transactor.Pending || Transactor.ActiveCount
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
						const auto* Object = std::get_if<FTransaction::FWholeObjectData>(&Current.Data);
						if (!Object || !Object->HasSameState(*Before.Object)) return false;
					}
					else if (Before.Array)
					{
						const auto* Array = std::get_if<FTransaction::FArrayData>(&Current.Data);
						if (!Array || !Array->HasSameState(*Before.Array)) return false;
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
			for (size_t Index = Keep.size(); Index-- > 0;) if (!Keep[Index]) Transactor.RemapUndoBarriers(Index, 1);
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
				if (GIsGameThreadIdInitialized) CheckGameThread();
				Sources.clear();
				AfterHistory.clear();
			}
			return true;
		}
	private:
		auto PrepareTransaction(const FTransaction& Source, const FObjectReplacementMap& Mapping)
			-> std::expected<FPreparedTransaction, std::string>
		{
			if (Source.bBusy || Source.RecordingOwner) return std::unexpected("Only settled history can be reinstanced.");
			FTransaction Transaction(Source.Id, Source.Context);
			Transaction.bFinalized = Source.bFinalized;
			Transaction.OperationId = Source.OperationId;
			Transaction.bObserverRecoveryRequired = Source.bObserverRecoveryRequired;
			auto Primary = Source.PrimaryObject.CreateReplacement(Mapping);
			if (!Primary) return std::unexpected(Primary.error());
			Transaction.PrimaryObject = std::move(*Primary);
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
		mutable std::vector<FTransaction> AfterHistory;
		decltype(DTransBuffer::PackageStates) BeforePackages, AfterPackages;
		std::vector<FTransactionEvent> BeforeEvents, AfterEvents;
		std::vector<FObjectKey> BeforeReferences;
		std::vector<DPackage*> DirtyPackages;
		size_t BeforeCursor = 0, AfterCursor = 0, BeforeBytes = 0, AfterBytes = 0, BeforeBudget = 0;
		FRevisionId BeforeRevision = 0, AfterRevision = 0;
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
		verify(Participant->CanRetire());
		return {};
	}

	auto DTransBuffer::QueueEvent(
		ETransactionEventType Type,
		const FTransaction& Transaction,
		std::optional<std::string> Details) -> void
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
		Events.push_back({
			.Type = Type,
			.Operation = Operation,
			.Id = Transaction.GetId(),
			.Description = Transaction.GetContext().Description,
			.Details = Details ? std::move(*Details) : Transaction.GetDetails(Operation),
		});
	}
}
