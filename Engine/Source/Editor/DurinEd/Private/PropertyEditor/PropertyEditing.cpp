#include "PropertyEditor/PropertyHandleImpl.h"

#include "PropertyEditor/PropertyValueDraft.h"
#include "Editor/EditorEngine.h"

#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/Object.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Property.h"
#include "DObject/TransactionObjectReference.h"
#include "Logging/LogMacros.h"
#include "Misc/AssertionMacros.h"
#include "Tools/EditorScriptCall.h"

namespace Durin
{
	namespace
	{
		struct FResolveMapEntryContext
		{
			FProperty* KeyProperty = nullptr;
			const FPropertyValueSnapshotPayload* TargetKey = nullptr;
			const void* Key = nullptr;
			void* Value = nullptr;
			std::optional<FPropertySnapshotError> Error;
		};

		auto ResolveMapEntry(void* RawContext, const void* Key, void* Value) -> bool
		{
			auto& Context = *static_cast<FResolveMapEntryContext*>(RawContext);
			FPropertyValueSnapshotPayload StoredKey;
			if (const auto Result = CapturePropertyValuePayload(Context.KeyProperty, Key, 0, StoredKey); !Result)
			{
				Context.Error = Result.error();
				return false;
			}
			if (StoredKey == *Context.TargetKey)
			{
				Context.Key = Key;
				Context.Value = Value;
				return false;
			}
			return true;
		}

		auto CaptureTargetValue(const FPropertyEditTarget& Target,
			FPropertyValueSnapshotPayload& OutSnapshot) -> std::expected<void, FPropertySnapshotError>
		{
			return CapturePropertyValuePayload(Target.SnapshotProperty, Target.SnapshotContainer,
				Target.SnapshotArrayIndex, OutSnapshot);
		}

		auto RestoreTargetValue(const FPropertyEditTarget& Target,
			const FPropertyValueSnapshotPayload& Snapshot) -> std::expected<void, FPropertySnapshotError>
		{
			return RestorePropertyValuePayload(Target.SnapshotProperty, Target.SnapshotContainer,
				Target.SnapshotArrayIndex, Snapshot);
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

		constexpr auto NestedMutationError = "A reflected property hook cannot start a nested edit of the same target.";
		class FGenericMutationScope
		{
		public:
			static auto IsActiveFor(const FPropertyEditTarget& Target) -> bool
			{
				return std::ranges::any_of(GActiveGenericMutations,
					[&](const auto* Active) { return Active->IsSameMutationTarget(Target); });
			}
			explicit FGenericMutationScope(const FPropertyEditTarget& Target)
				{ GActiveGenericMutations.push_back(&Target); }
			~FGenericMutationScope() { GActiveGenericMutations.pop_back(); }
		};

		auto RejectPath(const FPropertyEditTarget& Target, EPropertyEditPathError Code,
			uint64 Index = 0, uint64 Actual = 0, uint64 Expected = 0,
			std::optional<EContainerOpResult> ContainerCause = {},
			std::optional<FPropertySnapshotError> SnapshotCause = {}) -> std::expected<void, FPropertyEditPathError>
		{
			FPropertyEditPathError Error{.Code = Code, .Owner = FObjectKey(Target.Object),
				.Member = Target.MemberProperty ? Target.MemberProperty->NamePrivate.ToString() : std::string{},
				.Leaf = Target.LeafProperty ? Target.LeafProperty->NamePrivate.ToString() : std::string{},
				.Snapshot = Target.SnapshotProperty ? Target.SnapshotProperty->NamePrivate.ToString() : std::string{},
				.HasSnapshotContainer = Target.SnapshotContainer != nullptr, .SnapshotArrayIndex = Target.SnapshotArrayIndex,
				.PathIndex = Index, .Actual = Actual, .Expected = Expected,
				.ContainerCause = ContainerCause, .SnapshotCause = std::move(SnapshotCause)};
			for (const auto& Segment : Target.Path)
				Error.Path.push_back({Segment.Property ? Segment.Property->NamePrivate.ToString() : std::string{},
					Segment.Selector, Segment.Index, Segment.MapKeyData, Segment.MapKey});
			return std::unexpected(std::move(Error));
		}

		auto ValidateTarget(const FPropertyEditTarget& Target) -> std::expected<void, FPropertyEditPathError>
		{
			if (!Target.Object) return RejectPath(Target, EPropertyEditPathError::MissingOwner);
			if (!Target.MemberProperty || !Target.LeafProperty || !Target.SnapshotProperty || !Target.SnapshotContainer)
				return RejectPath(Target, EPropertyEditPathError::IncompleteTarget);
			if (Target.SnapshotArrayIndex >= Target.SnapshotProperty->GetArrayDim())
				return RejectPath(Target, EPropertyEditPathError::SnapshotIndex, 0,
					Target.SnapshotArrayIndex, Target.SnapshotProperty->GetArrayDim());
			if (Target.Path.empty() || Target.Path.front().Property != Target.MemberProperty || Target.Path.back().Property != Target.LeafProperty)
				return RejectPath(Target, EPropertyEditPathError::Endpoints);
			for (size_t Index = 0; Index < Target.Path.size(); ++Index)
			{
				const auto& Segment = Target.Path[Index];
				if (!Segment.Property) return RejectPath(Target, EPropertyEditPathError::EmptySegment, Index);
				if (Segment.Selector != EPropertyPathSelector::MapKey && !Segment.MapKeyData.empty())
					return RejectPath(Target, EPropertyEditPathError::UnexpectedKeyData, Index, Segment.MapKeyData.size());
			}
			return {};
		}
	}

	static auto NotifyMutation(
		const FPropertyEditTarget& Target,
		PropertyEditor::EPropertyValueSetFlags::Type Flags
	) -> void
	{
		const TStrongObjectPtr<DObject> Owner(Target.Object);
		const auto Path = MakeEventPath(Target);
		auto ChangeType = Target.ChangeType;
		if (Flags & PropertyEditor::EPropertyValueSetFlags::InteractiveChange)
			ChangeType = ChangeType == EPropertyChangeType::ValueSet
				? EPropertyChangeType::Interactive : ChangeType | EPropertyChangeType::Interactive;
		const FPropertyChangedEvent Event{Target.MemberProperty, Target.LeafProperty, Path, ChangeType};
		Owner->PostEditChangeProperty(Event);
	}

	auto PropertyEditor::FPropertyHandleBase::NotifyValueSet(const FPropertyEditTarget& Target, EPropertyValueSetFlags::Type Flags, bool bChanged) -> void
	{
		const bool bInteractive = (Flags & EPropertyValueSetFlags::InteractiveChange) != 0;
		if (!bChanged && (bInteractive || !bInteractiveChangeInProgress)) return;
		bInteractiveChangeInProgress = bInteractive;
		NotifyMutation(Target, Flags);
	}

	auto FormatPropertyValueDraftError(const FPropertyValueDraftError& Error) -> std::string
	{
		if (Error.SnapshotCause) return ToString(*Error.SnapshotCause);
		if (Error.ValueCause) return ToString(*Error.ValueCause);
		if (Error.PathCause) return FormatPropertyEditPathError(*Error.PathCause);
		switch (Error.Code)
		{
		case EPropertyValueDraftError::None: return {};
		case EPropertyValueDraftError::MissingRoot: return "The reflected property draft root is unavailable.";
		case EPropertyValueDraftError::Accessors: return "Properties with custom value accessors cannot be used as draft roots.";
		case EPropertyValueDraftError::Lifecycle: return "The reflected property lacks generated draft-value lifecycle metadata.";
		case EPropertyValueDraftError::RootMismatch: return "The edit target does not match its reflected property draft root.";
		default: return "The reflected property draft operation failed.";
		}
	}

	auto FPropertyEditTarget::Validate() const -> std::expected<void, FPropertyEditPathError> { return ValidateTarget(*this); }

	auto FormatPropertyEditPathError(const FPropertyEditPathError& Error) -> std::string
	{
		if (Error.SnapshotCause) return ToString(*Error.SnapshotCause);
		switch (Error.Code)
		{
		case EPropertyEditPathError::None: return {};
		case EPropertyEditPathError::MissingOwner: return "The edit target has no owning object.";
		case EPropertyEditPathError::IncompleteTarget: return "The edit target is incomplete.";
		case EPropertyEditPathError::SnapshotIndex: return "The snapshot property array index is out of range.";
		case EPropertyEditPathError::Endpoints: return "The property path must run from the member property to the leaf property.";
		case EPropertyEditPathError::EmptySegment: return "The property path contains an empty segment.";
		case EPropertyEditPathError::UnexpectedKeyData: return "Only map-key path segments may contain serialized key data.";
		case EPropertyEditPathError::SnapshotRoot: return "The property path does not begin at the snapshot root.";
		case EPropertyEditPathError::ArrayProperty: return "The reflected array path property is not an array.";
		case EPropertyEditPathError::ArrayCount: return "The reflected array path count is unavailable.";
		case EPropertyEditPathError::ArrayIndex: return "The reflected array path index is unavailable.";
		case EPropertyEditPathError::ArrayAccess: return "The reflected array path requires mutable random access.";
		case EPropertyEditPathError::MapSnapshot: return "The reflected map path lacks a stable key snapshot.";
		case EPropertyEditPathError::MapTraversal: return "The reflected map path requires mutable mapped traversal.";
		case EPropertyEditPathError::MapCapture: return "The reflected map key snapshot could not be captured.";
		case EPropertyEditPathError::MapMissing: return "The reflected map key is unavailable.";
		case EPropertyEditPathError::MapSelection: return "The reflected map path does not select its key or value property.";
		case EPropertyEditPathError::Selector: return "The reflected property path selector is unsupported.";
		case EPropertyEditPathError::Unresolved: return "The reflected property path could not be resolved.";
		case EPropertyEditPathError::Empty: return "The reflected property path is empty.";
		}
		return "Unknown reflected property path failure.";
	}

	auto ResolveReflectedPropertyValue(
		const FPropertyEditTarget& Target,
		FResolvedPropertyValue& OutValue
	) -> std::expected<void, FPropertyEditPathError>
	{
		if (const auto Valid = ValidateTarget(Target); !Valid) return Valid;
		if (Target.Path.front().Property != Target.SnapshotProperty)
			return RejectPath(Target, EPropertyEditPathError::SnapshotRoot);

		void* Container = Target.SnapshotContainer;
		uint32 CurrentArrayIndex = Target.SnapshotArrayIndex;
		for (size_t PathIndex = 0; PathIndex < Target.Path.size(); ++PathIndex)
		{
			const FPropertyEditPathSegment& Segment = Target.Path[PathIndex];
			auto* CurrentProperty = const_cast<FProperty*>(Segment.Property);
			if (PathIndex + 1 == Target.Path.size())
			{
				OutValue = {CurrentProperty, Container, CurrentArrayIndex};
				return {};
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
				if (!ArrayProperty) return RejectPath(Target, EPropertyEditPathError::ArrayProperty, PathIndex,
					static_cast<uint64>(CurrentProperty->GetKind()), static_cast<uint64>(DurinCodeGen::EPropertyGenFlags::Array));
				uint64 Num = 0;
				const auto Counted = ArrayProperty->GetNum(Container, Num, CurrentArrayIndex);
				if (Counted != EContainerOpResult::Success)
					return RejectPath(Target, EPropertyEditPathError::ArrayCount, PathIndex, 0, 0, Counted);
				if (Segment.Index >= Num)
					return RejectPath(Target, EPropertyEditPathError::ArrayIndex, PathIndex, Segment.Index, Num);
				void* Element = nullptr;
				const auto Accessed = ArrayProperty->GetMutableElement(Container, Segment.Index, &Element, CurrentArrayIndex);
				if (Accessed != EContainerOpResult::Success)
					return RejectPath(Target, EPropertyEditPathError::ArrayAccess, PathIndex, Segment.Index, Num, Accessed);
				Container = Element;
				CurrentArrayIndex = 0;
				break;
			}
			case EPropertyPathSelector::MapKey:
			{
				auto* MapProperty = CurrentProperty->GetKind() == DurinCodeGen::EPropertyGenFlags::Map
					? static_cast<FMapProperty*>(CurrentProperty) : nullptr;
				if (!MapProperty || !Segment.MapKey.IsValid())
					return RejectPath(Target, EPropertyEditPathError::MapSnapshot, PathIndex,
						static_cast<uint64>(CurrentProperty->GetKind()), static_cast<uint64>(DurinCodeGen::EPropertyGenFlags::Map));
				FResolveMapEntryContext ResolveContext{MapProperty->GetKeyProp(), &Segment.MapKey};
				const EContainerOpResult VisitResult = MapProperty->VisitMutableEntries(
					Container, &ResolveMapEntry, &ResolveContext, CurrentArrayIndex);
				if (VisitResult != EContainerOpResult::Success)
					return RejectPath(Target, EPropertyEditPathError::MapTraversal, PathIndex, 0, 0, VisitResult);
				if (ResolveContext.Error) return RejectPath(Target, EPropertyEditPathError::MapCapture, PathIndex, 0, 0, {}, std::move(ResolveContext.Error));
				if (!ResolveContext.Key) return RejectPath(Target, EPropertyEditPathError::MapMissing, PathIndex);
				if (NextProperty == MapProperty->GetKeyProp())
					Container = const_cast<void*>(ResolveContext.Key);
				else if (NextProperty == MapProperty->GetValueProp())
					Container = ResolveContext.Value;
				else
					return RejectPath(Target, EPropertyEditPathError::MapSelection, PathIndex);
				CurrentArrayIndex = 0;
				break;
			}
			default:
				return RejectPath(Target, EPropertyEditPathError::Selector, PathIndex);
			}
			if (!Container || !NextProperty) return RejectPath(Target, EPropertyEditPathError::Unresolved, PathIndex);
		}
		return RejectPath(Target, EPropertyEditPathError::Empty);
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
		Target.ChangeType = EPropertyChangeType::ValueSet;
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
		Target.ChangeType = EPropertyChangeType::ValueSet;
		return Target;
	}

	auto FPropertyEditTarget::ForMapEntry(const FProperty* EntryProperty, FByteBuffer SerializedKey) const -> FPropertyEditTarget
	{
		return ForMapEntry(EntryProperty, FPropertyValueSnapshotPayload{}, std::move(SerializedKey));
	}

	auto FPropertyEditTarget::ForMapEntry(const FProperty* EntryProperty,
		FPropertyValueSnapshotPayload KeySnapshot, FByteBuffer SerializedKey) const -> FPropertyEditTarget
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
		Target.ChangeType = EPropertyChangeType::ValueSet;
		return Target;
	}

	auto FPropertyEditTarget::ForMapEntry(
		const FProperty* EntryProperty,
		const FPropertyValueSnapshot& KeySnapshot,
		FByteBuffer SerializedKey) const -> FPropertyEditTarget
	{
		return ForMapEntry(
			EntryProperty, KeySnapshot.GetPayload(), std::move(SerializedKey));
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

	namespace
	{
		auto IsMapKeyTarget(const FPropertyEditTarget& Target) -> bool
		{
			if (Target.Path.size() < 2) return false;
			const auto& Parent = Target.Path[Target.Path.size() - 2];
			return Parent.Selector == EPropertyPathSelector::MapKey && Parent.Property
				&& Parent.Property->GetKind() == DurinCodeGen::EPropertyGenFlags::Map
				&& static_cast<const FMapProperty*>(Parent.Property)->GetKeyProp() == Target.LeafProperty;
		}
	}

	auto FPropertyEditTarget::MatchesContinuousEdit(const FPropertyEditTarget& Other) const -> bool
	{
		if (Object != Other.Object || MemberProperty != Other.MemberProperty
			|| LeafProperty != Other.LeafProperty || SnapshotProperty != Other.SnapshotProperty
			|| SnapshotContainer != Other.SnapshotContainer || SnapshotArrayIndex != Other.SnapshotArrayIndex
			|| ChangeType != Other.ChangeType || LogicalIdentity != Other.LogicalIdentity
			|| Path.size() != Other.Path.size()) return false;
		for (size_t Index = 0; Index < Path.size(); ++Index)
		{
			const FPropertyEditPathSegment& Left = Path[Index];
			const FPropertyEditPathSegment& Right = Other.Path[Index];
			if (Left.Property != Right.Property || Left.Selector != Right.Selector || Left.Index != Right.Index) return false;
			const bool bContinuousKeyRename = Index + 2 == Path.size() && IsMapKeyTarget(*this);
			if (!bContinuousKeyRename && (Left.MapKeyData != Right.MapKeyData || Left.MapKey != Right.MapKey)) return false;
		}
		return true;
	}

	auto PropertyEditor::EnrollPropertyTarget(const FPropertyEditTarget& Target) -> std::expected<void, std::string>
	{
		if (!Target.Object->IsTemplateObject()) Target.Object->SetFlags(EObjectFlags::Transactional);
		if (!GUndo || !CanTransactObject(*Target.Object) || !Target.Object->HasAnyObjectFlags(EObjectFlags::Transactional))
			return std::unexpected("The target cannot participate in transaction history.");
		return {};
	}

	auto PropertyEditor::BeginPropertyRecording(const FPropertyEditTarget& Target, std::string_view Description,
		DTransactor& Transactor, bool bOpenScope, std::optional<FScopedTransaction>& Scope)
		-> std::expected<void, std::string>
	{
		if (!GEditor || GEditor->GetTransactor() != &Transactor)
			return std::unexpected("Property recording requires the active editor's transactor.");
		if (bOpenScope)
		{
			Scope.emplace("ReflectedProperty", Description, Target.Object);
			if (!Scope->IsOutstanding())
				return std::unexpected("The reflected-property transactor rejected the edit scope.");
		}
		return EnrollPropertyTarget(Target);
	}

	auto PropertyEditor::FPropertyHandleBase::WriteDraft(const FPropertyEditTarget& WriteTarget, FPropertyValueDraft& Draft, EPropertyValueSetFlags::Type Flags,
		FPropertyValueSnapshotPayload& LiveValue) -> std::expected<bool, std::string>
	{
		if (FGenericMutationScope::IsActiveFor(WriteTarget)) return std::unexpected(NestedMutationError);
		FGenericMutationScope Scope(WriteTarget);

		if (!Draft.IsValid()) return std::unexpected(FormatPropertyValueDraftError(Draft.GetError()));

		FResolvedPropertyValue DraftLeaf;
		const auto LeafResult = Draft.Resolve(WriteTarget, DraftLeaf.Property, DraftLeaf.Container, DraftLeaf.ArrayIndex);
		const bool bResolvedLeaf = static_cast<bool>(LeafResult);
		if (!bResolvedLeaf && !IsMapKeyTarget(WriteTarget)
			&& !(WriteTarget.ChangeType == EPropertyChangeType::ArrayRemove && !WriteTarget.Path.empty()
				&& WriteTarget.Path.back().Selector == EPropertyPathSelector::MapKey))
			return std::unexpected(FormatPropertyValueDraftError(LeafResult.error()));

		FPropertyEditProposal Proposal{
			WriteTarget.MemberProperty,
			WriteTarget.LeafProperty,
			Flags,
			Draft.GetRootProperty(),
			Draft.GetRootContainer(),
			Draft.GetRootArrayIndex(),
			bResolvedLeaf ? DraftLeaf.Container : nullptr,
			bResolvedLeaf ? DraftLeaf.ArrayIndex : 0
		};
		const TWeakObjectPtr<DObject> TargetObject(WriteTarget.Object);
		constexpr auto Unavailable = "The property edit target is unavailable.";
		DObject* Object = TargetObject.Get();
		if (!Object) return std::unexpected(Unavailable);
		if (const auto Validation = Object->PreEditChangeProperty(Proposal); !Validation)
			return std::unexpected(ToString(Validation.error()));

		FPropertyValueSnapshotPayload Normalized;
		if (const auto Result = Draft.Capture(Normalized); !Result)
			return std::unexpected(FormatPropertyValueDraftError(Result.error()));
		Object = TargetObject.Get();
		if (!Object) return std::unexpected(Unavailable);
		// Proposal hooks can retire a document, target or endpoint. Check live authority
		// at the publication boundary, after all synchronous normalization callbacks.
		if (auto* Script = FScopedEditorScriptCall::Current(); Script
			&& Script->ResolveEditAccess(*Object) != EEditorScriptTargetAccess::Writable)
		{
			Script->Reject("Property target is no longer writable.");
			return std::unexpected("Script property publication lost target admission.");
		}
		if (!Object->CanEditChange(WriteTarget.MemberProperty))
		{
			constexpr auto ReadOnly = "The property is no longer editable.";
			if (auto* Script = FScopedEditorScriptCall::Current()) Script->Reject(ReadOnly);
			return std::unexpected(ReadOnly);
		}
		if (Normalized == LiveValue) return false;
		if (WriteTarget.SnapshotProperty->HasAnyPropertyFlags(EPropertyFlags::NonTransactional))
			Object->MarkPackageDirty();
		else
			(void)Object->Modify();
		if (!TargetObject.IsValid()) return std::unexpected(Unavailable);
		FPropertyValueSnapshotPayload Applied;
		if (const auto Publication = RestoreTargetValue(WriteTarget, Normalized); !Publication)
		{
			LiveValue = {};
			return std::unexpected(ToString(Publication.error()));
		}
		if (const auto Capture = CaptureTargetValue(WriteTarget, Applied); !Capture)
		{
			LiveValue = {};
			return std::unexpected(ToString(Capture.error()));
		}
		const bool bChanged = Applied != LiveValue;
		LiveValue = std::move(Applied);
		return bChanged;
	}

}
