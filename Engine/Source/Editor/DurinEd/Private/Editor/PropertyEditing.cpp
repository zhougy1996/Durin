#include "Editor/PropertyEditing.h"

#include "Misc/Failure.h"
#include "Editor/PropertyValueDraft.h"

#include "DObject/DObjectArray.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/Object.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Property.h"
#include "DObject/WeakObjectPtr.h"
#include "Logging/LogMacros.h"
#include "Misc/AssertionMacros.h"

namespace Durin::Editor
{
	namespace
	{
		std::mutex GPropertyEditExtensionsMutex;
		std::unordered_map<FPropertyEditExtensionHandle, FPropertyEditExtension>
			GPropertyEditExtensions;
		FPropertyEditExtensionHandle GNextPropertyEditExtensionHandle = 1;

		auto SnapshotPropertyEditExtensions() -> std::vector<FPropertyEditExtension>
		{
			std::lock_guard Lock(GPropertyEditExtensionsMutex);
			std::vector<FPropertyEditExtension> Extensions;
			Extensions.reserve(GPropertyEditExtensions.size());
			for (const auto& [Handle, Extension] : GPropertyEditExtensions)
				Extensions.push_back(Extension);
			return Extensions;
		}

		struct FResolveMapEntryContext
		{
			FProperty* KeyProperty = nullptr;
			const FPropertyValueSnapshot* TargetKey = nullptr;
			const void* Key = nullptr;
			void* Value = nullptr;
			std::string Error;
		};

		auto ResolveMapEntry(void* RawContext, const void* Key, void* Value) -> bool
		{
			auto& Context = *static_cast<FResolveMapEntryContext*>(RawContext);
			FPropertyValueSnapshot StoredKey;
			if (!CapturePropertyValue(Context.KeyProperty, Key, 0, StoredKey, &Context.Error)) return false;
			if (StoredKey == *Context.TargetKey)
			{
				Context.Key = Key;
				Context.Value = Value;
				return false;
			}
			return true;
		}

		auto CaptureTargetValue(const FPropertyEditTarget& Target,
			FPropertyValueSnapshot& OutSnapshot, std::string* OutError) -> bool
		{
			return CapturePropertyValue(
				Target.SnapshotProperty, Target.SnapshotContainer, Target.SnapshotArrayIndex, OutSnapshot, OutError);
		}

		auto RestoreTargetValue(const FPropertyEditTarget& Target,
			const FPropertyValueSnapshot& Snapshot, std::string* OutError) -> bool
		{
			return RestorePropertyValue(
				Target.SnapshotProperty, Target.SnapshotContainer, Target.SnapshotArrayIndex, Snapshot, OutError);
		}

		auto MakeEventPath(const FPropertyEditTarget& Target) -> std::vector<FPropertyPathSegment>
		{
			std::vector<FPropertyPathSegment> Result;
			Result.reserve(Target.Path.size());
			for (const FPropertyEditPathSegment& Segment : Target.Path)
				Result.push_back({Segment.Property, Segment.Selector, Segment.Index, Segment.MapKeyData});
			return Result;
		}

		thread_local std::vector<const FPropertyEditTarget*> GActiveGenericMutations;

		// Balances pre/post mutation callbacks even when a container edit fails.
		class FGenericMutationScope
		{
		public:
			explicit FGenericMutationScope(const FPropertyEditTarget& Target)
				: Target(&Target) { GActiveGenericMutations.push_back(this->Target); }
			~FGenericMutationScope() { GActiveGenericMutations.pop_back(); }
		private:
			const FPropertyEditTarget* Target;
		};

		struct FDeferredMutation
		{
			FPropertyValueSnapshot ProposedValue;
			FPropertyEditDeferredAction Action;
		};

		auto ApplyGenericMutation(
			const FPropertyEditTarget& Target,
			const FPropertyValueSnapshot& ProposedValue,
			EPropertyChangePhase Phase,
			EPropertyChangeOrigin Origin,
			FPropertyValueSnapshot* OutAppliedValue,
			FDeferredMutation* OutDeferred,
			std::string* OutError
		) -> bool
		{
			if (std::ranges::any_of(GActiveGenericMutations, [&](const auto* Active) { return Active->IsSameMutationTarget(Target); }))
				return Fail(OutError, "A reflected property hook cannot start a nested edit of the same target.");
			FGenericMutationScope Scope(Target);

			FPropertyValueSnapshot Before;
			if (!CaptureTargetValue(Target, Before, OutError)) return false;
			FPropertyValueDraft Draft(Target, OutError);
			if (!Draft.IsValid() || !Draft.Restore(ProposedValue, OutError)) return false;

			FResolvedPropertyValue DraftLeaf;
			const bool bResolvedLeaf = Draft.Resolve(Target, DraftLeaf.Property, DraftLeaf.Container, DraftLeaf.ArrayIndex, nullptr);
			if (!bResolvedLeaf && Target.Kind != EPropertyChangeKind::MapKeyRename
				&& Target.Kind != EPropertyChangeKind::MapRemove)
				return Fail(OutError, "The detached property proposal leaf could not be resolved.");

			std::vector<FPropertyPathSegment> EventPath = MakeEventPath(Target);
			FPropertyEditProposal Proposal{
				Target.MemberProperty,
				Target.LeafProperty,
				EventPath,
				Phase,
				Target.Kind,
				Origin,
				Draft.GetRootProperty(),
				Draft.GetRootContainer(),
				Draft.GetRootArrayIndex(),
				bResolvedLeaf ? DraftLeaf.Container : nullptr,
				bResolvedLeaf ? DraftLeaf.ArrayIndex : 0
			};
			std::string HookError;
			for (const FPropertyEditExtension& Extension : SnapshotPropertyEditExtensions())
			{
				if (Extension.PreEdit && !Extension.PreEdit(*Target.Object, Proposal, HookError))
				{
					if (OutError) *OutError = HookError.empty()
						? "A property edit extension rejected the reflected property proposal."
						: HookError;
					return false;
				}
			}
			if (!Target.Object->PreEditChangeProperty(Proposal, HookError))
			{
				if (OutError) *OutError = HookError.empty() ? "The object rejected the reflected property proposal." : HookError;
				return false;
			}

			FPropertyValueSnapshot Normalized;
			if (!Draft.Capture(Normalized, OutError)) return false;
			if (Proposal.DeferredAction)
			{
				if (!OutDeferred)
					return Fail(OutError, "The reflected-property caller cannot retain deferred validation.");
				OutDeferred->ProposedValue = std::move(Normalized);
				OutDeferred->Action = std::move(Proposal.DeferredAction);
				if (OutAppliedValue) *OutAppliedValue = std::move(Before);
				return true;
			}
			std::string ApplyError;
			if (!RestoreTargetValue(Target, Normalized, &ApplyError))
			{
				std::string RollbackError;
				const bool bRolledBack = RestoreTargetValue(Target, Before, &RollbackError);
				if (!bRolledBack)
					ApplyError += std::format(" Rollback also failed: {}", RollbackError);
				if (OutAppliedValue)
				{
					if (bRolledBack) *OutAppliedValue = Before;
					else CaptureTargetValue(Target, *OutAppliedValue, nullptr);
				}
				if (OutError) *OutError = ApplyError;
				return false;
			}

			FPropertyValueSnapshot Applied;
			std::string CaptureError;
			if (!CaptureTargetValue(Target, Applied, &CaptureError))
			{
				std::string RollbackError;
				const bool bRolledBack = RestoreTargetValue(Target, Before, &RollbackError);
				if (!bRolledBack)
					CaptureError += std::format(" Rollback also failed: {}", RollbackError);
				if (OutAppliedValue)
				{
					if (bRolledBack) *OutAppliedValue = Before;
					else CaptureTargetValue(Target, *OutAppliedValue, nullptr);
				}
				if (OutError) *OutError = CaptureError;
				return false;
			}
			if (OutAppliedValue) *OutAppliedValue = std::move(Applied);
			return true;
		}

		auto ValidateTarget(const FPropertyEditTarget& Target, std::string* OutError) -> bool
		{
			if (!Target.Object) return Fail(OutError, "The edit target has no owning object.");
			if (!Target.MemberProperty || !Target.LeafProperty
				|| !Target.SnapshotProperty || !Target.SnapshotContainer) return Fail(OutError, "The edit target is incomplete.");
			if (Target.SnapshotArrayIndex >= Target.SnapshotProperty->GetArrayDim()) return Fail(OutError, "The snapshot property array index is out of range.");
			if (Target.Path.empty() || Target.Path.front().Property != Target.MemberProperty || Target.Path.back().Property != Target.LeafProperty)
			{
				return Fail(OutError, "The property path must run from the member property to the leaf property.");
			}
			for (const FPropertyEditPathSegment& Segment : Target.Path)
			{
				if (!Segment.Property) return Fail(OutError, "The property path contains an empty segment.");
				if (Segment.Selector != EPropertyPathSelector::MapKey && !Segment.MapKeyData.empty())
				{
					return Fail(OutError, "Only map-key path segments may contain serialized key data.");
				}
			}
			return true;
		}

		// Identifies the container mutation applied while resolving an edit path.
		enum class EMutationOperation : uint8
		{
			Apply,
			NotifyOnly,
		};

		// Reports whether a container mutation changed storage and its resulting index.
		struct FMutationExecutionResult
		{
			FPropertyValueSnapshot AppliedValue;
			FDeferredMutation Deferred;
			bool bSucceeded = false;
			bool bChanged = false;
			bool bDeferred = false;
		};

		auto NotifyMutation(
			const FPropertyEditTarget& Target,
			EPropertyChangePhase Phase,
			EPropertyChangeOrigin Origin
		) -> void
		{
			std::vector<FPropertyPathSegment> EventPath = MakeEventPath(Target);
			Target.Object->PostEditChangeProperty({
				Target.MemberProperty,
				Target.LeafProperty,
				EventPath,
				Phase,
				Target.Kind,
				Origin
			});
			const FPropertyChangedEvent Event{
				Target.MemberProperty,
				Target.LeafProperty,
				std::move(EventPath),
				Phase,
				Target.Kind,
				Origin};
			for (const FPropertyEditExtension& Extension : SnapshotPropertyEditExtensions())
				if (Extension.PostEdit) Extension.PostEdit(*Target.Object, Event);
		}

		auto ApplyDeferredMutation(
			const FPropertyEditTarget& Target,
			const FPropertyValueSnapshot& ProposedValue,
			EPropertyChangePhase Phase,
			EPropertyChangeOrigin Origin,
			std::string* OutError) -> FMutationExecutionResult
		{
			FMutationExecutionResult Result;
			if (!RestoreTargetValue(Target, ProposedValue, OutError)) return Result;
			if (!CaptureTargetValue(Target, Result.AppliedValue, OutError)) return Result;
			Result.bSucceeded = true;
			Result.bChanged = true;
			NotifyMutation(Target, Phase, Origin);
			return Result;
		}

		auto ExecuteMutation(
			const FPropertyEditTarget& Target,
			const FPropertyValueSnapshot* Value,
			const FPropertyValueSnapshot* PreviousValue,
			EMutationOperation Operation,
			EPropertyChangePhase Phase,
			EPropertyChangeOrigin Origin,
			std::string* OutError
		) -> FMutationExecutionResult
		{
			FMutationExecutionResult Result;
			if (Operation == EMutationOperation::NotifyOnly)
			{
				NotifyMutation(Target, Phase, Origin);
				Result.bSucceeded = true;
				return Result;
			}
			if (!Value)
			{
				Fail(OutError, "The reflected-property mutation value is unavailable.");
				return Result;
			}

			if (!ApplyGenericMutation(
				Target, *Value, Phase, Origin, &Result.AppliedValue, &Result.Deferred, OutError))
				return Result;

			Result.bSucceeded = true;
			if (Result.Deferred.Action)
			{
				Result.bDeferred = true;
				return Result;
			}
			Result.bChanged = !PreviousValue || !(Result.AppliedValue == *PreviousValue);
			if (Phase != EPropertyChangePhase::Interactive || Result.bChanged) NotifyMutation(Target, Phase, Origin);
			return Result;
		}
	}

	auto RegisterPropertyEditExtension(FPropertyEditExtension Extension)
		-> FPropertyEditExtensionHandle
	{
		if (!Extension.PreEdit && !Extension.PostEdit) return 0;
		std::lock_guard Lock(GPropertyEditExtensionsMutex);
		const FPropertyEditExtensionHandle Handle = GNextPropertyEditExtensionHandle++;
		GPropertyEditExtensions.emplace(Handle, std::move(Extension));
		return Handle;
	}

	auto UnregisterPropertyEditExtension(FPropertyEditExtensionHandle Handle) -> void
	{
		if (Handle == 0) return;
		std::lock_guard Lock(GPropertyEditExtensionsMutex);
		GPropertyEditExtensions.erase(Handle);
	}

	struct FPropertyEditSession::FDeferredOwnerState
	{
		std::mutex Mutex;
		FPropertyEditSession* Owner = nullptr;
	};

	auto ResolveReflectedPropertyValue(
		const FPropertyEditTarget& Target,
		FResolvedPropertyValue& OutValue,
		std::string* OutError
	) -> bool
	{
		if (!ValidateTarget(Target, OutError)) return false;
		if (Target.Path.front().Property != Target.SnapshotProperty)
			return Fail(OutError, "The property path does not begin at the snapshot root.");

		void* Container = Target.SnapshotContainer;
		uint32 CurrentArrayIndex = Target.SnapshotArrayIndex;
		for (size_t PathIndex = 0; PathIndex < Target.Path.size(); ++PathIndex)
		{
			const FPropertyEditPathSegment& Segment = Target.Path[PathIndex];
			auto* CurrentProperty = const_cast<FProperty*>(Segment.Property);
			if (PathIndex + 1 == Target.Path.size())
			{
				OutValue = {CurrentProperty, Container, CurrentArrayIndex};
				return true;
			}

			FProperty* NextProperty = const_cast<FProperty*>(Target.Path[PathIndex + 1].Property);
			switch (Segment.Selector)
			{
			case EPropertyPathSelector::None:
			case EPropertyPathSelector::StaticArrayIndex:
				Container = CurrentProperty->GetValuePtr(Container, CurrentArrayIndex);
				CurrentArrayIndex = Target.Path[PathIndex + 1].Selector == EPropertyPathSelector::StaticArrayIndex
					? static_cast<uint32>(Target.Path[PathIndex + 1].Index) : 0;
				break;
			case EPropertyPathSelector::ArrayIndex:
			{
				auto* ArrayProperty = CurrentProperty->GetKind() == DurinCodeGen::EPropertyGenFlags::Array
					? static_cast<FArrayProperty*>(CurrentProperty) : nullptr;
				uint64 Num = 0;
				if (!ArrayProperty || ArrayProperty->GetNum(Container, Num, CurrentArrayIndex) != EContainerOpResult::Success
					|| Segment.Index >= Num)
					return Fail(OutError, "The reflected array path index is unavailable.");
				void* Element = nullptr;
				if (ArrayProperty->GetMutableElement(Container, Segment.Index, &Element, CurrentArrayIndex)
					!= EContainerOpResult::Success)
					return Fail(OutError, "The reflected array path requires mutable random access.");
				Container = Element;
				CurrentArrayIndex = 0;
				break;
			}
			case EPropertyPathSelector::MapKey:
			{
				auto* MapProperty = CurrentProperty->GetKind() == DurinCodeGen::EPropertyGenFlags::Map
					? static_cast<FMapProperty*>(CurrentProperty) : nullptr;
				if (!MapProperty || !Segment.MapKey.IsValid())
					return Fail(OutError, "The reflected map path lacks a stable key snapshot.");
				FResolveMapEntryContext ResolveContext{MapProperty->GetKeyProp(), &Segment.MapKey};
				const EContainerOpResult VisitResult = MapProperty->VisitMutableEntries(
					Container, &ResolveMapEntry, &ResolveContext, CurrentArrayIndex);
				if (VisitResult != EContainerOpResult::Success)
					return Fail(OutError, "The reflected map path requires mutable mapped traversal.");
				if (!ResolveContext.Error.empty()) return Fail(OutError, ResolveContext.Error);
				if (!ResolveContext.Key) return Fail(OutError, "The reflected map key is unavailable.");
				if (NextProperty == MapProperty->GetKeyProp())
					Container = const_cast<void*>(ResolveContext.Key);
				else if (NextProperty == MapProperty->GetValueProp())
					Container = ResolveContext.Value;
				else
					return Fail(OutError, "The reflected map path does not select its key or value property.");
				CurrentArrayIndex = 0;
				break;
			}
			default:
				return Fail(OutError, "The reflected property path selector is unsupported.");
			}
			if (!Container || !NextProperty) return Fail(OutError, "The reflected property path could not be resolved.");
		}
		return Fail(OutError, "The reflected property path is empty.");
	}

	auto FPropertyEditTarget::ForMember(DObject* Object, const FProperty* Property, uint32 ArrayIndex) -> FPropertyEditTarget
	{
		FPropertyEditTarget Target;
		Target.Object = Object;
		Target.MemberProperty = Property;
		Target.LeafProperty = Property;
		Target.SnapshotProperty = Property;
		Target.SnapshotContainer = Object;
		Target.SnapshotArrayIndex = ArrayIndex;
		Target.Path.push_back({
			Property,
			Property && Property->GetArrayDim() > 1 ? EPropertyPathSelector::StaticArrayIndex : EPropertyPathSelector::None,
			ArrayIndex
		});
		return Target;
	}

	auto FPropertyEditTarget::ForStructMember(const FProperty* Property, uint32 ArrayIndex) const -> FPropertyEditTarget
	{
		FPropertyEditTarget Target = *this;
		Target.LeafProperty = Property;
		Target.Path.push_back({
			Property,
			Property && Property->GetArrayDim() > 1 ? EPropertyPathSelector::StaticArrayIndex : EPropertyPathSelector::None,
			ArrayIndex
		});
		Target.Kind = EPropertyChangeKind::ValueSet;
		return Target;
	}

	auto FPropertyEditTarget::ForArrayElement(const FProperty* ElementProperty, uint64 ElementIndex) const -> FPropertyEditTarget
	{
		FPropertyEditTarget Target = *this;
		Target.LeafProperty = ElementProperty;
		if (!Target.Path.empty())
		{
			Target.Path.back().Selector = EPropertyPathSelector::ArrayIndex;
			Target.Path.back().Index = ElementIndex;
		}
		Target.Path.push_back({ElementProperty});
		Target.Kind = EPropertyChangeKind::ValueSet;
		return Target;
	}

	auto FPropertyEditTarget::ForMapEntry(const FProperty* EntryProperty, std::vector<uint8> SerializedKey) const -> FPropertyEditTarget
	{
		return ForMapEntry(EntryProperty, {}, std::move(SerializedKey));
	}

	auto FPropertyEditTarget::ForMapEntry(const FProperty* EntryProperty,
		FPropertyValueSnapshot KeySnapshot, std::vector<uint8> SerializedKey) const -> FPropertyEditTarget
	{
		FPropertyEditTarget Target = *this;
		Target.LeafProperty = EntryProperty;
		if (!Target.Path.empty())
		{
			Target.Path.back().Selector = EPropertyPathSelector::MapKey;
			Target.Path.back().MapKeyData = std::move(SerializedKey);
			Target.Path.back().MapKey = std::move(KeySnapshot);
		}
		Target.Path.push_back({EntryProperty});
		Target.Kind = EPropertyChangeKind::ValueSet;
		return Target;
	}

	auto FPropertyEditTarget::IsSameMutationTarget(const FPropertyEditTarget& Other) const -> bool
	{
		if (Object != Other.Object || MemberProperty != Other.MemberProperty
			|| LeafProperty != Other.LeafProperty || SnapshotProperty != Other.SnapshotProperty
			|| SnapshotContainer != Other.SnapshotContainer || SnapshotArrayIndex != Other.SnapshotArrayIndex
			|| LogicalIdentity != Other.LogicalIdentity || Path.size() != Other.Path.size()) return false;
		for (size_t Index = 0; Index < Path.size(); ++Index)
		{
			const FPropertyEditPathSegment& Left = Path[Index];
			const FPropertyEditPathSegment& Right = Other.Path[Index];
			if (Left.Property != Right.Property || Left.Selector != Right.Selector || Left.Index != Right.Index
				|| Left.MapKeyData != Right.MapKeyData || Left.MapKey != Right.MapKey) return false;
		}
		return true;
	}

	auto FPropertyEditTarget::IsSameStableTarget(const FPropertyEditTarget& Other) const -> bool
	{
		if (Object != Other.Object || MemberProperty != Other.MemberProperty
			|| LeafProperty != Other.LeafProperty || SnapshotProperty != Other.SnapshotProperty
			|| SnapshotArrayIndex != Other.SnapshotArrayIndex
			|| LogicalIdentity != Other.LogicalIdentity || Path.size() != Other.Path.size()) return false;
		for (size_t Index = 0; Index < Path.size(); ++Index)
		{
			const FPropertyEditPathSegment& Left = Path[Index];
			const FPropertyEditPathSegment& Right = Other.Path[Index];
			if (Left.Property != Right.Property || Left.Selector != Right.Selector || Left.Index != Right.Index
				|| Left.MapKeyData != Right.MapKeyData || Left.MapKey != Right.MapKey) return false;
		}
		return true;
	}

	auto FPropertyEditTarget::MatchesContinuousEdit(const FPropertyEditTarget& Other) const -> bool
	{
		if (Object != Other.Object || MemberProperty != Other.MemberProperty
			|| LeafProperty != Other.LeafProperty || SnapshotProperty != Other.SnapshotProperty
			|| SnapshotContainer != Other.SnapshotContainer || SnapshotArrayIndex != Other.SnapshotArrayIndex
			|| Kind != Other.Kind || LogicalIdentity != Other.LogicalIdentity
			|| Path.size() != Other.Path.size()) return false;
		for (size_t Index = 0; Index < Path.size(); ++Index)
		{
			const FPropertyEditPathSegment& Left = Path[Index];
			const FPropertyEditPathSegment& Right = Other.Path[Index];
			if (Left.Property != Right.Property || Left.Selector != Right.Selector || Left.Index != Right.Index) return false;
			const bool bContinuousKeyRename = Kind == EPropertyChangeKind::MapKeyRename
				&& Left.Selector == EPropertyPathSelector::MapKey;
			if (!bContinuousKeyRename && (Left.MapKeyData != Right.MapKeyData || Left.MapKey != Right.MapKey)) return false;
		}
		return true;
	}

	struct FPropertyTransaction::FDeferredRestoreOwnerState
	{
		std::mutex Mutex;
		FPropertyTransaction* Owner = nullptr;
	};

	FPropertyTransaction::FPropertyTransaction(
		FPropertyEditTarget InTarget,
		FPropertyValueSnapshot InBefore,
		FPropertyValueSnapshot InAfter,
		std::string InDescription
	)
		: Target(std::move(InTarget))
		, Before(std::move(InBefore))
		, After(std::move(InAfter))
		, Description(std::move(InDescription))
	{
		AffectedPackages.front() = Target.Object ? Target.Object->GetPackage() : nullptr;
		// Transaction history is not reflected, so it must keep both the edited
		// object and any object references inside its snapshots visible to GC.
		if (GDObjectArray.Contains(Target.Object))
		{
			AddToRoot(Target.Object);
			bObjectRooted = true;
		}
	}

	FPropertyTransaction::~FPropertyTransaction()
	{
		if (DeferredRestoreOwnerState)
		{
			std::lock_guard Lock(DeferredRestoreOwnerState->Mutex);
			DeferredRestoreOwnerState->Owner = nullptr;
		}
		if (CancelDeferredRestore) CancelDeferredRestore();
		if (bObjectRooted && GDObjectArray.Contains(Target.Object)) RemoveFromRoot(Target.Object);
	}

	auto FPropertyTransaction::GetDetails(ETransactionOperation) const -> std::string
	{
		if (!LastError.empty()) return LastError;
		if (!Target.Object || !Target.MemberProperty) return {};
		return std::format("{}.{}", Target.Object->GetObjectPath(), Target.MemberProperty->NamePrivate.ToString());
	}

	auto FPropertyTransaction::Undo() -> bool
	{
		return Restore(Before, EPropertyChangeOrigin::Undo);
	}

	auto FPropertyTransaction::Redo() -> bool
	{
		return Restore(After, EPropertyChangeOrigin::Redo);
	}

	auto FPropertyTransaction::Restore(const FPropertyValueSnapshot& Snapshot, EPropertyChangeOrigin Origin) -> bool
	{
		LastError.clear();
		if (bDeferredRestorePending)
		{
			LastError = "The reflected-property transaction already has a pending restore.";
			return false;
		}
		if (!Target.Object)
		{
			LastError = "The reflected-property transaction target is unavailable.";
			return false;
		}
		const FMutationExecutionResult Result = ExecuteMutation(
			Target, &Snapshot, nullptr,
			EMutationOperation::Apply,
			EPropertyChangePhase::Committed, Origin, &LastError);
		if (!Result.bSucceeded)
		{
			if (LastError.empty()) LastError = "The reflected-property transaction could not restore its value.";
			return false;
		}
		if (Result.bDeferred)
		{
			const FPropertyEditTarget DeferredTarget = Target;
			const FWeakObjectPtr WeakObject(Target.Object);
			FPropertyValueSnapshot ProposedValue = std::move(Result.Deferred.ProposedValue);
			bDeferredRestorePending = true;
			bStartingDeferredRestore = true;
			ImmediateDeferredRestoreResult.reset();
			DeferredRestoreOwnerState = std::make_shared<FDeferredRestoreOwnerState>();
			DeferredRestoreOwnerState->Owner = this;
			const std::shared_ptr<FDeferredRestoreOwnerState> OwnerState = DeferredRestoreOwnerState;
			CancelDeferredRestore = Result.Deferred.Action(
				[OwnerState, DeferredTarget, WeakObject, ProposedValue = std::move(ProposedValue), Origin](
					bool bSucceeded, std::string Error) mutable {
					FPropertyTransaction* Owner = nullptr;
					{
						std::lock_guard Lock(OwnerState->Mutex);
						Owner = OwnerState->Owner;
					}
					if (!Owner) return;
					if (WeakObject.Get() != DeferredTarget.Object)
					{
						bSucceeded = false;
						Error = "The reflected-property transaction target was destroyed while validation was pending.";
					}
					Owner->CompleteDeferredRestore(
						bSucceeded,
						std::move(Error),
						DeferredTarget,
						std::move(ProposedValue),
						Origin);
				});
			bStartingDeferredRestore = false;
			if (ImmediateDeferredRestoreResult.has_value())
			{
				const bool bSucceeded = *ImmediateDeferredRestoreResult;
				ImmediateDeferredRestoreResult.reset();
				CancelDeferredRestore = {};
				return bSucceeded;
			}
			return true;
		}
		Target.Object->MarkPackageDirty();
		return true;
	}

	auto FPropertyTransaction::CompleteDeferredRestore(
		bool bSucceeded,
		std::string Error,
		FPropertyEditTarget DeferredTarget,
		FPropertyValueSnapshot ProposedValue,
		EPropertyChangeOrigin Origin) -> void
	{
		if (!bDeferredRestorePending) return;
		if (bSucceeded)
		{
			FMutationExecutionResult Applied = ApplyDeferredMutation(
				DeferredTarget,
				ProposedValue,
				EPropertyChangePhase::Committed,
				Origin,
				&Error);
			bSucceeded = Applied.bSucceeded;
		}
		if (!bSucceeded)
			LastError = Error.empty()
				? "Deferred reflected-property transaction restore failed." : std::move(Error);
		bDeferredRestorePending = false;
		CancelDeferredRestore = {};
		if (DeferredRestoreOwnerState)
		{
			std::lock_guard Lock(DeferredRestoreOwnerState->Mutex);
			DeferredRestoreOwnerState->Owner = nullptr;
		}
		DeferredRestoreOwnerState.reset();
		if (bStartingDeferredRestore)
		{
			ImmediateDeferredRestoreResult = bSucceeded;
			return;
		}
		FTransactionDeferredCompletion Completion = DeferredRestoreCompletion;
		if (Completion) Completion(bSucceeded);
	}

	FPropertyEditSession::~FPropertyEditSession()
	{
		// An applied preview must never be abandoned merely because its UI owner is
		// destroyed. Explicit Commit/Cancel remains preferable because it can surface errors.
		if (bActive)
		{
			std::string Error;
			if (Cancel(&Error) == EPropertyEditResult::Failed)
			{
				DURIN_FATAL("Unable to restore an unfinished reflected-property preview: {}", Error);
				check(false);
			}
		}
		Reset();
	}

	auto FPropertyEditSession::Begin(
		const FPropertyEditTarget& InTarget,
		std::string_view InDescription,
		std::string* OutError,
		FTransactionManager* InTransactionManager
	) -> bool
	{
		if (bActive) return Fail(OutError, "A reflected-property edit session is already active.");
		Target = InTarget;
		if (!ValidateTarget(Target, OutError))
		{
			Reset();
			return false;
		}
		TransactionManager = InTransactionManager;
		Description = InDescription.empty()
			? std::format("Edit {}", Target.MemberProperty->NamePrivate.ToString())
			: InDescription;
		if (!CaptureTargetValue(Target, OriginalValue, OutError))
		{
			Reset();
			return false;
		}
		CurrentValue = OriginalValue;
		// Editor services are not reflected GC owners, so the session roots its target
		// explicitly while raw leaf-container addresses and callbacks depend on it.
		if (GDObjectArray.Contains(Target.Object))
		{
			AddToRoot(Target.Object);
			bObjectRooted = true;
		}
		bActive = true;
		return true;
	}

	auto FPropertyEditSession::Apply(const FPropertyValueSnapshot& ProposedValue, std::string* OutError) -> EPropertyEditResult
	{
		if (!bActive) { Fail(OutError, "No reflected-property edit session is active."); return EPropertyEditResult::Failed; }
		if (bDeferredPending)
		{
			if (DeferredOwnerState)
			{
				std::lock_guard Lock(DeferredOwnerState->Mutex);
				DeferredOwnerState->Owner = nullptr;
			}
			if (CancelDeferredEdit) CancelDeferredEdit();
			CancelDeferredEdit = {};
			DeferredOwnerState.reset();
			bDeferredPending = false;
		}
		if (ProposedValue == CurrentValue) return EPropertyEditResult::NoChange;
		FMutationExecutionResult Result = ExecuteMutation(
			Target, &ProposedValue, &CurrentValue, EMutationOperation::Apply,
			EPropertyChangePhase::Interactive, EPropertyChangeOrigin::Edit, OutError);
		if (!Result.bSucceeded)
		{
			if (Result.AppliedValue.IsValid()) CurrentValue = std::move(Result.AppliedValue);
			return EPropertyEditResult::Failed;
		}
		if (Result.bDeferred)
		{
			bDeferredPending = true;
			DeferredOwnerState = std::make_shared<FDeferredOwnerState>();
			DeferredOwnerState->Owner = this;
			const std::shared_ptr<FDeferredOwnerState> OwnerState = DeferredOwnerState;
			FPropertyValueSnapshot DeferredValue = std::move(Result.Deferred.ProposedValue);
			FPropertyEditDeferredCancel Cancel = Result.Deferred.Action(
				[OwnerState, DeferredValue = std::move(DeferredValue)](
					bool bSucceeded, std::string Error) mutable {
					FPropertyEditSession* Owner = nullptr;
					{
						std::lock_guard Lock(OwnerState->Mutex);
						Owner = OwnerState->Owner;
					}
					if (Owner)
						Owner->CompleteDeferredEdit(
							bSucceeded, std::move(Error), std::move(DeferredValue));
				});
			if (bDeferredPending && DeferredOwnerState == OwnerState)
				CancelDeferredEdit = std::move(Cancel);
			return EPropertyEditResult::Pending;
		}
		CurrentValue = std::move(Result.AppliedValue);
		return Result.bChanged ? EPropertyEditResult::Changed : EPropertyEditResult::NoChange;
	}

	auto FPropertyEditSession::CompleteDeferredEdit(
		bool bSucceeded,
		std::string Error,
		FPropertyValueSnapshot ProposedValue) -> void
	{
		if (!bActive || !bDeferredPending) return;
		if (DeferredOwnerState)
		{
			std::lock_guard Lock(DeferredOwnerState->Mutex);
			DeferredOwnerState->Owner = nullptr;
		}
		CancelDeferredEdit = {};
		DeferredOwnerState.reset();
		bDeferredPending = false;
		if (!bSucceeded)
		{
			if (!Error.empty()) DURIN_ERROR("Deferred reflected-property edit failed: {}", Error);
			Reset();
			return;
		}
		FMutationExecutionResult Result = ApplyDeferredMutation(
			Target,
			ProposedValue,
			EPropertyChangePhase::Interactive,
			EPropertyChangeOrigin::Edit,
			&Error);
		if (!Result.bSucceeded)
		{
			DURIN_ERROR("Deferred reflected-property publication failed: {}", Error);
			Reset();
			return;
		}
		CurrentValue = std::move(Result.AppliedValue);
		if (Commit(&Error) == EPropertyEditResult::Failed)
			DURIN_ERROR("Deferred reflected-property transaction failed: {}", Error);
	}

	auto FPropertyEditSession::MatchesTarget(const FPropertyEditTarget& Other) const -> bool
	{
		return bActive && Target.MatchesContinuousEdit(Other);
	}

	auto FPropertyEditSession::Commit(std::string* OutError) -> EPropertyEditResult
	{
		if (!bActive) { Fail(OutError, "No reflected-property edit session is active."); return EPropertyEditResult::Failed; }
		const bool bChanged = HasChanges();
		if (!ExecuteMutation(Target, nullptr, nullptr, EMutationOperation::NotifyOnly,
			EPropertyChangePhase::Committed, EPropertyChangeOrigin::Edit, OutError).bSucceeded)
			return EPropertyEditResult::Failed;
		if (bChanged)
		{
			Target.Object->MarkPackageDirty();
			if (TransactionManager)
			{
				// Preview already placed the object in its final state. Register exactly
				// one applied transaction here instead of replaying the value on commit.
				TransactionManager->CommitApplied(std::make_unique<FPropertyTransaction>(
					Target, OriginalValue, CurrentValue, Description
				));
			}
		}
		Reset();
		return bChanged ? EPropertyEditResult::Changed : EPropertyEditResult::NoChange;
	}

	auto FPropertyEditSession::Cancel(std::string* OutError) -> EPropertyEditResult
	{
		if (!bActive) { Fail(OutError, "No reflected-property edit session is active."); return EPropertyEditResult::Failed; }
		const bool bChanged = HasChanges();
		FMutationExecutionResult Result = ExecuteMutation(
			Target, bChanged ? &OriginalValue : nullptr, nullptr,
			bChanged ? EMutationOperation::Apply : EMutationOperation::NotifyOnly,
			EPropertyChangePhase::Cancelled, EPropertyChangeOrigin::Edit, OutError);
		if (!Result.bSucceeded)
		{
			if (Result.AppliedValue.IsValid()) CurrentValue = std::move(Result.AppliedValue);
			return EPropertyEditResult::Failed;
		}
		Reset();
		return bChanged ? EPropertyEditResult::Changed : EPropertyEditResult::NoChange;
	}

	auto FPropertyEditSession::Reset() -> void
	{
		if (DeferredOwnerState)
		{
			std::lock_guard Lock(DeferredOwnerState->Mutex);
			DeferredOwnerState->Owner = nullptr;
		}
		if (CancelDeferredEdit) CancelDeferredEdit();
		CancelDeferredEdit = {};
		DeferredOwnerState.reset();
		bDeferredPending = false;
		if (bObjectRooted && GDObjectArray.Contains(Target.Object)) RemoveFromRoot(Target.Object);
		bObjectRooted = false;
		bActive = false;
		Target = {};
		OriginalValue = {};
		CurrentValue = {};
		Description.clear();
		TransactionManager = nullptr;
	}
}
