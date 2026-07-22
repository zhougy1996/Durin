#include "Editor/ReflectedPropertyEditing.h"
#include "Editor/PropertyValueDraft.h"

#include "DObject/DObjectArray.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
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
				|| Left.Path.size() != Right.Path.size()) return false;
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
			if (!GGenericMutationAdapter.Capture(Target, Before, OutError)) return false;
			FPropertyValueDraft Draft(Target, OutError);
			if (!Draft.IsValid() || !Draft.Restore(ProposedValue, OutError)) return false;

			FReflectedPropertyEditTarget DraftTarget;
			const bool bResolvedLeaf = Draft.Resolve(Target, DraftTarget, nullptr);
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
				bResolvedLeaf ? DraftTarget.LeafContainer : nullptr,
				bResolvedLeaf ? DraftTarget.LeafArrayIndex : 0
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
			if (!GGenericMutationAdapter.Apply(Target, Normalized, &ApplyError))
			{
				std::string RollbackError;
				if (!GGenericMutationAdapter.Restore(Target, Before, &RollbackError))
					ApplyError += std::format(" Rollback also failed: {}", RollbackError);
				if (OutError) *OutError = ApplyError;
				return false;
			}

			FPropertyValueSnapshot Applied;
			std::string CaptureError;
			if (!GGenericMutationAdapter.Capture(Target, Applied, &CaptureError))
			{
				std::string RollbackError;
				if (!GGenericMutationAdapter.Restore(Target, Before, &RollbackError))
					CaptureError += std::format(" Rollback also failed: {}", RollbackError);
				if (OutError) *OutError = CaptureError;
				return false;
			}
			if (OutAppliedValue) *OutAppliedValue = std::move(Applied);
			return true;
		}

		struct FMutationAdapterRegistration
		{
			const DClass* ObjectClass = nullptr;
			FName PropertyName;
			std::unique_ptr<IReflectedPropertyMutationAdapter> Adapter;
		};

		auto GetMutationAdapterRegistrations() -> std::vector<FMutationAdapterRegistration>&
		{
			static std::vector<FMutationAdapterRegistration> Registrations;
			return Registrations;
		}

		auto RegisterMutationAdapter(
			const DClass* ObjectClass,
			FName PropertyName,
			std::unique_ptr<IReflectedPropertyMutationAdapter> Adapter
		) -> bool
		{
			if (!ObjectClass || PropertyName.IsNone() || !Adapter) return false;
			auto& Registrations = GetMutationAdapterRegistrations();
			if (std::ranges::any_of(Registrations, [&](const FMutationAdapterRegistration& Entry) {
				return Entry.ObjectClass == ObjectClass && Entry.PropertyName == PropertyName;
			})) return false;
			Registrations.push_back({ObjectClass, PropertyName, std::move(Adapter)});
			return true;
		}

		auto ValidateTarget(const FReflectedPropertyEditTarget& Target, std::string* OutError) -> bool
		{
			if (!Target.Object) return Fail(OutError, "The edit target has no owning object.");
			if (!Target.MemberProperty || !Target.LeafProperty
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

		auto ResolveMutationTarget(
			const FReflectedPropertyEditTarget& Target,
			const IReflectedPropertyMutationAdapter* Adapter,
			FReflectedPropertyEditTarget& OutResolvedTarget,
			std::string* OutError
		) -> bool
		{
			if (Adapter == &GGenericMutationAdapter)
			{
				OutResolvedTarget = Target;
				return true;
			}
			return ResolveReflectedPropertyEditTarget(Target, OutResolvedTarget, OutError);
		}
	}

	auto ResolveReflectedPropertyEditTarget(
		const FReflectedPropertyEditTarget& Target,
		FReflectedPropertyEditTarget& OutResolvedTarget,
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
				OutResolvedTarget = Target;
				OutResolvedTarget.LeafContainer = Container;
				OutResolvedTarget.LeafArrayIndex = CurrentArrayIndex;
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

	auto FReflectedPropertyEditTarget::ForStructMember(const FProperty* Property, void* StructContainer, uint32 ArrayIndex) const -> FReflectedPropertyEditTarget
	{
		FReflectedPropertyEditTarget Target = *this;
		Target.LeafProperty = Property;
		Target.LeafContainer = StructContainer;
		Target.LeafArrayIndex = ArrayIndex;
		Target.Path.push_back({
			Property,
			Property && Property->GetArrayDim() > 1 ? EPropertyPathSelector::StaticArrayIndex : EPropertyPathSelector::None,
			ArrayIndex
		});
		Target.Kind = EPropertyChangeKind::ValueSet;
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
		return ForMapEntry(EntryProperty, EntryContainer, {}, std::move(SerializedKey));
	}

	auto FReflectedPropertyEditTarget::ForMapEntry(const FProperty* EntryProperty, void* EntryContainer,
		FPropertyValueSnapshot KeySnapshot, std::vector<uint8> SerializedKey) const -> FReflectedPropertyEditTarget
	{
		FReflectedPropertyEditTarget Target = *this;
		Target.LeafProperty = EntryProperty;
		Target.LeafContainer = EntryContainer;
		Target.LeafArrayIndex = 0;
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

	auto GetGenericReflectedPropertyMutationAdapter() -> const IReflectedPropertyMutationAdapter&
	{
		return GGenericMutationAdapter;
	}

	auto RegisterReflectedPropertyMutationAdapter(
		const DClass* ObjectClass,
		FName PropertyName,
		std::unique_ptr<IReflectedPropertyMutationAdapter> Adapter
	) -> bool
	{
		return RegisterMutationAdapter(ObjectClass, PropertyName, std::move(Adapter));
	}

	auto GetReflectedPropertyMutationAdapter(const FReflectedPropertyEditTarget& Target) -> const IReflectedPropertyMutationAdapter&
	{
		if (!Target.Object || !Target.MemberProperty) return GetGenericReflectedPropertyMutationAdapter();
		// Registrations own object members. A member-specific adapter may deliberately
		// interpret nested paths while still capturing the stable outer snapshot.
		for (const DClass* Class = Target.Object->GetClass(); Class; Class = Class->GetSuperClass())
		{
			const auto It = std::ranges::find_if(GetMutationAdapterRegistrations(), [&](const FMutationAdapterRegistration& Entry) {
				return Entry.ObjectClass == Class && Target.MemberProperty->NamePrivate == Entry.PropertyName;
			});
			if (It != GetMutationAdapterRegistrations().end()) return *It->Adapter;
		}
		return GetGenericReflectedPropertyMutationAdapter();
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
		Target.LeafContainer = nullptr;
		Target.LeafArrayIndex = 0;
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
		FReflectedPropertyEditTarget ResolvedTarget;
		if (!ResolveMutationTarget(Target, Adapter, ResolvedTarget, &LastError)) return false;
		const bool bApplied = Adapter == &GGenericMutationAdapter
			? ApplyGenericMutation(ResolvedTarget, Snapshot, EPropertyChangePhase::Committed, Origin, nullptr, &LastError)
			: (Origin == EPropertyChangeOrigin::Undo
				? Adapter->Restore(ResolvedTarget, Snapshot, &LastError)
				: Adapter->Apply(ResolvedTarget, Snapshot, &LastError));
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
		std::vector<FPropertyPathSegment> EventPath = MakeEventPath(Target);
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
		if (!ValidateTarget(Target, OutError))
		{
			Reset();
			return false;
		}
		Adapter = InAdapter ? InAdapter : &GetReflectedPropertyMutationAdapter(Target);
		TransactionManager = InTransactionManager;
		Description = InDescription.empty()
			? std::format("Edit {}", Target.MemberProperty->NamePrivate.ToString())
			: InDescription;
		FReflectedPropertyEditTarget ResolvedTarget;
		if (!ResolveMutationTarget(Target, Adapter, ResolvedTarget, OutError)
			|| !Adapter->Capture(ResolvedTarget, OriginalValue, OutError))
		{
			Reset();
			return false;
		}
		CurrentValue = OriginalValue;
		Target.LeafContainer = nullptr;
		Target.LeafArrayIndex = 0;
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
		FReflectedPropertyEditTarget ResolvedTarget;
		FPropertyValueSnapshot AppliedValue;
		if (!ResolveMutationTarget(Target, Adapter, ResolvedTarget, OutError)) return EReflectedPropertyEditResult::Failed;
		if (Adapter == &GGenericMutationAdapter)
		{
			if (!ApplyGenericMutation(ResolvedTarget, ProposedValue, EPropertyChangePhase::Interactive,
				EPropertyChangeOrigin::Edit, &AppliedValue, OutError)) return EReflectedPropertyEditResult::Failed;
		}
		else if (!Adapter->Apply(ResolvedTarget, ProposedValue, OutError)
			|| !ResolveMutationTarget(Target, Adapter, ResolvedTarget, OutError)
			|| !Adapter->Capture(ResolvedTarget, AppliedValue, OutError)) return EReflectedPropertyEditResult::Failed;
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
			if (!bContinuousKeyRename && (Left.MapKeyData != Right.MapKeyData || Left.MapKey != Right.MapKey)) return false;
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
		FReflectedPropertyEditTarget ResolvedTarget;
		if (bChanged && !ResolveMutationTarget(Target, Adapter, ResolvedTarget, OutError)) return EReflectedPropertyEditResult::Failed;
		if (bChanged && Adapter == &GGenericMutationAdapter)
		{
			if (!ApplyGenericMutation(ResolvedTarget, OriginalValue, EPropertyChangePhase::Cancelled,
				EPropertyChangeOrigin::Edit, nullptr, OutError)) return EReflectedPropertyEditResult::Failed;
		}
		else if (bChanged && !Adapter->Restore(ResolvedTarget, OriginalValue, OutError)) return EReflectedPropertyEditResult::Failed;
		if (bChanged) Notify(EPropertyChangePhase::Cancelled);
		Reset();
		return bChanged ? EReflectedPropertyEditResult::Changed : EReflectedPropertyEditResult::NoChange;
	}

	auto FReflectedPropertyEditSession::Notify(EPropertyChangePhase Phase) const -> void
	{
		std::vector<FPropertyPathSegment> EventPath = MakeEventPath(Target);
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
