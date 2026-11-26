#include "PropertyEditor/PropertyHandle.h"

#include "CoreGlobals.h"
#include "Editor/EditorEngine.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/MathStructs.h"
#include "DObject/Object.h"
#include "DObject/Property.h"
#include "Math/Color.h"
#include "Math/Vector.h"
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

		struct FStructValue
		{
			const DStruct* Type;
			const void* Data;
		};

		template<typename TValue>
		auto MatchesValueType(const FProperty* Property) -> bool
		{
			using DurinCodeGen::EPropertyGenFlags;
			if (!Property || Property->HasValueAccessors()) return false;
			if constexpr (std::is_same_v<TValue, int32>) return Property->GetKind() == EPropertyGenFlags::Int32;
			if constexpr (std::is_same_v<TValue, float>) return Property->GetKind() == EPropertyGenFlags::Float;
			if (Property->GetKind() != EPropertyGenFlags::Struct) return false;
			const auto* Type = static_cast<const FStructProperty*>(Property)->GetStruct();
			if constexpr (std::is_same_v<TValue, FVector2>) return Type == Z_Construct_DStruct_FVector2();
			if constexpr (std::is_same_v<TValue, FVector3>) return Type == Z_Construct_DStruct_FVector3();
			if constexpr (std::is_same_v<TValue, FVector4>) return Type == Z_Construct_DStruct_FVector4();
			if constexpr (std::is_same_v<TValue, FLinearColor>) return Type == Z_Construct_DStruct_FLinearColor();
			return std::is_same_v<TValue, FStructValue>;
		}

		auto IsEditablePath(const FPropertyEditTarget& Target) -> bool
		{
			return Target.Object->CanEditChange(Target.MemberProperty)
				&& std::ranges::all_of(Target.Path, [](const auto& Segment) { return IsEditableMember(Segment.Property); });
		}

		auto IsSupportedMember(const FProperty* Property) -> bool
		{
			return MatchesValueType<int32>(Property) || MatchesValueType<float>(Property) || MatchesValueType<FStructValue>(Property);
		}

		auto ResolveBoundPath(DObject* Owner, const FPropertyEditTarget& Target, FResolvedPropertyValue& OutValue) -> bool
		{
			return Owner && Target.SnapshotContainer == Owner && ResolveReflectedPropertyValue(Target, OutValue).has_value();
		}
	}

	template<typename TValue>
	static auto PrepareTypedValue(const FPropertyEditTarget& Target, const TValue& Value,
		FPropertyValueDraft& Draft, FPropertyValueSnapshotPayload& Proposed, std::string* OutError) -> bool
	{
		if (!IsEditablePath(Target) || !MatchesValueType<TValue>(Target.LeafProperty))
			return Reject(OutError, "The property edit cannot write this value.");
		if (!Draft.IsValid()) return Reject(OutError, FormatPropertyValueDraftError(Draft.GetError()));

		FResolvedPropertyValue Leaf;
		if (const auto Resolved = Draft.Resolve(Target, Leaf.Property, Leaf.Container, Leaf.ArrayIndex); !Resolved)
			return Reject(OutError, FormatPropertyValueDraftError(Resolved.error()));
		if constexpr (std::is_same_v<TValue, FStructValue>)
		{
			if (static_cast<const FStructProperty*>(Leaf.Property)->GetStruct() != Value.Type
				|| !Leaf.Property->CopyAssignValue(Leaf.Property->GetValuePtr(Leaf.Container, Leaf.ArrayIndex), Value.Data))
				return Reject(OutError, "Incompatible reflected structure.");
		}
		else *Leaf.Property->ContainerPtrToValuePtr<TValue>(Leaf.Container, Leaf.ArrayIndex) = Value;
		if (const auto Validated = ValidatePropertyEditValue(
			Draft.GetRootProperty(), Draft.GetRootContainer(), Draft.GetRootArrayIndex()); !Validated)
			return Reject(OutError, ToString(Validated.error()));

		if (const auto Captured = Draft.Capture(Proposed); !Captured)
			return Reject(OutError, FormatPropertyValueDraftError(Captured.error()));
		return true;
	}

	template<typename TValue>
	auto FPropertyEdit::WriteValue(const TValue& Value, std::string* OutError, bool* OutChanged) -> bool
	{
		if (!IsActive())
			return Reject(OutError, "The property edit cannot write this value.");
		FPropertyValueDraft Draft(Target);
		FPropertyValueSnapshotPayload Proposed;
		if (!PrepareTypedValue(Target, Value, Draft, Proposed, OutError)) return false;
		if (Proposed == CurrentValue)
		{
			if (OutChanged) *OutChanged = false;
			return true;
		}
		return ApplyDraft(Draft, OutError, OutChanged);
	}

	auto FPropertyEdit::SetValue(const FPropertyControlValue& Value, std::string* OutError) -> bool
	{
		return WriteControlValue(Value, OutError, nullptr);
	}

	auto FPropertyEdit::WriteControlValue(const FPropertyControlValue& Value, std::string* OutError, bool* OutChanged) -> bool
	{
		return std::visit([&](const auto& TypedValue) { return WriteValue(TypedValue, OutError, OutChanged); }, Value);
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

	auto FReflectedPropertyHandle::GetValue(int32& OutValue) const -> FPropertyAccess::Result
	{
		return ReadValue(OutValue) ? FPropertyAccess::Success : FPropertyAccess::Fail;
	}

	auto FReflectedPropertyHandle::GetValue(float& OutValue) const -> FPropertyAccess::Result
	{
		return ReadValue(OutValue) ? FPropertyAccess::Success : FPropertyAccess::Fail;
	}

	auto FReflectedPropertyHandle::GetValue(FVector2& OutValue) const -> FPropertyAccess::Result
	{
		return ReadValue(OutValue) ? FPropertyAccess::Success : FPropertyAccess::Fail;
	}

	auto FReflectedPropertyHandle::GetValue(FVector3& OutValue) const -> FPropertyAccess::Result
	{
		return ReadValue(OutValue) ? FPropertyAccess::Success : FPropertyAccess::Fail;
	}

	auto FReflectedPropertyHandle::GetValue(FVector4& OutValue) const -> FPropertyAccess::Result
	{
		return ReadValue(OutValue) ? FPropertyAccess::Success : FPropertyAccess::Fail;
	}

	auto FReflectedPropertyHandle::GetValue(FLinearColor& OutValue) const -> FPropertyAccess::Result
	{
		return ReadValue(OutValue) ? FPropertyAccess::Success : FPropertyAccess::Fail;
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
		const auto* Property = ResolveMember(Owner);
		return Owner && Property && Owner->CanEditChange(Property);
	}

	auto FReflectedPropertyHandle::ResolveTarget() const -> FPropertyEditTarget
	{
		if (PathTarget) return *PathTarget;
		DObject* Owner = nullptr;
		const auto* Property = ResolveMember(Owner);
		return FPropertyEditTarget::ForMember(Owner, Property, ArrayIndex);
	}

	auto FReflectedPropertyHandle::SetValue(const int32& Value, EPropertyValueSetFlags::Type Flags) -> FPropertyAccess::Result
	{
		return SetValueWithFlags(Value, Flags);
	}

	auto FReflectedPropertyHandle::SetValue(const float& Value, EPropertyValueSetFlags::Type Flags) -> FPropertyAccess::Result
	{
		return SetValueWithFlags(Value, Flags);
	}

	auto FReflectedPropertyHandle::SetValue(const FVector2& Value, EPropertyValueSetFlags::Type Flags) -> FPropertyAccess::Result
	{
		return SetValueWithFlags(Value, Flags);
	}

	auto FReflectedPropertyHandle::SetValue(const FVector3& Value, EPropertyValueSetFlags::Type Flags) -> FPropertyAccess::Result
	{
		return SetValueWithFlags(Value, Flags);
	}

	auto FReflectedPropertyHandle::SetValue(const FVector4& Value, EPropertyValueSetFlags::Type Flags) -> FPropertyAccess::Result
	{
		return SetValueWithFlags(Value, Flags);
	}

	auto FReflectedPropertyHandle::SetValue(const FLinearColor& Value, EPropertyValueSetFlags::Type Flags) -> FPropertyAccess::Result
	{
		return SetValueWithFlags(Value, Flags);
	}

	auto FReflectedPropertyHandle::SetStructValue(const DStruct* Type, const void* Value, EPropertyValueSetFlags::Type Flags) -> FPropertyAccess::Result
	{
		return SetValueWithFlags(FStructValue{Type, Value}, Flags);
	}

	template<typename TValue>
	auto FReflectedPropertyHandle::SetValueWithFlags(const TValue& Value, EPropertyValueSetFlags::Type Flags) -> FPropertyAccess::Result
	{
		LastError.clear();
		auto Fail = [&](std::string_view Message) { LastError = Message; return FPropertyAccess::Fail; };
		if (!IsEditable()) return Fail("The property is unavailable or read-only.");
		const auto Target = ResolveTarget();
		if (!Target.Object || !Target.SnapshotProperty) return Fail("The property edit target is unavailable.");
		const TStrongObjectPtr<DObject> Owner(Target.Object);
		if (!Owner || (GEditor && GEditor->IsShutdownRequested()))
			return Fail("The property edit target is unavailable.");

		std::optional<FScopedTransaction> Scope;
		if (!Target.SnapshotProperty->HasAnyPropertyFlags(EPropertyFlags::NonTransactional))
		{
			const bool bOpenScope = !(Flags & EPropertyValueSetFlags::NotTransactable) && !GUndo;
			const auto Transactor = ResolveEditorTransactor(bOpenScope);
			if (!Transactor) return Fail(Transactor.error());
			if (*Transactor)
			{
				auto Recording = BeginPropertyRecording(Target, std::format("Edit {}", Target.MemberProperty->NamePrivate),
					**Transactor, bOpenScope, Scope);
				if (!Recording)
				{
					if (Scope) Scope->Cancel();
					return Fail(Recording.error());
				}
			}
		}

		if (!Owner.IsValid()) return Fail("The property edit target is unavailable.");
		FPropertyValueSnapshotPayload Before;
		if (const auto Capture = CapturePropertyValuePayload(Target.SnapshotProperty, Target.SnapshotContainer,
			Target.SnapshotArrayIndex, Before); !Capture)
			return Fail(ToString(Capture.error()));
		FPropertyValueDraft Draft(Target, &Before);
		FPropertyValueSnapshotPayload Proposed;
		const bool bInteractive = (Flags & EPropertyValueSetFlags::InteractiveChange) != 0;
		if (!PrepareTypedValue(Target, Value, Draft, Proposed, &LastError)) return FPropertyAccess::Fail;
		const bool bProposedChange = Proposed != Before;
		bool bChanged = false;
		if (bProposedChange)
		{
			auto Result = WriteValidatedValue(Target, Draft, Flags, Before);
			if (!Result)
			{
				LastError = std::move(Result.error());
				return FPropertyAccess::Fail;
			}
			bChanged = *Result;
		}
		if (bChanged || (!bInteractive && bInteractiveChangeInProgress))
			NotifyMutation(Target, Flags);
		Scope.reset();
		if (bChanged || !bInteractive) bInteractiveChangeInProgress = bInteractive;
		return FPropertyAccess::Success;
	}
}
