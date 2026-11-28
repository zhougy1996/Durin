#pragma once

#include "PropertyEditor/PropertyHandle.h"
#include "DObject/WeakObjectPtr.h"

namespace Durin { class FPropertyValueDraft; }

namespace Durin::PropertyEditor
{
	// Resolves a reflected member or nested path on the bound object generation.
	class FPropertyHandleBase : public IPropertyHandle
	{
	public:
		FPropertyHandleBase(DObject& Object, FName Member, uint32 ArrayIndex = 0);
		explicit FPropertyHandleBase(FPropertyEditTarget Target);
		auto SetValueFromSnapshot(const FPropertyValueSnapshot& Value, EPropertyValueSetFlags::Type Flags = EPropertyValueSetFlags::DefaultFlags)
			-> std::expected<bool, EPropertyWriteFailure> override;
		auto GetLastError() const -> const std::string& override { return LastError; }
		auto IsEditable() const -> bool override;
		auto GetValue(int32& OutValue) const -> FPropertyAccess::Result override;
		auto GetValue(float& OutValue) const -> FPropertyAccess::Result override;
		auto GetValue(FVector2& OutValue) const -> FPropertyAccess::Result override;
		auto GetValue(FVector3& OutValue) const -> FPropertyAccess::Result override;
		auto GetValue(FVector4& OutValue) const -> FPropertyAccess::Result override;
		auto GetValue(FLinearColor& OutValue) const -> FPropertyAccess::Result override;
		auto SetValue(const int32& Value, EPropertyValueSetFlags::Type Flags = EPropertyValueSetFlags::DefaultFlags) -> FPropertyAccess::Result override;
		auto SetValue(const float& Value, EPropertyValueSetFlags::Type Flags = EPropertyValueSetFlags::DefaultFlags) -> FPropertyAccess::Result override;
		auto SetValue(const FVector2& Value, EPropertyValueSetFlags::Type Flags = EPropertyValueSetFlags::DefaultFlags) -> FPropertyAccess::Result override;
		auto SetValue(const FVector3& Value, EPropertyValueSetFlags::Type Flags = EPropertyValueSetFlags::DefaultFlags) -> FPropertyAccess::Result override;
		auto SetValue(const FVector4& Value, EPropertyValueSetFlags::Type Flags = EPropertyValueSetFlags::DefaultFlags) -> FPropertyAccess::Result override;
		auto SetValue(const FLinearColor& Value, EPropertyValueSetFlags::Type Flags = EPropertyValueSetFlags::DefaultFlags) -> FPropertyAccess::Result override;

	private:
		auto FinishInteractiveChange() -> void override;
		auto WriteDraft(const FPropertyEditTarget& WriteTarget, FPropertyValueDraft& Draft, EPropertyValueSetFlags::Type Flags,
			FPropertyValueSnapshotPayload& LiveValue) -> std::expected<bool, std::string>;
		auto NotifyValueSet(const FPropertyEditTarget& Target, EPropertyValueSetFlags::Type Flags, bool bChanged) -> void;
		auto ResolveTarget() const -> FPropertyEditTarget override;
		auto ResolveEditablePath() const -> const FPropertyEditTarget*;
		auto ResolveMember(DObject*& OutOwner) const -> const FProperty*;
		template<typename TValue>
		auto ReadValue(TValue& OutValue) const -> bool;
		template<typename TValue>
		auto SetValueWithFlags(const TValue& Value, EPropertyValueSetFlags::Type Flags) -> FPropertyAccess::Result;
		auto SetStructValue(const DStruct* Type, const void* Value, EPropertyValueSetFlags::Type Flags) -> FPropertyAccess::Result override;
		bool bInteractiveChangeInProgress = false;
		std::string LastError;

		TWeakObjectPtr<DObject> Object;
		FName Member;
		uint32 ArrayIndex = 0;
		std::optional<FPropertyEditTarget> PathTarget;
	};
}
