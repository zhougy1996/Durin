#include "Editor/ReflectedPropertyEditing.h"

#include "DObject/DObjectArray.h"
#include "DObject/Object.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Property.h"

namespace Durin
{
	namespace
	{
		auto Fail(std::string* OutError, std::string_view Message) -> bool
		{
			if (OutError) *OutError = Message;
			return false;
		}

		class FGenericReflectedPropertyMutationAdapter final : public IReflectedPropertyMutationAdapter
		{
		public:
			auto Capture(const FReflectedPropertyEditTarget& Target, FPropertyValueSnapshot& OutSnapshot, std::string* OutError) const -> bool override
			{
				return CapturePropertyValue(Target.SnapshotProperty, Target.SnapshotContainer, Target.SnapshotArrayIndex, OutSnapshot, OutError);
			}

			auto Apply(const FReflectedPropertyEditTarget& Target, const FPropertyValueSnapshot& ProposedValue, std::string* OutError) const -> bool override
			{
				return RestorePropertyValue(Target.SnapshotProperty, Target.SnapshotContainer, Target.SnapshotArrayIndex, ProposedValue, OutError);
			}

			auto Restore(const FReflectedPropertyEditTarget& Target, const FPropertyValueSnapshot& Snapshot, std::string* OutError) const -> bool override
			{
				return RestorePropertyValue(Target.SnapshotProperty, Target.SnapshotContainer, Target.SnapshotArrayIndex, Snapshot, OutError);
			}
		};

		const FGenericReflectedPropertyMutationAdapter GGenericMutationAdapter;

		auto ValidateTarget(const FReflectedPropertyEditTarget& Target, std::string* OutError) -> bool
		{
			if (!Target.Object) return Fail(OutError, "The edit target has no owning object.");
			if (!Target.MemberProperty || !Target.LeafProperty || !Target.LeafContainer
				|| !Target.SnapshotProperty || !Target.SnapshotContainer) return Fail(OutError, "The edit target is incomplete.");
			if (Target.LeafArrayIndex >= Target.LeafProperty->GetArrayDim()) return Fail(OutError, "The leaf property array index is out of range.");
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
	}

	auto FReflectedPropertyEditTarget::ForMember(DObject* Object, const FProperty* Property, uint32 ArrayIndex) -> FReflectedPropertyEditTarget
	{
		FReflectedPropertyEditTarget Target;
		Target.Object = Object;
		Target.MemberProperty = Property;
		Target.LeafProperty = Property;
		Target.LeafContainer = Object;
		Target.LeafArrayIndex = ArrayIndex;
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

	auto FReflectedPropertyEditTarget::ForArrayElement(const FProperty* ElementProperty, void* ElementContainer, uint64 ElementIndex) const -> FReflectedPropertyEditTarget
	{
		FReflectedPropertyEditTarget Target = *this;
		Target.LeafProperty = ElementProperty;
		Target.LeafContainer = ElementContainer;
		Target.LeafArrayIndex = 0;
		if (!Target.Path.empty())
		{
			Target.Path.back().Selector = EPropertyPathSelector::ArrayIndex;
			Target.Path.back().Index = ElementIndex;
		}
		Target.Path.push_back({ElementProperty});
		Target.Kind = EPropertyChangeKind::ValueSet;
		return Target;
	}

	auto FReflectedPropertyEditTarget::ForMapEntry(const FProperty* EntryProperty, void* EntryContainer, std::vector<uint8> SerializedKey) const -> FReflectedPropertyEditTarget
	{
		FReflectedPropertyEditTarget Target = *this;
		Target.LeafProperty = EntryProperty;
		Target.LeafContainer = EntryContainer;
		Target.LeafArrayIndex = 0;
		if (!Target.Path.empty())
		{
			Target.Path.back().Selector = EPropertyPathSelector::MapKey;
			Target.Path.back().MapKeyData = std::move(SerializedKey);
		}
		Target.Path.push_back({EntryProperty});
		Target.Kind = EPropertyChangeKind::ValueSet;
		return Target;
	}

	auto GetGenericReflectedPropertyMutationAdapter() -> const IReflectedPropertyMutationAdapter&
	{
		return GGenericMutationAdapter;
	}

	FReflectedPropertyTransaction::FReflectedPropertyTransaction(
		FReflectedPropertyEditTarget InTarget,
		FPropertyValueSnapshot InBefore,
		FPropertyValueSnapshot InAfter,
		std::string InDescription,
		const IReflectedPropertyMutationAdapter* InAdapter
	)
		: Target(std::move(InTarget))
		, Before(std::move(InBefore))
		, After(std::move(InAfter))
		, Description(std::move(InDescription))
		, Adapter(InAdapter ? InAdapter : &GetGenericReflectedPropertyMutationAdapter())
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
		if (!Target.Object || !Adapter)
		{
			LastError = "The reflected-property transaction target is unavailable.";
			return false;
		}
		const bool bApplied = Origin == EPropertyChangeOrigin::Undo
			? Adapter->Restore(Target, Snapshot, &LastError)
			: Adapter->Apply(Target, Snapshot, &LastError);
		if (!bApplied)
		{
			if (LastError.empty()) LastError = "The reflected-property transaction could not restore its value.";
			return false;
		}
		Notify(Origin);
		Target.Object->MarkPackageDirty();
		return true;
	}

	auto FReflectedPropertyTransaction::Notify(EPropertyChangeOrigin Origin) const -> void
	{
		std::vector<FPropertyPathSegment> EventPath;
		EventPath.reserve(Target.Path.size());
		for (const FReflectedPropertyEditPathSegment& Segment : Target.Path)
		{
			EventPath.push_back({Segment.Property, Segment.Selector, Segment.Index, Segment.MapKeyData});
		}
		Target.Object->PostEditChangeProperty({
			Target.MemberProperty,
			Target.LeafProperty,
			EventPath,
			EPropertyChangePhase::Committed,
			Target.Kind,
			Origin
		});
	}

	FReflectedPropertyEditSession::~FReflectedPropertyEditSession()
	{
		// An applied preview must never be abandoned merely because its UI owner is
		// destroyed. Explicit Commit/Cancel remains preferable because it can surface errors.
		if (bActive) Cancel();
		Reset();
	}

	auto FReflectedPropertyEditSession::Begin(
		const FReflectedPropertyEditTarget& InTarget,
		std::string_view InDescription,
		const IReflectedPropertyMutationAdapter* InAdapter,
		std::string* OutError,
		FEditorTransactionManager* InTransactionManager
	) -> bool
	{
		if (bActive) return Fail(OutError, "A reflected-property edit session is already active.");
		Target = InTarget;
		// Older/custom callers that describe only a leaf still get the original
		// behavior; container-aware callers explicitly select a stable snapshot root.
		if (!Target.SnapshotProperty) Target.SnapshotProperty = Target.LeafProperty;
		if (!Target.SnapshotContainer) Target.SnapshotContainer = Target.LeafContainer;
		if (!ValidateTarget(Target, OutError))
		{
			Reset();
			return false;
		}
		Adapter = InAdapter ? InAdapter : &GetGenericReflectedPropertyMutationAdapter();
		TransactionManager = InTransactionManager;
		Description = InDescription;
		if (!Adapter->Capture(Target, OriginalValue, OutError))
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
		if (!Adapter->Apply(Target, ProposedValue, OutError)) return EReflectedPropertyEditResult::Failed;

		FPropertyValueSnapshot AppliedValue;
		if (!Adapter->Capture(Target, AppliedValue, OutError)) return EReflectedPropertyEditResult::Failed;
		if (AppliedValue == CurrentValue) return EReflectedPropertyEditResult::NoChange;
		CurrentValue = std::move(AppliedValue);
		Notify(EPropertyChangePhase::Interactive);
		return EReflectedPropertyEditResult::Changed;
	}

	auto FReflectedPropertyEditSession::MatchesTarget(const FReflectedPropertyEditTarget& Other) const -> bool
	{
		if (!bActive || Target.Object != Other.Object || Target.MemberProperty != Other.MemberProperty
			|| Target.LeafProperty != Other.LeafProperty || Target.SnapshotProperty != Other.SnapshotProperty
			|| Target.SnapshotContainer != Other.SnapshotContainer || Target.SnapshotArrayIndex != Other.SnapshotArrayIndex
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
			if (!bContinuousKeyRename && Left.MapKeyData != Right.MapKeyData) return false;
		}
		return true;
	}

	auto FReflectedPropertyEditSession::Commit(std::string* OutError) -> EReflectedPropertyEditResult
	{
		if (!bActive) { Fail(OutError, "No reflected-property edit session is active."); return EReflectedPropertyEditResult::Failed; }
		const bool bChanged = HasChanges();
		if (bChanged)
		{
			Notify(EPropertyChangePhase::Committed);
			Target.Object->MarkPackageDirty();
			if (TransactionManager)
			{
				// Preview already placed the object in its final state. Register exactly
				// one applied transaction here instead of replaying the value on commit.
				TransactionManager->CommitApplied(std::make_unique<FReflectedPropertyTransaction>(
					Target, OriginalValue, CurrentValue, Description, Adapter
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
		if (bChanged && !Adapter->Restore(Target, OriginalValue, OutError)) return EReflectedPropertyEditResult::Failed;
		if (bChanged) Notify(EPropertyChangePhase::Cancelled);
		Reset();
		return bChanged ? EReflectedPropertyEditResult::Changed : EReflectedPropertyEditResult::NoChange;
	}

	auto FReflectedPropertyEditSession::Notify(EPropertyChangePhase Phase) const -> void
	{
		std::vector<FPropertyPathSegment> EventPath;
		EventPath.reserve(Target.Path.size());
		for (const FReflectedPropertyEditPathSegment& Segment : Target.Path)
		{
			EventPath.push_back({Segment.Property, Segment.Selector, Segment.Index, Segment.MapKeyData});
		}
		Target.Object->PostEditChangeProperty({
			Target.MemberProperty,
			Target.LeafProperty,
			EventPath,
			Phase,
			Target.Kind,
			EPropertyChangeOrigin::Edit
		});
	}

	auto FReflectedPropertyEditSession::Reset() -> void
	{
		if (bObjectRooted && GDObjectArray.Contains(Target.Object)) RemoveFromRoot(Target.Object);
		bObjectRooted = false;
		bActive = false;
		Target = {};
		Adapter = nullptr;
		OriginalValue = {};
		CurrentValue = {};
		Description.clear();
		TransactionManager = nullptr;
	}
}
