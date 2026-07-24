#include "Editor/ReflectedPropertyEditing.h"
#include "Editor/PropertyValueDraft.h"

#include "DObject/DObjectArray.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/Object.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Property.h"
#include "Logging/LogMacros.h"
#include "Misc/AssertionMacros.h"

namespace Durin
{
	namespace
	{
		auto Fail(std::string* OutError, std::string_view Message) -> bool
		{
			if (OutError) *OutError = Message;
			return false;
		}

		auto CaptureTargetValue(const FReflectedPropertyEditTarget& Target,
			FPropertyValueSnapshot& OutSnapshot, std::string* OutError) -> bool
		{
			return CapturePropertyValue(
				Target.SnapshotProperty, Target.SnapshotContainer, Target.SnapshotArrayIndex, OutSnapshot, OutError);
		}

		auto RestoreTargetValue(const FReflectedPropertyEditTarget& Target,
			const FPropertyValueSnapshot& Snapshot, std::string* OutError) -> bool
		{
			return RestorePropertyValue(
				Target.SnapshotProperty, Target.SnapshotContainer, Target.SnapshotArrayIndex, Snapshot, OutError);
		}

		auto MakeEventPath(const FReflectedPropertyEditTarget& Target) -> std::vector<FPropertyPathSegment>
		{
			std::vector<FPropertyPathSegment> Result;
			Result.reserve(Target.Path.size());
			for (const FReflectedPropertyEditPathSegment& Segment : Target.Path)
				Result.push_back({Segment.Property, Segment.Selector, Segment.Index, Segment.MapKeyData});
			return Result;
		}

		auto IsSameMutationTarget(const FReflectedPropertyEditTarget& Left, const FReflectedPropertyEditTarget& Right) -> bool
		{
			if (Left.Object != Right.Object || Left.MemberProperty != Right.MemberProperty
				|| Left.LeafProperty != Right.LeafProperty || Left.SnapshotProperty != Right.SnapshotProperty
				|| Left.SnapshotContainer != Right.SnapshotContainer || Left.SnapshotArrayIndex != Right.SnapshotArrayIndex
				|| Left.LogicalIdentity != Right.LogicalIdentity || Left.Path.size() != Right.Path.size()) return false;
			for (size_t Index = 0; Index < Left.Path.size(); ++Index)
			{
				const auto& A = Left.Path[Index];
				const auto& B = Right.Path[Index];
				if (A.Property != B.Property || A.Selector != B.Selector || A.Index != B.Index
					|| A.MapKeyData != B.MapKeyData || A.MapKey != B.MapKey) return false;
			}
			return true;
		}

		thread_local std::vector<const FReflectedPropertyEditTarget*> GActiveGenericMutations;

		// Balances pre/post mutation callbacks even when a container edit fails.
		class FGenericMutationScope
		{
		public:
			explicit FGenericMutationScope(const FReflectedPropertyEditTarget& Target)
				: Target(&Target) { GActiveGenericMutations.push_back(this->Target); }
			~FGenericMutationScope() { GActiveGenericMutations.pop_back(); }
		private:
			const FReflectedPropertyEditTarget* Target;
		};

		auto ApplyGenericMutation(
			const FReflectedPropertyEditTarget& Target,
			const FPropertyValueSnapshot& ProposedValue,
			EPropertyChangePhase Phase,
			EPropertyChangeOrigin Origin,
			FPropertyValueSnapshot* OutAppliedValue,
			std::string* OutError
		) -> bool
		{
			if (std::ranges::any_of(GActiveGenericMutations, [&](const auto* Active) { return IsSameMutationTarget(*Active, Target); }))
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
			if (!Target.Object->PreEditChangeProperty(Proposal, HookError))
			{
				if (OutError) *OutError = HookError.empty() ? "The object rejected the reflected property proposal." : HookError;
				return false;
			}

			FPropertyValueSnapshot Normalized;
			if (!Draft.Capture(Normalized, OutError)) return false;
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

		auto ValidateTarget(const FReflectedPropertyEditTarget& Target, std::string* OutError) -> bool
		{
			if (!Target.Object) return Fail(OutError, "The edit target has no owning object.");
			if (!Target.MemberProperty || !Target.LeafProperty
				|| !Target.SnapshotProperty || !Target.SnapshotContainer) return Fail(OutError, "The edit target is incomplete.");
			if (Target.SnapshotArrayIndex >= Target.SnapshotProperty->GetArrayDim()) return Fail(OutError, "The snapshot property array index is out of range.");
			if (Target.Path.empty() || Target.Path.front().Property != Target.MemberProperty || Target.Path.back().Property != Target.LeafProperty)
			{
				return Fail(OutError, "The property path must run from the member property to the leaf property.");
			}
			for (const FReflectedPropertyEditPathSegment& Segment : Target.Path)
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
			bool bSucceeded = false;
			bool bChanged = false;
		};

		auto NotifyMutation(
			const FReflectedPropertyEditTarget& Target,
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
		}

		auto ExecuteMutation(
			const FReflectedPropertyEditTarget& Target,
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

			if (!ApplyGenericMutation(Target, *Value, Phase, Origin, &Result.AppliedValue, OutError)) return Result;

			Result.bSucceeded = true;
			Result.bChanged = !PreviousValue || !(Result.AppliedValue == *PreviousValue);
			if (Phase != EPropertyChangePhase::Interactive || Result.bChanged) NotifyMutation(Target, Phase, Origin);
			return Result;
		}
	}

	auto ResolveReflectedPropertyValue(
		const FReflectedPropertyEditTarget& Target,
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
			const FReflectedPropertyEditPathSegment& Segment = Target.Path[PathIndex];
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
				if (!ArrayProperty || Segment.Index >= ArrayProperty->Num(Container, CurrentArrayIndex))
					return Fail(OutError, "The reflected array path index is unavailable.");
				Container = ArrayProperty->GetMutableElementPtr(Container, Segment.Index, CurrentArrayIndex);
				CurrentArrayIndex = 0;
				break;
			}
			case EPropertyPathSelector::MapKey:
			{
				auto* MapProperty = CurrentProperty->GetKind() == DurinCodeGen::EPropertyGenFlags::Map
					? static_cast<FMapProperty*>(CurrentProperty) : nullptr;
				if (!MapProperty || !Segment.MapKey.IsValid())
					return Fail(OutError, "The reflected map path lacks a stable key snapshot.");
				uint64 MapIndex = UINT64_MAX;
				for (uint64 Index = 0; Index < MapProperty->Num(Container, CurrentArrayIndex); ++Index)
				{
					FPropertyValueSnapshot StoredKey;
					const void* Key = MapProperty->GetKeyPtr(Container, Index, CurrentArrayIndex);
					if (Key && CapturePropertyValue(MapProperty->GetKeyProp(), Key, 0, StoredKey)
						&& StoredKey == Segment.MapKey)
					{
						MapIndex = Index;
						break;
					}
				}
				if (MapIndex == UINT64_MAX) return Fail(OutError, "The reflected map key is unavailable.");
				if (NextProperty == MapProperty->GetKeyProp())
					Container = const_cast<void*>(MapProperty->GetKeyPtr(Container, MapIndex, CurrentArrayIndex));
				else if (NextProperty == MapProperty->GetValueProp())
					Container = MapProperty->GetMutableMappedValuePtr(Container, MapIndex, CurrentArrayIndex);
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

	auto FReflectedPropertyEditTarget::ForMember(DObject* Object, const FProperty* Property, uint32 ArrayIndex) -> FReflectedPropertyEditTarget
	{
		FReflectedPropertyEditTarget Target;
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

	auto FReflectedPropertyEditTarget::ForStructMember(const FProperty* Property, uint32 ArrayIndex) const -> FReflectedPropertyEditTarget
	{
		FReflectedPropertyEditTarget Target = *this;
		Target.LeafProperty = Property;
		Target.Path.push_back({
			Property,
			Property && Property->GetArrayDim() > 1 ? EPropertyPathSelector::StaticArrayIndex : EPropertyPathSelector::None,
			ArrayIndex
		});
		Target.Kind = EPropertyChangeKind::ValueSet;
		return Target;
	}

	auto FReflectedPropertyEditTarget::ForArrayElement(const FProperty* ElementProperty, uint64 ElementIndex) const -> FReflectedPropertyEditTarget
	{
		FReflectedPropertyEditTarget Target = *this;
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

	auto FReflectedPropertyEditTarget::ForMapEntry(const FProperty* EntryProperty, std::vector<uint8> SerializedKey) const -> FReflectedPropertyEditTarget
	{
		return ForMapEntry(EntryProperty, {}, std::move(SerializedKey));
	}

	auto FReflectedPropertyEditTarget::ForMapEntry(const FProperty* EntryProperty,
		FPropertyValueSnapshot KeySnapshot, std::vector<uint8> SerializedKey) const -> FReflectedPropertyEditTarget
	{
		FReflectedPropertyEditTarget Target = *this;
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

	FReflectedPropertyTransaction::FReflectedPropertyTransaction(
		FReflectedPropertyEditTarget InTarget,
		FPropertyValueSnapshot InBefore,
		FPropertyValueSnapshot InAfter,
		std::string InDescription
	)
		: Target(std::move(InTarget))
		, Before(std::move(InBefore))
		, After(std::move(InAfter))
		, Description(std::move(InDescription))
	{
		// Transaction history is not reflected, so it must keep both the edited
		// object and any object references inside its snapshots visible to GC.
		if (GDObjectArray.Contains(Target.Object))
		{
			AddToRoot(Target.Object);
			bObjectRooted = true;
		}
	}

	FReflectedPropertyTransaction::~FReflectedPropertyTransaction()
	{
		if (bObjectRooted && GDObjectArray.Contains(Target.Object)) RemoveFromRoot(Target.Object);
	}

	auto FReflectedPropertyTransaction::GetDetails(EEditorTransactionOperation) const -> std::string
	{
		if (!LastError.empty()) return LastError;
		if (!Target.Object || !Target.MemberProperty) return {};
		return std::format("{}.{}", Target.Object->GetObjectPath(), Target.MemberProperty->NamePrivate.ToString());
	}

	auto FReflectedPropertyTransaction::Undo() -> bool
	{
		return Restore(Before, EPropertyChangeOrigin::Undo);
	}

	auto FReflectedPropertyTransaction::Redo() -> bool
	{
		return Restore(After, EPropertyChangeOrigin::Redo);
	}

	auto FReflectedPropertyTransaction::Restore(const FPropertyValueSnapshot& Snapshot, EPropertyChangeOrigin Origin) -> bool
	{
		LastError.clear();
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
		Target.Object->MarkPackageDirty();
		return true;
	}

	FReflectedPropertyEditSession::~FReflectedPropertyEditSession()
	{
		// An applied preview must never be abandoned merely because its UI owner is
		// destroyed. Explicit Commit/Cancel remains preferable because it can surface errors.
		if (bActive)
		{
			std::string Error;
			if (Cancel(&Error) == EReflectedPropertyEditResult::Failed)
			{
				DURIN_FATAL("Unable to restore an unfinished reflected-property preview: {}", Error);
				check(false);
			}
		}
		Reset();
	}

	auto FReflectedPropertyEditSession::Begin(
		const FReflectedPropertyEditTarget& InTarget,
		std::string_view InDescription,
		std::string* OutError,
		FEditorTransactionManager* InTransactionManager
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

	auto FReflectedPropertyEditSession::Apply(const FPropertyValueSnapshot& ProposedValue, std::string* OutError) -> EReflectedPropertyEditResult
	{
		if (!bActive) { Fail(OutError, "No reflected-property edit session is active."); return EReflectedPropertyEditResult::Failed; }
		if (ProposedValue == CurrentValue) return EReflectedPropertyEditResult::NoChange;
		FMutationExecutionResult Result = ExecuteMutation(
			Target, &ProposedValue, &CurrentValue, EMutationOperation::Apply,
			EPropertyChangePhase::Interactive, EPropertyChangeOrigin::Edit, OutError);
		if (!Result.bSucceeded)
		{
			if (Result.AppliedValue.IsValid()) CurrentValue = std::move(Result.AppliedValue);
			return EReflectedPropertyEditResult::Failed;
		}
		CurrentValue = std::move(Result.AppliedValue);
		return Result.bChanged ? EReflectedPropertyEditResult::Changed : EReflectedPropertyEditResult::NoChange;
	}

	auto FReflectedPropertyEditSession::MatchesTarget(const FReflectedPropertyEditTarget& Other) const -> bool
	{
		if (!bActive || Target.Object != Other.Object || Target.MemberProperty != Other.MemberProperty
			|| Target.LeafProperty != Other.LeafProperty || Target.SnapshotProperty != Other.SnapshotProperty
			|| Target.SnapshotContainer != Other.SnapshotContainer || Target.SnapshotArrayIndex != Other.SnapshotArrayIndex
			|| Target.Kind != Other.Kind || Target.LogicalIdentity != Other.LogicalIdentity
			|| Target.Path.size() != Other.Path.size()) return false;
		for (size_t Index = 0; Index < Target.Path.size(); ++Index)
		{
			const FReflectedPropertyEditPathSegment& Left = Target.Path[Index];
			const FReflectedPropertyEditPathSegment& Right = Other.Path[Index];
			if (Left.Property != Right.Property || Left.Selector != Right.Selector || Left.Index != Right.Index) return false;
			// A key's bytes necessarily change during a rename. The active ImGui item
			// is the sole editor of this leaf, so member/leaf/path shape is the stable
			// identity while the transaction retains the original key in Target.Path.
			const bool bContinuousKeyRename = Target.Kind == EPropertyChangeKind::MapKeyRename
				&& Other.Kind == EPropertyChangeKind::MapKeyRename && Left.Selector == EPropertyPathSelector::MapKey;
			if (!bContinuousKeyRename && (Left.MapKeyData != Right.MapKeyData || Left.MapKey != Right.MapKey)) return false;
		}
		return true;
	}

	auto FReflectedPropertyEditSession::Commit(std::string* OutError) -> EReflectedPropertyEditResult
	{
		if (!bActive) { Fail(OutError, "No reflected-property edit session is active."); return EReflectedPropertyEditResult::Failed; }
		const bool bChanged = HasChanges();
		if (!ExecuteMutation(Target, nullptr, nullptr, EMutationOperation::NotifyOnly,
			EPropertyChangePhase::Committed, EPropertyChangeOrigin::Edit, OutError).bSucceeded)
			return EReflectedPropertyEditResult::Failed;
		if (bChanged)
		{
			Target.Object->MarkPackageDirty();
			if (TransactionManager)
			{
				// Preview already placed the object in its final state. Register exactly
				// one applied transaction here instead of replaying the value on commit.
				TransactionManager->CommitApplied(std::make_unique<FReflectedPropertyTransaction>(
					Target, OriginalValue, CurrentValue, Description
				));
			}
		}
		Reset();
		return bChanged ? EReflectedPropertyEditResult::Changed : EReflectedPropertyEditResult::NoChange;
	}

	auto FReflectedPropertyEditSession::Cancel(std::string* OutError) -> EReflectedPropertyEditResult
	{
		if (!bActive) { Fail(OutError, "No reflected-property edit session is active."); return EReflectedPropertyEditResult::Failed; }
		const bool bChanged = HasChanges();
		FMutationExecutionResult Result = ExecuteMutation(
			Target, bChanged ? &OriginalValue : nullptr, nullptr,
			bChanged ? EMutationOperation::Apply : EMutationOperation::NotifyOnly,
			EPropertyChangePhase::Cancelled, EPropertyChangeOrigin::Edit, OutError);
		if (!Result.bSucceeded)
		{
			if (Result.AppliedValue.IsValid()) CurrentValue = std::move(Result.AppliedValue);
			return EReflectedPropertyEditResult::Failed;
		}
		Reset();
		return bChanged ? EReflectedPropertyEditResult::Changed : EReflectedPropertyEditResult::NoChange;
	}

	auto FReflectedPropertyEditSession::Reset() -> void
	{
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
