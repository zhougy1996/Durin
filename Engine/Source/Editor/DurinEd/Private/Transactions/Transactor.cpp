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
#include "CoreGlobals.h"
#include "Logging/LogMacros.h"
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

	auto FormatTransactorRejection(const FTransactorRejection& Rejection) -> std::string
	{
		switch (Rejection.Reason)
		{
		case ETransactorRejectionReason::BeginState: return "Begin is unavailable during a history transition or destruction.";
		case ETransactorRejectionReason::RecordScope: return "Record requires an active transaction scope.";
		case ETransactorRejectionReason::ExecuteState: return "Custom changes are unavailable during a history transition or destruction.";
		case ETransactorRejectionReason::MissingCustom: return "The custom transaction change is unavailable.";
		case ETransactorRejectionReason::UpdateScope: return "Record update requires an active transaction scope.";
		case ETransactorRejectionReason::MissingRecord: return "The pending transaction record identifier is unavailable.";
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
			case ETransactorFailure::Unsupported: Message = "The abstract transactor does not implement this operation."; break;
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

	auto FTransactionRecord::Validate() const -> FTransactionRecordResult
	{
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

	auto FTransactionRecord::Apply(bool bBefore, EPropertyChangeOrigin Origin, bool* OutRecoveryRequired) -> FTransactionRecordResult
	{
		if (OutRecoveryRequired) *OutRecoveryRequired = false;
		return std::visit([&](auto& Record) -> FTransactionRecordResult {
			using T = std::decay_t<decltype(Record)>;
			if constexpr (std::is_same_v<T, FTransactionObjectRecord>)
			{
				const auto Result = Record.Apply(bBefore, Origin);
				if (!Result) return std::unexpected(FormatTransactionObjectRecordError(Result.error()));
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
		CustomObject.AddReferencedObjects(Collector);
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
				OutBytes = sizeof(FCommandChange) + Allocated;
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

	auto FTransactionRecord::GetAffectedPackages() const -> std::span<DPackage* const>
	{
		if (const auto* Change = std::get_if<std::unique_ptr<FCommandChange>>(&Data);
			Change && *Change)
			return (*Change)->GetAffectedPackages();
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

	auto FTransactionRecord::GetObjectTarget() const -> DObject*
	{
		if (const auto* Record = std::get_if<FTransactionObjectRecord>(&Data))
			return Record->GetTarget().Resolve();
		return CustomObject.Resolve();
	}

	FTransaction::FTransaction(FTransactionId InId, FTransactionContext InContext, DTransBuffer* InRecordingOwner)
		: RecordingOwner(InRecordingOwner), Id(InId)
		, Context(std::move(InContext))
	{
	}

	auto FTransaction::AddRecord(FTransactionObjectRecord Record) -> uint64
	{
		for (size_t Index = 0; Index < Records.size(); ++Index)
		{
			auto* Existing = std::get_if<FTransactionObjectRecord>(&Records[Index].Data);
			if (Existing && Existing->Target == Record.Target
				&& Existing->SnapshotMember.GetDeclaringType() == Record.SnapshotMember.GetDeclaringType()
				&& Existing->SnapshotMember.GetMemberName() == Record.SnapshotMember.GetMemberName()
				&& Existing->SnapshotMember.GetArrayIndex() == Record.SnapshotMember.GetArrayIndex())
			{
				Existing->After = std::move(Record.After);
				return Index + 1;
			}
		}
		Records.emplace_back(std::move(Record));
		return Records.size();
	}

	auto FTransaction::AddRecord(std::unique_ptr<FCommandChange> Change, DObject* Object) -> uint64
	{
		Records.emplace_back(std::move(Change), Object);
		return Records.size();
	}

	auto FTransaction::UpdateRecord(
		uint64 RecordId,
		FTransactionObjectRecord Record) -> bool
	{
		if (RecordId == 0 || RecordId > Records.size()) return false;
		auto* Existing = std::get_if<FTransactionObjectRecord>(&Records[RecordId - 1].Data);
		if (!Existing) return false;
		Existing->After = std::move(Record.After);
		return true;
	}

	auto FTransaction::SaveObject(DObject* Object) -> void
	{
		if (RecordingOwner) (void)RecordingOwner->SaveObject(Object);
		else (void)CaptureObject(Object);
	}
	auto FTransaction::ContainsObject(const DObject* Object) const -> bool
	{
		return std::ranges::any_of(CapturedObjects, [&](const auto& Ref) { return Ref.Resolve() == Object; })
			|| std::ranges::any_of(Records, [&](const auto& Record) { return Record.GetObjectTarget() == Object; });
	}
	auto FTransaction::CaptureObject(DObject* Object) -> FTransactionRecordResult
	{
		// A custom record already owns this object's undo representation, as in UE.
		if (std::ranges::any_of(Records, [&](const auto& Record) {
			return Record.CustomObject.Resolve() == Object;
		})) return {};
		if (std::ranges::any_of(CapturedObjects, [&](const auto& Ref) { return Ref.Resolve() == Object; })) return {};
		std::vector<FTransactionObjectRecord> ObjectRecords;
		std::optional<std::string> Error;
		Object->GetClass()->ForEachProperty([&](FProperty* Property) {
			if (Error || !Property || Property->HasAnyPropertyFlags(EPropertyFlags::NonTransactional | EPropertyFlags::Deprecated)) return;
			for (uint32 Index = 0; Index < Property->GetArrayDim(); ++Index)
			{
				// Construction owns generated identities; automatic snapshots must not retain them.
				if (Property->GetKind() == DurinCodeGen::EPropertyGenFlags::Object)
				{
					const auto* Reference = static_cast<const FObjectProperty*>(Property)->GetObjectPropertyValue(Object, Index);
					if (Reference && Reference->GetConstructionPurpose() == EObjectConstructionPurpose::Generated) continue;
				}
				FPropertyValueSnapshotPayload Before;
				if (auto Result = CapturePropertyValuePayload(Property, Object, Index, Before, true); !Result)
				{ Error = ToString(Result.error()); return; }
				auto Record = FTransactionObjectRecord::Capture(FPropertyEditTarget::ForMember(Object, Property, Index), Before, Before);
				if (!Record) { Error = FormatTransactionObjectRecordError(Record.error()); return; }
				ObjectRecords.push_back(std::move(*Record));
			}
		});
		if (Error) return std::unexpected(std::move(*Error));
		for (auto& Record : ObjectRecords)
		{
			const auto Id = AddRecord(std::move(Record));
			if (!std::ranges::contains(CapturedRecordIds, Id)) CapturedRecordIds.push_back(Id);
		}
		CapturedObjects.emplace_back(Object);
		return {};
	}

	auto FTransaction::FinalizeObjects() -> FTransactionRecordResult
	{
		for (uint64 Id : CapturedRecordIds)
			if (auto Result = Records[Id - 1].CaptureAfter(); !Result) return Result;
		CapturedObjects.clear(); CapturedRecordIds.clear();
		return {};
	}


	auto FTransaction::RemoveNoOpRecords() -> void
	{
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
		FGarbageCollectionDeferralScope DeferCollection;
		if (auto Result = Validate(); !Result) return Result;
		std::vector<size_t> Applied;
		Applied.reserve(Records.size());
		std::vector<FPersistentObjectRef> ReplayTargets;
		if (Origin != EPropertyChangeOrigin::Edit)
		{
			for (const FTransactionRecord& Record : Records)
			{
				if (DObject* Object = Record.GetObjectTarget())
				{
					const FPersistentObjectRef Target(Object);
					if (!std::ranges::contains(ReplayTargets, Target)) ReplayTargets.push_back(Target);
				}
			}
			for (const FPersistentObjectRef& Target : ReplayTargets)
				if (DObject* Object = Target.Resolve()) Object->PreEditUndo();
		}
		auto NotifyReplayTargets = [&] {
			for (const FPersistentObjectRef& Target : ReplayTargets)
				if (DObject* Object = Target.Resolve()) Object->PostEditUndo();
		};
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
					if (const auto Rollback = Records[*It].Apply(!bUndo, Origin); !Rollback)
						Error.RollbackFailures.push_back({*It, Rollback.error()});
				}
				if (bRecordRecoveryRequired || !Error.RollbackFailures.empty()) Error.Status = ETransactionApplyFailure::RecoveryRequired;
				NotifyReplayTargets();
				return std::unexpected(std::move(Error));
			}
			Applied.push_back(Index);
		}
		NotifyReplayTargets();
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
			if (auto* Object = Record.GetObjectTarget(); Object && !std::ranges::contains(Objects, Object)) Objects.push_back(Object);
		return Objects;
	}

	auto FTransaction::GetAffectedPackages() const -> std::vector<DPackage*>
	{
		std::vector<DPackage*> Packages;
		for (const FTransactionRecord& Record : Records)
		{
			if (DObject* Object = Record.GetObjectTarget())
			{
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
		Context.PrimaryObject.AddReferencedObjects(Collector);
		for (const auto& Object : CapturedObjects) Object.AddReferencedObjects(Collector);
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
		if (!AddSize(Total, CapturedObjects.capacity() * sizeof(FPersistentObjectRef))
			|| !AddSize(Total, CapturedRecordIds.capacity() * sizeof(uint64))) return false;
		for (const FTransactionRecord& Record : Records)
		{
			size_t RecordBytes = 0;
			if (!Record.TryGetAllocatedSize(RecordBytes) || !AddSize(Total, RecordBytes))
				return false;
		}
		OutBytes = Total;
		return true;
	}

	FScopedTransaction::FScopedTransaction(std::string_view Description, bool bShouldActuallyTransact)
		: FScopedTransaction(GEditor && GEditor->CanTransact() ? GEditor->GetTransactor() : nullptr,
			{.Description = std::string(Description)}, bShouldActuallyTransact) {}
	FScopedTransaction::FScopedTransaction(std::string_view Context, std::string_view Description, DObject* PrimaryObject, bool bShouldActuallyTransact)
		: FScopedTransaction(GEditor && GEditor->CanTransact() ? GEditor->GetTransactor() : nullptr,
			{std::string(Context), std::string(Description), FPersistentObjectRef(PrimaryObject)}, bShouldActuallyTransact) {}
	FScopedTransaction::FScopedTransaction(DTransactor* InTransactor, FTransactionContext Context, bool bShouldActuallyTransact)
	{
		if (bShouldActuallyTransact && InTransactor)
		{
			StartIndex = InTransactor->Begin(Context.Name, Context.Description);
			if (StartIndex >= 0)
			{
				Transactor = InTransactor;
				RecordingId = InTransactor->GetRecordingId();
				InTransactor->SetPrimaryUndoObject(Context.PrimaryObject.Resolve());
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

	namespace
	{
		auto Unsupported() -> FTransactorResult
		{
			return {.Code = ETransactorResultCode::Rejected,
				.FailureCause = FTransactorFailure{.Code = ETransactorFailure::Unsupported}};
		}
	}

	auto DTransactor::Begin(std::string_view, std::string_view) -> int32 { LastResult = Unsupported(); return -1; }
	auto DTransactor::End() -> int32 { return 0; }
	auto DTransactor::Cancel(int32) -> void {}
	auto DTransactor::IsActive() const -> bool { return false; }
	auto DTransactor::SaveObject(DObject*) -> bool { return false; }
	auto DTransactor::ReportChangeFailure(std::unique_ptr<FCommandChange>, FCommandChangeError) -> FTransactorResult { return Unsupported(); }
	auto DTransactor::StoreUndo(DObject*, std::unique_ptr<FCommandChange>) -> FTransactorResult { return Unsupported(); }
	auto DTransactor::Record(FTransactionObjectRecord) -> std::expected<uint64, FTransactorResult> { return std::unexpected(Unsupported()); }
	auto DTransactor::UpdateRecord(uint64, FTransactionObjectRecord) -> FTransactorResult { return Unsupported(); }
	auto DTransactor::Undo(bool) -> bool { return false; }
	auto DTransactor::UndoExpected(FTransactionId) -> FTransactorResult { return Unsupported(); }
	auto DTransactor::Redo() -> bool { return false; }
	auto DTransactor::RedoExpected(FTransactionId) -> FTransactorResult { return Unsupported(); }
	auto DTransactor::Reset(std::string_view) -> FTransactorResult { return Unsupported(); }
	auto DTransactor::CanTransact() const -> bool { return false; }
	auto DTransactor::SetPrimaryUndoObject(DObject*) -> void {}
	auto DTransactor::GetTransactionDetails(FTransactionId, ETransactionOperation) const -> std::string { return {}; }
	auto DTransactor::DiscardCustomChangesByModule(std::string_view) -> FTransactorResult { return Unsupported(); }
	auto DTransactor::CanUndo(std::string*) const -> bool { return false; }
	auto DTransactor::CanRedo(std::string*) const -> bool { return false; }

	auto DTransactor::GetUndoId() const -> FTransactionId { return 0; }
	auto DTransactor::GetRedoId() const -> FTransactionId { return 0; }
	auto DTransactor::GetUndoDescription() const -> std::string_view { return {}; }
	auto DTransactor::GetRedoDescription() const -> std::string_view { return {}; }

	auto DTransactor::AddTransactionListener(FTransactionListener Listener) -> uint64
	{ if (!Listener) return 0; const auto Id = NextListener++; Listeners.emplace(Id, std::move(Listener)); return Id; }
	auto DTransactor::RemoveTransactionListener(uint64 Handle) -> void { Listeners.erase(Handle); }
	auto DTransactor::DispatchEvents() -> void {}
	auto DTransactor::GetQueueLength() const -> size_t { return 0; }
	auto DTransactor::GetTransaction(size_t) const -> const FTransaction* { return nullptr; }
	auto DTransactor::GetUndoContext(bool bCheckWhetherUndoPossible) const -> FTransactionContext
	{
		if (bCheckWhetherUndoPossible && !CanUndo()) return {};
		for (size_t Index = GetQueueLength(); Index-- > 0;)
			if (auto* Transaction = GetTransaction(Index); Transaction && Transaction->GetId() == GetUndoId()) return Transaction->GetContext();
		return {};
	}
	auto DTransactor::GetRedoContext() const -> FTransactionContext
	{
		if (!CanRedo()) return {};
		for (size_t Index = 0; Index < GetQueueLength(); ++Index)
			if (auto* Transaction = GetTransaction(Index); Transaction && Transaction->GetId() == GetRedoId()) return Transaction->GetContext();
		return {};
	}
	auto DTransactor::GetMountedContentMutationRevision() const -> uint64 { return 1; }
	auto DTransactor::NotifyMountedContentMutation() -> void {}
	auto DTransactor::NotifyMountedContentMutation(FContentChangeBatch) -> void { NotifyMountedContentMutation(); }
	auto DTransactor::CaptureMountedContentChanges(uint64 FromRevision) const -> FContentChangeBatch
	{
		return {FromRevision, GetMountedContentMutationRevision(), true};
	}
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
			RefreshOutsideEdits();
			Pending.emplace(NextTransactionId++, FTransactionContext{std::string(Context), std::string(Description)}, this);
			GUndo = &*Pending;
			State = ETransactorState::Recording;
		}
		LastResult = {.Code = ETransactorResultCode::Succeeded, .TransactionId = Pending->GetId()};
		return Index;
	}
	auto DTransBuffer::ReleaseRecording() -> void
	{
		if (Pending && GUndo == &*Pending) GUndo = nullptr;
		ActiveCount = 0;
	}
	auto DTransBuffer::SetPrimaryUndoObject(DObject* Object) -> void
	{ if (Pending && Pending->Context.PrimaryObject.IsNull()) Pending->Context.PrimaryObject = FPersistentObjectRef(Object); }
	auto DTransBuffer::SaveObject(DObject* Object) -> bool
	{
		CheckThread();
		if (State != ETransactorState::Recording || !Pending || !IsValid(Object)) return false;
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
		if (State != ETransactorState::Recording || !Pending) return std::unexpected(Reject(ETransactorRejectionReason::RecordScope));
		if (auto* Object = Record.GetTarget().Resolve())
			if (auto* Package = Object->GetPackage(); Package && Package->IsAssetPackage()) EnsurePackageState(*Package);
		const auto Id = Pending->AddRecord(std::move(Record));
		return Id;
	}
	auto DTransBuffer::UpdateRecord(uint64 RecordId, FTransactionObjectRecord Record) -> FTransactorResult
	{
		CheckThread();
		if (State != ETransactorState::Recording || !Pending) return Reject(ETransactorRejectionReason::UpdateScope);
		if (!Pending->UpdateRecord(RecordId, std::move(Record))) return Reject(ETransactorRejectionReason::MissingRecord, RecordId);
		return {.Code = ETransactorResultCode::Succeeded, .TransactionId = Pending->GetId()};
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
		if (State != ETransactorState::Recording || !Pending) return 0;
		if (--ActiveCount > 0) return Result;
		ReleaseRecording();
		if (auto Captured = Pending->FinalizeObjects(); !Captured)
		{
			LastResult = {.Code = ETransactorResultCode::Failed, .TransactionId = Pending->GetId(),
				.ApplyCause = std::make_shared<FTransactionApplyError>(FTransactionApplyError{.Message = Captured.error()})};
			QueueEvent(ETransactionEventType::Failed, *Pending, Captured.error());
			Pending.reset(); State = ETransactorState::Idle;
			RefreshOutsideEdits();
		}
		else LastResult = FinalizePending();
		return Result;
	}
	auto DTransBuffer::Cancel(int32 StartIndex) -> void
	{
		CheckThread();
		(void)StartIndex; // UE 5.3.2 cancels the entire recording, including inner Cancel.
		if (State != ETransactorState::Recording || !Pending) return;
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
	public:
		FTransactorReloadParticipant(DTransBuffer& InTransactor,
			std::span<DPackage* const> InPackages)
			: Transactor(InTransactor), Packages(InPackages.begin(), InPackages.end()) {}

		auto Prepare(const FObjectReplacementMap& Map) -> std::expected<void, FObjectReplacementError> override
		{
			if (Transactor.State != Editor::ETransactorState::Idle)
				return std::unexpected(FObjectReplacementError{.Code = EObjectReplacementError::Busy, .Reason = EObjectReplacementReason::ParticipantBusy});
			TransactionIds.clear();
			OtherPackages.clear();
			for (const Editor::FTransaction& Transaction : Transactor.History)
				if (std::ranges::any_of(Packages, [&](const DPackage* Package) {
					return Package && Transaction.ReferencesPackage(*Package);
				}))
				{
					TransactionIds.push_back(Transaction.GetId());
					for (DPackage* Affected : Transaction.GetAffectedPackages())
						if (Affected
							&& !std::ranges::contains(Packages, Affected)
							&& !std::ranges::contains(OtherPackages, Affected))
							OtherPackages.push_back(Affected);
				}
			for (DPackage* Package : Packages)
			{
				const auto* Entry = Map.Find(Package);
				if (!Entry || !Entry->Replacement)
					return std::unexpected(FObjectReplacementError{.Code = EObjectReplacementError::UnmappedReference, .Reason = EObjectReplacementReason::ParticipantUnmappedPackage,
						.ObjectPath = Package ? Package->GetPackagePath() : std::string{}});
			}
			return {};
		}

		auto Validate() const -> bool override
		{
			if (Transactor.State != Editor::ETransactorState::Idle) return false;
			std::vector<Editor::FTransactionId> Current;
			for (const Editor::FTransaction& Transaction : Transactor.History)
				if (std::ranges::any_of(Packages, [&](const DPackage* Package) {
					return Package && Transaction.ReferencesPackage(*Package);
				})) Current.push_back(Transaction.GetId());
			return Current == TransactionIds;
		}

		auto CoversNativeReferences(const DObject& Owner) const -> bool override
		{
			return &Owner == &Transactor;
		}
		auto GetStrongReferenceCount(const DObject&) const -> uint32 override { return 0; }
		auto Commit() noexcept -> void override
		{
			for (DPackage* Package : Packages) if (Package) Transactor.ForgetPackage(*Package);
			// A whole cross-package transaction was retired. Preserve the other
			// package's applied content and dirty bit, but prevent its old save
			// checkpoint from claiming the shortened history is a clean baseline.
			for (DPackage* Package : OtherPackages)
				if (Package)
					if (auto* PackageState = Transactor.FindPackageState(*Package))
					{
						PackageState->SavedRevision = 0;
						PackageState->bCheckpointValid = false;
						Transactor.SynchronizeDirtyState(*PackageState);
					}
		}
		auto Abort() noexcept -> void override {}
		auto CanRetire() const -> bool override { return true; }

	private:
		DTransBuffer& Transactor;
		std::vector<DPackage*> Packages;
		std::vector<DPackage*> OtherPackages;
		std::vector<Editor::FTransactionId> TransactionIds;
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
