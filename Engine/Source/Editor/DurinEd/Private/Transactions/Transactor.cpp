#include "Materials/MaterialDiagnostic.h"
#include "Transactions/Transactor.h"

#include "Editor/EditorEngine.h"
#include "PropertyEditor/PropertyEditing.h"
#include "DObject/Archive.h"
#include "DObject/Class.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "DObject/Property.h"
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
		case ETransactorRejectionReason::RecordOrder: return "Records must be added to the innermost transaction scope.";
		case ETransactorRejectionReason::ExecuteState: return "Custom transaction execution requires an idle transactor.";
		case ETransactorRejectionReason::MissingCustom: return "The custom transaction change is unavailable.";
		case ETransactorRejectionReason::UpdateScope: return "Record update requires an active transaction scope.";
		case ETransactorRejectionReason::UpdateOrder: return "Records must be updated by the innermost transaction scope.";
		case ETransactorRejectionReason::MissingRecord: return "The pending transaction record identifier is unavailable.";
		case ETransactorRejectionReason::CloseScope: return "No transaction scope is active.";
		case ETransactorRejectionReason::CloseOrder: return "Transaction scopes must close in reverse begin order.";
		case ETransactorRejectionReason::UndoState: return "Undo requires an idle transactor.";
		case ETransactorRejectionReason::UndoHead: return "The expected transaction is not the Undo head.";
		case ETransactorRejectionReason::RedoState: return "Redo requires an idle transactor.";
		case ETransactorRejectionReason::RedoHead: return "The expected transaction is not the Redo head.";
		case ETransactorRejectionReason::ResetState: return "Reset requires an idle transactor.";
		case ETransactorRejectionReason::CompletionIdentity: return "The transaction is not awaiting deferred completion.";
		case ETransactorRejectionReason::ModuleName: return "A module name is required.";
		case ETransactorRejectionReason::ModulePending: return "A deferred custom change from the module is still pending.";
		case ETransactorRejectionReason::ModuleRecording: return "A custom change from the module is still being recorded.";
		case ETransactorRejectionReason::LimitsState: return "Limits can change only while idle.";
		case ETransactorRejectionReason::ZeroLimits: return "Transaction limits must be non-zero.";
		case ETransactorRejectionReason::RedoLimits: return "The requested limits cannot retain the current redo branch.";
		}
		return {};
	}

	auto FormatTransactorResult(const FTransactorResult& Result) -> std::string
	{
		std::string Message;
		if (Result.RejectionCause) Message = FormatTransactorRejection(*Result.RejectionCause);
		else if (Result.ApplyCause)
		{
			Message = FormatTransactionApplyError(Result.ApplyCause->Error);
			if (Result.Code == ETransactorResultCode::RecoveryRequired)
			{
				for (const auto& Failure : Result.ApplyCause->RollbackFailures)
					Message += std::format(" Rollback record {} failed: {}", Failure.RecordIndex, FormatTransactionRecordError(Failure.Error));
				Message += " Transaction history is disabled; reload the editor session after recovering affected data.";
			}
		}
		else if (Result.FailureCause)
		{
			const auto& Error = *Result.FailureCause;
			switch (Error.Code)
			{
			case ETransactorFailure::Unsupported: Message = "The abstract transactor does not implement this operation."; break;
			case ETransactorFailure::InactiveModify: Message = "Object modification requires an active transaction scope."; break;
			case ETransactorFailure::InvalidObject: Message = "The transaction object is invalid."; break;
			case ETransactorFailure::InactiveRecord: Message = "Recording requires an active transaction scope."; break;
			case ETransactorFailure::InactiveUpdate: Message = "Record updates require an active transaction scope."; break;
			case ETransactorFailure::ExpiredObject: Message = "A modified transaction object became invalid before commit."; break;
			case ETransactorFailure::CaptureBefore:
			case ETransactorFailure::CaptureAfter:
				Message = std::format("{} '{}': {}", Error.Code == ETransactorFailure::CaptureBefore
					? "Unable to capture" : "Unable to capture the final value of",
					Error.Member, Error.SnapshotCause ? ToString(*Error.SnapshotCause) : std::string{}); break;
			case ETransactorFailure::PrepareRecord:
			case ETransactorFailure::FinalizeRecord:
				Message = std::format("{} '{}': {}", Error.Code == ETransactorFailure::PrepareRecord
					? "Unable to prepare" : "Unable to finalize",
					Error.Member, Error.RecordCause ? FormatTransactionObjectRecordError(*Error.RecordCause) : std::string{}); break;
			case ETransactorFailure::EntryAccounting: Message = "Transaction byte accounting overflowed."; break;
			case ETransactorFailure::RetainedAccounting: Message = "Retained transaction byte accounting overflowed."; break;
			case ETransactorFailure::ByteLimit: Message = "The transaction exceeded the owned-byte limit."; break;
			case ETransactorFailure::ModuleAccounting: Message = "Retained transaction byte accounting overflowed during module drain."; break;
			case ETransactorFailure::InconsistentAccounting: Message = "Retained transaction byte accounting is inconsistent."; break;
			}
		}
		else switch (Result.Notice)
		{
		case ETransactorNotice::None: break;
		case ETransactorNotice::NoMembers: Message = "The object has no transaction-capable reflected members."; break;
		case ETransactorNotice::InactiveScope: Message = "Transaction scope is inactive."; break;
		case ETransactorNotice::NothingUndo: Message = "Nothing to undo."; break;
		case ETransactorNotice::NothingRedo: Message = "Nothing to redo."; break;
		case ETransactorNotice::ModuleDrain: Message = std::format("Removed {} module-owned transaction(s).", Result.NoticeCount); break;
		}
		if (Result.CleanupCause && Result.CleanupCause->Code == ETransactorResultCode::Rejected)
			Message += std::format(" Scope cancellation also failed: {}", FormatTransactorResult(*Result.CleanupCause));
		return Message;
	}

	auto FormatTransactionCompletionError(const FTransactionCompletionError& Error) -> std::string
	{
		if (Error.RecordCause) return FormatTransactionRecordError(*Error.RecordCause);
		if (Error.FinalizationCause) return FormatTransactorResult(*Error.FinalizationCause);
		switch (Error.Code)
		{
		case ETransactionCompletionError::None: return {};
		case ETransactionCompletionError::Operation: return "The deferred transaction operation failed.";
		case ETransactionCompletionError::Finalization: return "The deferred transaction could not be finalized.";
		}
		return {};
	}

	auto FormatTransactionRecordError(const FTransactionRecordError& Error) -> std::string
	{
		if (Error.ObjectCause) return FormatTransactionObjectRecordError(*Error.ObjectCause);
		if (Error.Code == ETransactionRecordError::MissingCustom) return "The custom transaction change is unavailable.";
		if (Error.Code == ETransactionRecordError::CustomRejected)
			return Error.CustomCause
				? FormatTransactionCustomError(*Error.CustomCause) : "The custom transaction rejected replay.";
		return {};
	}

	auto FormatTransactionApplyError(const FTransactionApplyError& Error) -> std::string
	{
		return FormatTransactionRecordError(Error.RecordCause);
	}

	auto FTransactionRecord::Validate() const -> FTransactionRecordResult
	{
		return std::visit([&](const auto& Record) -> FTransactionRecordResult {
			using T = std::decay_t<decltype(Record)>;
			if constexpr (std::is_same_v<T, FTransactionObjectRecord>)
			{
				const auto Result = Record.Validate();
				if (!Result) return {{.Code = ETransactionRecordError::Object, .ObjectCause = Result.Error}};
			}
			else if (!Record) return {{.Code = ETransactionRecordError::MissingCustom}};
			return {};
		}, Data);
	}

	auto FTransactionRecord::IsNoOp() const -> bool
	{
		if (const auto* Record = std::get_if<FTransactionObjectRecord>(&Data))
			return Record->IsNoOp();
		return false;
	}

	auto FormatTransactionCustomError(const FTransactionCustomError& Error) -> std::string
	{
		auto Message = [&]() -> std::string {
		switch (Error.Code)
		{
		case ETransactionCustomError::None: return {};
		case ETransactionCustomError::WorldEnding: return "The target World is ending play.";
		case ETransactionCustomError::ActorType: return "The actor has an unexpected type.";
		case ETransactionCustomError::ActorUnsupported:
			switch (Error.ActorConstraint)
			{
			case ETransactionActorConstraint::Class: return "The actor class is unsupported.";
			case ETransactionActorConstraint::ComponentGraph: return "The actor component graph is unsupported.";
			case ETransactionActorConstraint::Parent: return "The actor has an attached parent.";
			case ETransactionActorConstraint::Children: return "The actor has attached children.";
			case ETransactionActorConstraint::BeginningPlay: return "The actor is beginning play.";
			case ETransactionActorConstraint::EndingPlay: return "The actor is ending play.";
			case ETransactionActorConstraint::None: return "The actor has an unsupported graph.";
			}
			return "The actor has an unsupported graph.";
		case ETransactionCustomError::ActorChanged: return "The actor changed after planning.";
		case ETransactionCustomError::ActorRename: return "The actor rename failed.";
		case ETransactionCustomError::InjectedMutation: return "Injected mutation failure.";
		case ETransactionCustomError::RollbackIncomplete: return "Rollback also failed.";
		case ETransactionCustomError::ResourceUnavailable: return "The transaction resource is unavailable.";
		case ETransactionCustomError::ActorMembership: return "The actor no longer belongs to the recorded level.";
		case ETransactionCustomError::ActorNameCollision: return "The requested actor name is occupied.";
		case ETransactionCustomError::ActorSpawn: return "The level rejected actor creation.";
		case ETransactionCustomError::ActorDestroy: return "The level rejected actor removal.";
		case ETransactionCustomError::EmptySelection: return "The transaction has no participants.";
		case ETransactionCustomError::ParentMismatch: return "The actor parent changed outside history.";
		case ETransactionCustomError::ParentInvalid: return "The desired actor parent is invalid.";
		case ETransactionCustomError::ParentCycle: return "The actor attachment would introduce a cycle.";
		case ETransactionCustomError::AttachmentWrite: return "The actor attachment rejected replay.";
		case ETransactionCustomError::TransformWrite: return "The transform target rejected replay.";
		case ETransactionCustomError::MemberUnavailable:
			return Error.MemberCause ? FormatTransactionSnapshotError(*Error.MemberCause) : "The graph member is unavailable.";
		case ETransactionCustomError::MembershipChanged: return "The graph participants changed outside history.";
		case ETransactionCustomError::PropertyRestore:
			return Error.PropertyCause ? ToString(*Error.PropertyCause) : "The graph property could not be restored.";
		case ETransactionCustomError::TargetUnavailable: return "The custom transaction target is unavailable.";
		case ETransactionCustomError::PresentationWrite: return "The graph presentation rejected replay.";
		case ETransactionCustomError::MaterialWrite:
			return Error.MaterialCause ? FormatMaterialError(*Error.MaterialCause) : "The material write failed.";
		}
		return "The custom transaction failed.";
		}();
		if (Error.CleanupCause) Message += " " + FormatTransactionCustomError(*Error.CleanupCause);
		return Message;
	}

	auto FTransactionRecord::Apply(bool bBefore, EPropertyChangeOrigin Origin) -> FTransactionRecordResult
	{
		return std::visit([&](auto& Record) -> FTransactionRecordResult {
			using T = std::decay_t<decltype(Record)>;
			if constexpr (std::is_same_v<T, FTransactionObjectRecord>)
			{
				const auto Result = Record.Apply(bBefore, Origin);
				if (!Result) return {{.Code = ETransactionRecordError::Object,
					.Before = bBefore, .Origin = Origin, .ObjectCause = Result.Error}};
			}
			else
			{
				if (!Record) return {{.Code = ETransactionRecordError::MissingCustom, .Before = bBefore, .Origin = Origin}};
				if (const auto Applied = Record->Replay(bBefore ? ETransactionOperation::Undo : ETransactionOperation::Redo); !Applied)
					return {{.Code = ETransactionRecordError::CustomRejected, .Before = bBefore, .Origin = Origin,
						.CustomDescription = std::string(Record->GetDescription()),
						.CustomCause = Applied.Error}};
			}
			return {};
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

	auto FTransactionRecord::GetObjectTarget() const -> DObject*
	{
		if (const auto* Record = std::get_if<FTransactionObjectRecord>(&Data))
			return Record->GetTarget().Resolve();
		return nullptr;
	}

	FTransaction::FTransaction(FTransactionId InId, FTransactionContext InContext)
		: Id(InId)
		, Context(std::move(InContext))
	{
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
				return {.Status = ETransactionApplyStatus::ValidationFailed,
					.Error = {.Code = ETransactionApplyError::Validation,
						.TransactionId = Id, .RecordIndex = Index, .RecordCause = Result.Error}};
		}
		return {};
	}

	auto FTransaction::Apply(bool bUndo, EPropertyChangeOrigin Origin) -> FTransactionApplyResult
	{
		if (auto Result = Validate(); !Result)
		{
			Result.Error.Undo = bUndo;
			Result.Error.Origin = Origin;
			return Result;
		}
		std::vector<size_t> Applied;
		Applied.reserve(Records.size());
		for (size_t Step = 0; Step < Records.size(); ++Step)
		{
			const size_t Index = bUndo ? Records.size() - 1 - Step : Step;
			if (const auto Application = Records[Index].Apply(bUndo, Origin); !Application)
			{
				FTransactionApplyResult Result{.Status = ETransactionApplyStatus::Restored,
					.Error = {.Code = ETransactionApplyError::Execution, .TransactionId = Id,
						.RecordIndex = Index, .Undo = bUndo, .Origin = Origin, .RecordCause = Application.Error}};
				for (auto It = Applied.rbegin(); It != Applied.rend(); ++It)
				{
					if (const auto Rollback = Records[*It].Apply(!bUndo, Origin); !Rollback)
					{
						Result.Status = ETransactionApplyStatus::RecoveryRequired;
						Result.RollbackFailures.push_back({*It, Rollback.Error});
					}
				}
				return Result;
			}
			Applied.push_back(Index);
		}
		return {};
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
		{
			if (DObject* Object = Record.GetObjectTarget())
			{
				DPackage* Package = Object->GetPackage();
				if (Package && std::ranges::find(Packages, Package) == Packages.end())
					Packages.push_back(Package);
			}
			for (DPackage* Package : Record.GetAffectedPackages())
				if (Package && std::ranges::find(Packages, Package) == Packages.end())
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

	struct FScopedTransaction::FModifiedProperty
	{
		FPropertyEditTarget Target;
		FPropertyValueSnapshotPayload Before;
		uint64 RecordId = 0;
	};

	FScopedTransaction::FScopedTransaction(std::string_view Description)
		: FScopedTransaction(GEditor ? GEditor->GetTransactor() : nullptr, {
			.Name = "ScopedTransaction",
			.Description = std::string(Description),
		})
	{
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
		, ModifiedProperties(std::move(Other.ModifiedProperties))
	{
	}

	auto FScopedTransaction::operator=(FScopedTransaction&& Other) noexcept
		-> FScopedTransaction&
	{
		if (this == &Other) return *this;
		if (IsActive()) (void)End();
		Transactor = std::exchange(Other.Transactor, nullptr);
		ScopeId = std::exchange(Other.ScopeId, 0);
		ModifiedProperties = std::move(Other.ModifiedProperties);
		return *this;
	}

	auto FScopedTransaction::Modify(DObject* Object) -> void
	{
		const bool bWasActive = IsActive();
		const FTransactorResult Result = CaptureModifiedObject(Object);
		if (bWasActive && (Result.Code == ETransactorResultCode::Rejected
			|| Result.Code == ETransactorResultCode::Failed))
		{
			DURIN_ERROR("Unable to record scoped object modification: {}", FormatTransactorResult(Result));
		}
	}

	auto FScopedTransaction::CaptureModifiedObject(DObject* Object) -> FTransactorResult
	{
		if (!IsActive())
			return {.Code = ETransactorResultCode::Rejected,
				.FailureCause = FTransactorFailure{.Code = ETransactorFailure::InactiveModify}};
		if (!IsValid(Object))
			return {.Code = ETransactorResultCode::Rejected,
				.TransactionId = 0, .ScopeId = ScopeId,
				.FailureCause = FTransactorFailure{.Code = ETransactorFailure::InvalidObject, .Owner = FObjectKey(Object)}};
		if (std::ranges::any_of(ModifiedProperties, [&](const auto& Modified) {
			return Modified->Target.Object == Object;
		}))
		{
			return {.Code = ETransactorResultCode::Succeeded, .ScopeId = ScopeId};
		}

		std::vector<std::unique_ptr<FModifiedProperty>> Captured;
		std::optional<FTransactorResult> Failure;
		Object->GetClass()->ForEachProperty([&](FProperty* Property) {
			if (Failure || !Property
				|| Property->HasAnyPropertyFlags(EPropertyFlags::Transient)) return;
			for (uint32 ArrayIndex = 0; ArrayIndex < Property->GetArrayDim(); ++ArrayIndex)
			{
				auto Modified = std::make_unique<FModifiedProperty>();
				Modified->Target = FPropertyEditTarget::ForMember(Object, Property, ArrayIndex);
				if (const auto Capture = CapturePropertyValuePayload(Property, Object, ArrayIndex, Modified->Before); !Capture)
				{
					Failure = {.Code = ETransactorResultCode::Failed, .ScopeId = ScopeId,
						.FailureCause = FTransactorFailure{.Code = ETransactorFailure::CaptureBefore,
							.Owner = FObjectKey(Object), .Member = Property->NamePrivate.ToString(),
							.ArrayIndex = ArrayIndex, .SnapshotCause = Capture.error()}};
					return;
				}
				FTransactionObjectRecord ObjectRecord;
				if (const auto Capture = FTransactionObjectRecord::Capture(
						Modified->Target, Modified->Before, Modified->Before, ObjectRecord); !Capture)
				{
					Failure = {.Code = ETransactorResultCode::Failed, .ScopeId = ScopeId,
						.FailureCause = FTransactorFailure{.Code = ETransactorFailure::PrepareRecord,
							.Owner = FObjectKey(Object), .Member = Property->NamePrivate.ToString(),
							.ArrayIndex = ArrayIndex, .RecordCause = Capture.Error}};
					return;
				}
				FTransactorResult RecordResult = Record(std::move(ObjectRecord));
				if (!RecordResult)
				{
					Failure = std::move(RecordResult);
					return;
				}
				Modified->RecordId = RecordResult.RecordId;
				Captured.push_back(std::move(Modified));
			}
		});
		if (Failure)
		{
			if (IsActive())
			{
				const FTransactorResult CancelResult = Cancel();
				Failure->CleanupCause = std::make_shared<FTransactorResult>(CancelResult);
			}
			return std::move(*Failure);
		}
		if (Captured.empty())
			return {.Code = ETransactorResultCode::NoOp, .ScopeId = ScopeId,
				.Notice = ETransactorNotice::NoMembers};
		ModifiedProperties.insert(ModifiedProperties.end(),
			std::make_move_iterator(Captured.begin()),
			std::make_move_iterator(Captured.end()));
		return {.Code = ETransactorResultCode::Succeeded, .ScopeId = ScopeId};
	}

	auto FScopedTransaction::Record(FTransactionObjectRecord Record) -> FTransactorResult
	{
		if (!IsActive())
			return {.Code = ETransactorResultCode::Rejected,
				.FailureCause = FTransactorFailure{.Code = ETransactorFailure::InactiveRecord}};
		return Transactor->Record(ScopeId, std::move(Record));
	}

	auto FScopedTransaction::UpdateRecord(
		uint64 RecordId,
		FTransactionObjectRecord Record) -> FTransactorResult
	{
		if (!IsActive())
			return {.Code = ETransactorResultCode::Rejected,
				.FailureCause = FTransactorFailure{.Code = ETransactorFailure::InactiveUpdate}};
		return Transactor->UpdateRecord(ScopeId, RecordId, std::move(Record));
	}

	auto FScopedTransaction::PrepareModifiedRecords() -> FTransactorResult
	{
		for (const auto& Modified : ModifiedProperties)
		{
			if (!IsValid(Modified->Target.Object))
				return {.Code = ETransactorResultCode::Failed, .ScopeId = ScopeId,
					.FailureCause = FTransactorFailure{.Code = ETransactorFailure::ExpiredObject, .Owner = FObjectKey(Modified->Target.Object)}};
			FPropertyValueSnapshotPayload After;
			if (const auto Capture = CapturePropertyValuePayload(Modified->Target.SnapshotProperty,
				Modified->Target.SnapshotContainer, Modified->Target.SnapshotArrayIndex, After); !Capture)
			{
				return {.Code = ETransactorResultCode::Failed, .ScopeId = ScopeId,
					.FailureCause = FTransactorFailure{.Code = ETransactorFailure::CaptureAfter,
						.Owner = FObjectKey(Modified->Target.Object),
						.Member = Modified->Target.MemberProperty->NamePrivate.ToString(),
						.ArrayIndex = Modified->Target.SnapshotArrayIndex, .SnapshotCause = Capture.error()}};
			}
			FTransactionObjectRecord Record;
			if (const auto Capture = FTransactionObjectRecord::Capture(Modified->Target,
					Modified->Before, std::move(After), Record); !Capture)
			{
				return {.Code = ETransactorResultCode::Failed, .ScopeId = ScopeId,
					.FailureCause = FTransactorFailure{.Code = ETransactorFailure::FinalizeRecord,
						.Owner = FObjectKey(Modified->Target.Object),
						.Member = Modified->Target.MemberProperty->NamePrivate.ToString(),
						.ArrayIndex = Modified->Target.SnapshotArrayIndex, .RecordCause = Capture.Error}};
			}
			FTransactorResult Result = UpdateRecord(Modified->RecordId, std::move(Record));
			if (!Result) return Result;
		}
		return {.Code = ETransactorResultCode::Succeeded, .ScopeId = ScopeId};
	}

	auto FScopedTransaction::End() -> FTransactorResult
	{
		if (!IsActive())
			return {.Code = ETransactorResultCode::NoOp, .Notice = ETransactorNotice::InactiveScope};
		const FTransactorResult Prepared = PrepareModifiedRecords();
		if (!Prepared)
		{
			FTransactorResult Failure = Prepared;
			FTransactorResult CancelResult = Cancel();
			if (CancelResult.Code == ETransactorResultCode::Rejected)
			{
				CancelResult.PreparationCause = std::make_shared<FTransactorResult>(Prepared);
				return CancelResult;
			}
			Failure.CleanupCause = std::make_shared<FTransactorResult>(std::move(CancelResult));
			return Failure;
		}
		const FTransactorResult Result = Transactor->End(ScopeId);
		if (Result.Code != ETransactorResultCode::Rejected)
		{
			Transactor = nullptr;
			ScopeId = 0;
			ModifiedProperties.clear();
		}
		return Result;
	}

	auto FScopedTransaction::Cancel() -> FTransactorResult
	{
		if (!IsActive())
			return {.Code = ETransactorResultCode::NoOp, .Notice = ETransactorNotice::InactiveScope};
		const FTransactorResult Result = Transactor->Cancel(ScopeId);
		if (Result.Code != ETransactorResultCode::Rejected)
		{
			Transactor = nullptr;
			ScopeId = 0;
			ModifiedProperties.clear();
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
				.FailureCause = FTransactorFailure{.Code = ETransactorFailure::Unsupported}};
		}
	}

	auto DTransactor::Begin(const FTransactionContext&) -> FTransactorResult { return Unsupported(); }
	auto DTransactor::Record(FTransactionScopeId, FTransactionObjectRecord) -> FTransactorResult { return Unsupported(); }
	auto DTransactor::Execute(std::unique_ptr<ITransactionCustomChange>, bool) -> FTransactorResult { return Unsupported(); }
	auto DTransactor::CommitApplied(std::unique_ptr<ITransactionCustomChange> Change)
		-> FTransactorResult
	{
		return Execute(std::move(Change), true);
	}
	auto DTransactor::UpdateRecord(FTransactionScopeId, uint64, FTransactionObjectRecord) -> FTransactorResult { return Unsupported(); }
	auto DTransactor::End(FTransactionScopeId) -> FTransactorResult { return Unsupported(); }
	auto DTransactor::Cancel(FTransactionScopeId) -> FTransactorResult { return Unsupported(); }
	auto DTransactor::Undo() -> FTransactorResult { return Unsupported(); }
	auto DTransactor::Undo(FTransactionId) -> FTransactorResult { return Unsupported(); }
	auto DTransactor::Redo() -> FTransactorResult { return Unsupported(); }
	auto DTransactor::Redo(FTransactionId) -> FTransactorResult { return Unsupported(); }
	auto DTransactor::Reset() -> FTransactorResult { return Unsupported(); }
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
		std::string_view ModuleName, std::optional<FTransactionBufferLimits> RequestedLimits) const -> FTransactorResult
	{
		FTransactorRejection Rejection{.Reason = Reason, .State = State,
			.RequestedId = RequestedId, .PendingId = Pending ? Pending->GetId() : PendingTransactionId,
			.ScopeId = Savepoints.empty() ? 0 : Savepoints.back().ScopeId,
			.UndoHead = GetUndoId(), .RedoHead = GetRedoId(),
			.HistoryCount = History.size(), .OwnedBytes = OwnedBytes,
			.Limits = Limits, .RequestedLimits = RequestedLimits, .ModuleName = std::string(ModuleName)};
		return {.Code = ETransactorResultCode::Rejected,
			.RejectionCause = std::move(Rejection)};
	}

	auto DTransBuffer::Begin(const FTransactionContext& Context) -> FTransactorResult
	{
		CheckThread();
		if (State != ETransactorState::Idle && State != ETransactorState::Recording)
			return Reject(ETransactorRejectionReason::BeginState);
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
				.FailureCause = FTransactorFailure{.Code = ETransactorFailure::EntryAccounting}};
		}
		Savepoints.push_back({ScopeId, Pending->GetRecordCount(), PendingBytes});
		return {.Code = ETransactorResultCode::Succeeded,
			.TransactionId = Pending->GetId(), .ScopeId = ScopeId};
	}

	auto DTransBuffer::Record(
		FTransactionScopeId ScopeId,
		FTransactionObjectRecord Record) -> FTransactorResult
	{
		CheckThread();
		if (State != ETransactorState::Recording || !Pending || Savepoints.empty())
			return Reject(ETransactorRejectionReason::RecordScope, ScopeId);
		if (Savepoints.back().ScopeId != ScopeId)
			return Reject(ETransactorRejectionReason::RecordOrder, ScopeId);
		// Capture the initial checkpoint before the caller applies the property edit.
		// Finalization runs after mutation and may already see a dirty package.
		if (DObject* Object = Record.GetTarget().Resolve())
			if (DPackage* Package = Object->GetPackage(); Package && Package->IsAssetPackage())
				EnsurePackageState(*Package);
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
			return Reject(ETransactorRejectionReason::ExecuteState);
		if (!Change) return Reject(ETransactorRejectionReason::MissingCustom);

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
				[this, TransactionId](FTransactionCompletionResult Result) {
					CompleteDeferredOperation(
						ETransactionOperation::Execute, TransactionId, std::move(Result));
				});
			auto ApplyResult = Transaction.Apply(false, EPropertyChangeOrigin::Edit);
			if (!ApplyResult)
			{
				Transaction.SetDeferredOperationCompletion({});
				PendingTransactionId = 0;
				return HandleApplyFailure(Transaction, std::move(ApplyResult));
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
		FTransactionScopeId ScopeId,
		uint64 RecordId,
		FTransactionObjectRecord Record) -> FTransactorResult
	{
		CheckThread();
		if (State != ETransactorState::Recording || !Pending || Savepoints.empty())
			return Reject(ETransactorRejectionReason::UpdateScope, ScopeId);
		if (Savepoints.back().ScopeId != ScopeId)
			return Reject(ETransactorRejectionReason::UpdateOrder, ScopeId);
		if (!Pending->UpdateRecord(RecordId, std::move(Record)))
			return Reject(ETransactorRejectionReason::MissingRecord, RecordId);
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
			return Reject(ETransactorRejectionReason::CloseScope, ScopeId);
		if (Savepoints.back().ScopeId != ScopeId)
			return Reject(ETransactorRejectionReason::CloseOrder, ScopeId);

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
		Pending->RemoveNoOpRecords();
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
				.FailureCause = FTransactorFailure{.Code = ETransactorFailure::EntryAccounting}};
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
				.FailureCause = FTransactorFailure{.Code = ETransactorFailure::RetainedAccounting, .Actual = OwnedBytes}};
		}
		if (EntryBytes > Limits.MaximumOwnedBytes)
		{
			QueueEvent(ETransactionEventType::Discarded, *Pending,
				"The transaction exceeded the owned-byte limit.");
			Pending.reset();
			RecalculateOwnedBytes();
			State = ETransactorState::Idle;
			return {.Code = ETransactorResultCode::Discarded, .TransactionId = TransactionId,
				.FailureCause = FTransactorFailure{.Code = ETransactorFailure::ByteLimit, .Actual = EntryBytes, .Limit = Limits.MaximumOwnedBytes}};
		}
		if (OwnedBytes > std::numeric_limits<size_t>::max() - EntryBytes)
		{
			QueueEvent(ETransactionEventType::Failed, *Pending,
				"Retained transaction byte accounting overflowed.");
			Pending.reset();
			State = ETransactorState::Idle;
			return {.Code = ETransactorResultCode::Failed, .TransactionId = TransactionId,
				.FailureCause = FTransactorFailure{.Code = ETransactorFailure::RetainedAccounting, .Actual = OwnedBytes}};
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
		if (State != ETransactorState::Idle) return Reject(ETransactorRejectionReason::UndoState, ExpectedId);
		if (Cursor == 0) return {.Code = ETransactorResultCode::NoOp, .Notice = ETransactorNotice::NothingUndo};
		FTransaction& Transaction = History[Cursor - 1];
		if (ExpectedId != 0 && Transaction.GetId() != ExpectedId)
			return Reject(ETransactorRejectionReason::UndoHead, ExpectedId);
		State = ETransactorState::Undoing;
		PendingTransactionId = Transaction.GetId();
		PendingOperation = ETransactionOperation::Undo;
		Transaction.SetDeferredOperationCompletion(
			[this, TransactionId = Transaction.GetId()](FTransactionCompletionResult Result) {
				CompleteDeferredOperation(
					ETransactionOperation::Undo, TransactionId, std::move(Result));
			});
		auto ApplyResult = Transaction.Apply(true, EPropertyChangeOrigin::Undo);
		if (!ApplyResult)
		{
			Transaction.SetDeferredOperationCompletion({});
			PendingTransactionId = 0;
			return HandleApplyFailure(Transaction, std::move(ApplyResult));
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
		if (State != ETransactorState::Idle) return Reject(ETransactorRejectionReason::RedoState, ExpectedId);
		if (Cursor >= History.size())
			return {.Code = ETransactorResultCode::NoOp, .Notice = ETransactorNotice::NothingRedo};
		FTransaction& Transaction = History[Cursor];
		if (ExpectedId != 0 && Transaction.GetId() != ExpectedId)
			return Reject(ETransactorRejectionReason::RedoHead, ExpectedId);
		State = ETransactorState::Redoing;
		PendingTransactionId = Transaction.GetId();
		PendingOperation = ETransactionOperation::Redo;
		Transaction.SetDeferredOperationCompletion(
			[this, TransactionId = Transaction.GetId()](FTransactionCompletionResult Result) {
				CompleteDeferredOperation(
					ETransactionOperation::Redo, TransactionId, std::move(Result));
			});
		auto ApplyResult = Transaction.Apply(false, EPropertyChangeOrigin::Redo);
		if (!ApplyResult)
		{
			Transaction.SetDeferredOperationCompletion({});
			PendingTransactionId = 0;
			return HandleApplyFailure(Transaction, std::move(ApplyResult));
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

	auto DTransBuffer::HandleApplyFailure(const FTransaction& Transaction, FTransactionApplyResult Result) -> FTransactorResult
	{
		const bool bRecoveryRequired = Result.Status == ETransactionApplyStatus::RecoveryRequired;
		State = bRecoveryRequired ? ETransactorState::RecoveryRequired : ETransactorState::Idle;
		FTransactorResult Failure{.Code = bRecoveryRequired ? ETransactorResultCode::RecoveryRequired : ETransactorResultCode::Failed,
			.TransactionId = Transaction.GetId(), .RollbackFailures = Result.RollbackFailures,
			.ApplyCause = std::make_shared<FTransactionApplyResult>(std::move(Result))};
		const std::string Message = FormatTransactorResult(Failure);
		if (bRecoveryRequired)
		{
			for (const auto& Transition : Transaction.GetPackageTransitions())
				if (auto* Package = Cast<DPackage>(Transition.Package.Resolve())) InvalidateSavedState(*Package);
			DURIN_ERROR("Transaction {} requires recovery: {}", Transaction.GetId(), Message);
		}
		QueueEvent(ETransactionEventType::Failed, Transaction, Message);
		return Failure;
	}

	auto DTransBuffer::Reset() -> FTransactorResult
	{
		CheckThread();
		if (State != ETransactorState::Idle) return Reject(ETransactorRejectionReason::ResetState);
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
		FTransactionCompletionResult CompletionResult) -> void
	{
		if (PendingTransactionId != TransactionId || PendingOperation != Operation)
			return;
		FTransaction* Transaction = Operation == ETransactionOperation::Execute
			? (Pending ? &*Pending : nullptr)
			: FindTransaction(TransactionId);
		if (!Transaction) return;
		Transaction->SetDeferredOperationCompletion({});
		PendingTransactionId = 0;
		if (!CompletionResult)
		{
			CompletionResult.Error.TransactionId = TransactionId;
			CompletionResult.Error.Operation = Operation;
			QueueEvent(ETransactionEventType::Failed, *Transaction,
				FormatTransactionCompletionError(CompletionResult.Error));
			if (Operation == ETransactionOperation::Execute) Pending.reset();
			State = ETransactorState::Idle;
			if (TransactionCompletion)
				std::exchange(TransactionCompletion, {})(std::move(CompletionResult));
			return;
		}
		if (Operation == ETransactionOperation::Execute)
		{
			State = ETransactorState::Recording;
			const FTransactorResult Result = FinalizePending();
			if (TransactionCompletion)
			{
				FTransactionCompletionResult Finalized;
				if (!Result.IsSuccess())
					Finalized.Error = {.Code = ETransactionCompletionError::Finalization,
						.TransactionId = TransactionId, .Operation = Operation,
						.FinalizationCause = std::make_shared<FTransactorResult>(Result)};
				std::exchange(TransactionCompletion, {})(std::move(Finalized));
			}
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
			std::exchange(TransactionCompletion, {})({});
	}

	auto DTransBuffer::SetTransactionCompletion(
		FTransactionId TransactionId,
		FTransactionDeferredCompletion Completion) -> FTransactorResult
	{
		CheckThread();
		if (PendingTransactionId != TransactionId)
			return Reject(ETransactorRejectionReason::CompletionIdentity, TransactionId);
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
		if (ModuleName.empty()) return Reject(ETransactorRejectionReason::ModuleName, 0, ModuleName);
		if (PendingTransactionId != 0)
		{
			const FTransaction* Active = FindTransaction(PendingTransactionId);
			if (Active && Active->IsOwnedByModule(ModuleName))
				return Reject(ETransactorRejectionReason::ModulePending, 0, ModuleName);
		}
		if (State == ETransactorState::Recording && Pending
			&& Pending->IsOwnedByModule(ModuleName))
			return Reject(ETransactorRejectionReason::ModuleRecording, 0, ModuleName);
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

	auto DTransBuffer::SetLimits(FTransactionBufferLimits InLimits) -> FTransactorResult
	{
		CheckThread();
		if (State != ETransactorState::Idle) return Reject(ETransactorRejectionReason::LimitsState, 0, {}, InLimits);
		if (InLimits.MaximumEntries == 0 || InLimits.MaximumOwnedBytes == 0)
			return Reject(ETransactorRejectionReason::ZeroLimits, 0, {}, InLimits);
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
					.FailureCause = FTransactorFailure{.Code = ETransactorFailure::InconsistentAccounting, .Actual = EntryBytes, .Limit = ProjectedBytes, .Index = RemovalCount}};
			}
			ProjectedBytes -= EntryBytes;
			--ProjectedCount;
			++RemovalCount;
		}
		if (ProjectedCount > InLimits.MaximumEntries
			|| ProjectedBytes > InLimits.MaximumOwnedBytes)
		{
			return Reject(ETransactorRejectionReason::RedoLimits, 0, {}, InLimits);
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
							&& std::ranges::find(Packages, Affected) == Packages.end()
							&& std::ranges::find(OtherPackages, Affected) == OtherPackages.end())
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
