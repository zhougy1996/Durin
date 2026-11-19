#include "PropertyEditor/PropertyHandle.h"

#include "CoreGlobals.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/MathStructs.h"
#include "DObject/Object.h"
#include "DObject/Property.h"
#include "Math/Color.h"
#include "PropertyEditor/PropertyValueDraft.h"

namespace Durin::PropertyEditor
{
	namespace
	{
		auto Reject(std::string* OutError, std::string_view Message) -> bool
		{
			if (OutError) *OutError = Message;
			return false;
		}

		auto IsEditableMember(const FProperty* Property) -> bool
		{
			return Property && !Property->HasAnyPropertyFlags(EPropertyFlags::ReadOnly);
		}

		template<typename TValue>
		auto MatchesValueType(const FProperty* Property) -> bool
		{
			using DurinCodeGen::EPropertyGenFlags;
			if (!Property || Property->HasValueAccessors()) return false;
			if constexpr (std::is_same_v<TValue, int32>)
				return Property->GetKind() == EPropertyGenFlags::Int32;
			else
				return Property->GetKind() == EPropertyGenFlags::Struct
					&& static_cast<const FStructProperty*>(Property)->GetStruct() == Z_Construct_DStruct_FLinearColor();
		}

		auto IsEditablePath(const FPropertyEditTarget& Target) -> bool
		{
			return std::ranges::all_of(Target.Path, [](const auto& Segment) { return IsEditableMember(Segment.Property); });
		}

		auto IsSupportedMember(const FProperty* Property) -> bool
		{
			return MatchesValueType<int32>(Property) || MatchesValueType<FLinearColor>(Property);
		}

		auto ResolveBoundPath(DObject* Owner, const FPropertyEditTarget& Target, FResolvedPropertyValue& OutValue) -> bool
		{
			return Owner && Target.SnapshotContainer == Owner && ResolveReflectedPropertyValue(Target, OutValue).has_value();
		}
	}

	auto FPropertyEdit::SetValue(int32 Value, std::string* OutError) -> bool
	{
		return WriteValue(Value, OutError);
	}

	auto FPropertyEdit::SetValue(const FLinearColor& Value, std::string* OutError) -> bool
	{
		return WriteValue(Value, OutError);
	}

	template<typename TValue>
	auto FPropertyEdit::WriteValue(const TValue& Value, std::string* OutError, bool* OutChanged) -> bool
	{
		if (!IsActive() || !IsEditablePath(Target) || !MatchesValueType<TValue>(Target.LeafProperty))
			return Reject(OutError, "The property edit cannot write this value.");
		FPropertyValueDraft Draft(Target);
		if (!Draft.IsValid()) return Reject(OutError, FormatPropertyValueDraftError(Draft.GetError()));

		FResolvedPropertyValue Leaf;
		if (const auto Resolved = Draft.Resolve(Target, Leaf.Property, Leaf.Container, Leaf.ArrayIndex); !Resolved)
			return Reject(OutError, FormatPropertyValueDraftError(Resolved.error()));
		*Leaf.Property->ContainerPtrToValuePtr<TValue>(Leaf.Container, Leaf.ArrayIndex) = Value;
		if (const auto Validated = ValidatePropertyEditValue(
			Draft.GetRootProperty(), Draft.GetRootContainer(), Draft.GetRootArrayIndex()); !Validated)
			return Reject(OutError, ToString(Validated.error()));

		FPropertyValueSnapshotPayload Proposed;
		if (const auto Captured = Draft.Capture(Proposed); !Captured)
			return Reject(OutError, FormatPropertyValueDraftError(Captured.error()));
		return Session.ApplyInternal(Proposed, OutError, OutChanged);
	}

	auto FPropertyEdit::WriteControlValue(const std::variant<int32, FLinearColor>& Value, std::string* OutError,
		bool& bOutChanged) -> bool
	{
		return std::visit([&](const auto& TypedValue) { return WriteValue(TypedValue, OutError, &bOutChanged); }, Value);
	}

	auto FPropertyEdit::Commit(std::string* OutError) -> bool
	{
		return Session.Commit(OutError);
	}

	auto FPropertyEdit::Cancel(std::string* OutError) -> bool
	{
		return Session.HasChanges() ? Session.Cancel(OutError) : Session.CancelUnchanged(OutError);
	}

	FReflectedPropertyHandle::FReflectedPropertyHandle(DObject& InObject, FName InMember, uint32 InArrayIndex)
		: Object(&InObject), Member(InMember), ArrayIndex(InArrayIndex)
	{}

	FReflectedPropertyHandle::FReflectedPropertyHandle(FPropertyEditTarget Target)
		: Object(Target.Object), PathTarget(std::move(Target))
	{}

	auto FReflectedPropertyHandle::ResolveMember(DObject*& OutOwner) const -> const FProperty*
	{
		OutOwner = Object.Get();
		const auto* Property = OutOwner ? OutOwner->GetClass()->FindPropertyByName(Member) : nullptr;
		if (!IsSupportedMember(Property) || ArrayIndex >= Property->GetArrayDim()) return nullptr;
		return Property;
	}

	auto FReflectedPropertyHandle::GetValue(int32& OutValue) const -> bool
	{
		return ReadValue(OutValue);
	}

	auto FReflectedPropertyHandle::GetValue(FLinearColor& OutValue) const -> bool
	{
		return ReadValue(OutValue);
	}

	template<typename TValue>
	auto FReflectedPropertyHandle::ReadValue(TValue& OutValue) const -> bool
	{
		FResolvedPropertyValue Resolved;
		if (PathTarget)
		{
			if (!ResolveBoundPath(Object.Get(), *PathTarget, Resolved)) return false;
		}
		else
		{
			DObject* Owner = nullptr;
			const auto* Property = ResolveMember(Owner);
			Resolved = {Property, Owner, ArrayIndex};
		}
		if (!MatchesValueType<TValue>(Resolved.Property)) return false;
		OutValue = *Resolved.Property->ContainerPtrToValuePtr<TValue>(Resolved.Container, Resolved.ArrayIndex);
		return true;
	}

	auto FReflectedPropertyHandle::IsEditable() const -> bool
	{
		if (PathTarget)
		{
			FResolvedPropertyValue Resolved;
			return ResolveBoundPath(Object.Get(), *PathTarget, Resolved)
				&& IsSupportedMember(Resolved.Property) && IsEditablePath(*PathTarget);
		}
		DObject* Owner = nullptr;
		return IsEditableMember(ResolveMember(Owner));
	}

	auto FReflectedPropertyHandle::BeginEdit(FPropertyEdit& Edit, ETransactionMode Mode, std::string* OutError) const -> bool
	{
		if (!IsEditable()) return Reject(OutError, "The property is unavailable or read-only.");
		if (GUndo) return Reject(OutError, "A property edit cannot start inside an active transaction.");
		FPropertyEditTarget Target;
		if (PathTarget) Target = *PathTarget;
		else
		{
			DObject* Owner = nullptr;
			const auto* Property = ResolveMember(Owner);
			Target = FPropertyEditTarget::ForMember(Owner, Property, ArrayIndex);
		}
		auto Result = Edit.Session.Begin(Target, {}, Mode, OutError);
		if (Result) Edit.Target = std::move(Target);
		return Result;
	}

	auto FReflectedPropertyHandle::SetValue(int32 Value, ETransactionMode Mode) const -> bool
	{
		return SetValueAndCommit(Value, Mode);
	}

	auto FReflectedPropertyHandle::SetValue(const FLinearColor& Value, ETransactionMode Mode) const -> bool
	{
		return SetValueAndCommit(Value, Mode);
	}

	template<typename TValue>
	auto FReflectedPropertyHandle::SetValueAndCommit(const TValue& Value, ETransactionMode Mode) const -> bool
	{
		FPropertyEdit Edit;
		if (!BeginEdit(Edit, Mode)) return false;
		if (Edit.SetValue(Value) && Edit.Commit()) return true;
		if (Edit.IsActive()) Edit.Cancel();
		return false;
	}
}
