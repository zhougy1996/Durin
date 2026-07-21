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
				return CapturePropertyValue(Target.LeafProperty, Target.LeafContainer, Target.LeafArrayIndex, OutSnapshot, OutError);
			}

			auto Apply(const FReflectedPropertyEditTarget& Target, const FPropertyValueSnapshot& ProposedValue, std::string* OutError) const -> bool override
			{
				return RestorePropertyValue(Target.LeafProperty, Target.LeafContainer, Target.LeafArrayIndex, ProposedValue, OutError);
			}

			auto Restore(const FReflectedPropertyEditTarget& Target, const FPropertyValueSnapshot& Snapshot, std::string* OutError) const -> bool override
			{
				return RestorePropertyValue(Target.LeafProperty, Target.LeafContainer, Target.LeafArrayIndex, Snapshot, OutError);
			}
		};

		const FGenericReflectedPropertyMutationAdapter GGenericMutationAdapter;

		auto ValidateTarget(const FReflectedPropertyEditTarget& Target, std::string* OutError) -> bool
		{
			if (!Target.Object) return Fail(OutError, "The edit target has no owning object.");
			if (!Target.MemberProperty || !Target.LeafProperty || !Target.LeafContainer) return Fail(OutError, "The edit target is incomplete.");
			if (Target.LeafArrayIndex >= Target.LeafProperty->GetArrayDim()) return Fail(OutError, "The leaf property array index is out of range.");
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
		Target.Path.push_back({
			Property,
			Property && Property->GetArrayDim() > 1 ? EPropertyPathSelector::StaticArrayIndex : EPropertyPathSelector::None,
			ArrayIndex
		});
		return Target;
	}

	auto GetGenericReflectedPropertyMutationAdapter() -> const IReflectedPropertyMutationAdapter&
	{
		return GGenericMutationAdapter;
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
		std::string* OutError
	) -> bool
	{
		if (bActive) return Fail(OutError, "A reflected-property edit session is already active.");
		if (!ValidateTarget(InTarget, OutError)) return false;

		Target = InTarget;
		Adapter = InAdapter ? InAdapter : &GetGenericReflectedPropertyMutationAdapter();
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

	auto FReflectedPropertyEditSession::Commit(std::string* OutError) -> EReflectedPropertyEditResult
	{
		if (!bActive) { Fail(OutError, "No reflected-property edit session is active."); return EReflectedPropertyEditResult::Failed; }
		const bool bChanged = HasChanges();
		if (bChanged)
		{
			Notify(EPropertyChangePhase::Committed);
			Target.Object->MarkPackageDirty();
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
	}
}
