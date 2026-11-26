#pragma once

#include "CoreMinimal.h"

#include "DObject/StrongObjectPtr.h"
#include "DObject/WeakObjectPtr.h"
#include "Math/Color.h"
#include "Math/Vector.h"
#include "PropertyEditor/PropertyEditing.h"
#include "Transactions/Transaction.h"

namespace Durin { class DEditorEngine; class FPropertyView; class FPropertyValueDraft; }

namespace Durin::PropertyEditor
{
	namespace FPropertyAccess
	{
		enum Result { MultipleValues, Fail, Success };
	}

	using FPropertyControlValue = std::variant<int32, float, FVector4, FLinearColor>;

	// Coalesces continuous widget changes into one reflected-property transaction.
	class FPropertyEdit
	{
	public:
		DURINED_API ~FPropertyEdit();

		// Operations return success. Optional diagnostics are written on failure.
		FPropertyEdit() = default;
		FPropertyEdit(const FPropertyEdit&) = delete;
		auto operator=(const FPropertyEdit&) -> FPropertyEdit& = delete;

		DURINED_API auto Begin(
			const FPropertyEditTarget& InTarget,
			// An empty description uses "Edit <MemberProperty>" after target validation.
			std::string_view InDescription,
			bool bShouldTransact = true,
			std::string* OutError = nullptr
		) -> bool;
		DURINED_API auto Begin(const FPropertyEditTarget& Target, std::string_view Description,
			DTransactor& Transactor, std::string* OutError = nullptr) -> bool;
		DURINED_API auto Apply(const FPropertyValueSnapshot& ProposedValue, std::string* OutError = nullptr) -> bool;
		DURINED_API auto Apply(const FPropertyValueSnapshotPayload& ProposedValue, std::string* OutError = nullptr) -> bool;
		DURINED_API auto SetValue(const FPropertyControlValue& Value, std::string* OutError = nullptr) -> bool;
		DURINED_API auto Commit(std::string* OutError = nullptr) -> bool;
		DURINED_API auto Cancel(std::string* OutError = nullptr) -> bool;
		// Close a rejected/no-change edit without discarding an enclosing recording.
		// Applied previews must use Cancel instead.
		DURINED_API auto CancelUnchanged(std::string* OutError = nullptr) -> bool;

		auto IsActive() const -> bool { return bActive && !bCommitting; }
		DURINED_API auto MatchesTarget(const FPropertyEditTarget& Other) const -> bool;
		auto IsEditingObject(const DObject* Object) const -> bool { return IsActive() && Target.Object == Object; }
		auto HasChanges() const -> bool { return bActive && !(GetOriginalValue() == CurrentValue); }
		auto GetDescription() const -> std::string_view { return Description; }
		auto GetOriginalValue() const -> const FPropertyValueSnapshotPayload& { return OriginalValue.GetPayload(); }
		auto GetCurrentValue() const -> const FPropertyValueSnapshotPayload& { return CurrentValue; }

	private:
		auto BeginInternal(const FPropertyEditTarget& Target, std::string_view Description,
			DTransactor* Transactor, std::string* OutError, bool bOpenScope = true) -> bool;
		friend class ::Durin::DEditorEngine;
		friend class ::Durin::FPropertyView;
		friend class FPropertyControlFrame;
		auto ApplyDraft(FPropertyValueDraft& Draft, std::string* OutError, bool* OutChanged, bool bRestoring = false) -> bool;
		auto ApplyInternal(const FPropertyValueSnapshotPayload& ProposedValue,
			std::string* OutError, bool* OutChanged, bool bRestoring = false) -> bool;
		auto Reset(bool bCancelRecording = true) -> void;

		auto WriteControlValue(const FPropertyControlValue& Value, std::string* OutError, bool* OutChanged) -> bool;
		template<typename TValue>
		auto WriteValue(const TValue& Value, std::string* OutError, bool* OutChanged) -> bool;
		FPropertyEditTarget Target;
		TStrongObjectPtr<DObject> TargetObject;
		FPropertyValueSnapshot OriginalValue;
		FPropertyValueSnapshotPayload CurrentValue;
		std::string Description;
		std::optional<FScopedTransaction> TransactionScope;
		bool bActive = false;
		bool bCommitting = false;
		bool bInteractiveChangeInProgress = false;
		bool bOwnsRecording = false;
	};

	class IPropertyHandle
	{
	public:
		virtual ~IPropertyHandle() = default;
		virtual auto IsEditable() const -> bool = 0;
		virtual auto GetValue(int32& OutValue) const -> FPropertyAccess::Result = 0;
		virtual auto GetValue(float& OutValue) const -> FPropertyAccess::Result = 0;
		virtual auto GetValue(FVector2& OutValue) const -> FPropertyAccess::Result = 0;
		virtual auto GetValue(FVector3& OutValue) const -> FPropertyAccess::Result = 0;
		virtual auto GetValue(FVector4& OutValue) const -> FPropertyAccess::Result = 0;
		virtual auto GetValue(FLinearColor& OutValue) const -> FPropertyAccess::Result = 0;
		virtual auto SetValue(const int32& Value, EPropertyValueSetFlags::Type Flags = EPropertyValueSetFlags::DefaultFlags) -> FPropertyAccess::Result = 0;
		virtual auto SetValue(const float& Value, EPropertyValueSetFlags::Type Flags = EPropertyValueSetFlags::DefaultFlags) -> FPropertyAccess::Result = 0;
		virtual auto SetValue(const FVector2& Value, EPropertyValueSetFlags::Type Flags = EPropertyValueSetFlags::DefaultFlags) -> FPropertyAccess::Result = 0;
		virtual auto SetValue(const FVector3& Value, EPropertyValueSetFlags::Type Flags = EPropertyValueSetFlags::DefaultFlags) -> FPropertyAccess::Result = 0;
		virtual auto SetValue(const FVector4& Value, EPropertyValueSetFlags::Type Flags = EPropertyValueSetFlags::DefaultFlags) -> FPropertyAccess::Result = 0;
		virtual auto SetValue(const FLinearColor& Value, EPropertyValueSetFlags::Type Flags = EPropertyValueSetFlags::DefaultFlags) -> FPropertyAccess::Result = 0;
	};

	// Resolves a reflected member or nested path on the bound object generation.
	class FReflectedPropertyHandle final : public IPropertyHandle
	{
	public:
		DURINED_API FReflectedPropertyHandle(DObject& Object, FName Member, uint32 ArrayIndex = 0);
		DURINED_API explicit FReflectedPropertyHandle(FPropertyEditTarget Target);
		auto GetLastError() const -> const std::string& { return LastError; }
		DURINED_API auto IsEditable() const -> bool override;
		DURINED_API auto GetValue(int32& OutValue) const -> FPropertyAccess::Result override;
		DURINED_API auto GetValue(float& OutValue) const -> FPropertyAccess::Result override;
		DURINED_API auto GetValue(FVector2& OutValue) const -> FPropertyAccess::Result override;
		DURINED_API auto GetValue(FVector3& OutValue) const -> FPropertyAccess::Result override;
		DURINED_API auto GetValue(FVector4& OutValue) const -> FPropertyAccess::Result override;
		DURINED_API auto GetValue(FLinearColor& OutValue) const -> FPropertyAccess::Result override;
		DURINED_API auto SetValue(const int32& Value, EPropertyValueSetFlags::Type Flags = EPropertyValueSetFlags::DefaultFlags) -> FPropertyAccess::Result override;
		DURINED_API auto SetValue(const float& Value, EPropertyValueSetFlags::Type Flags = EPropertyValueSetFlags::DefaultFlags) -> FPropertyAccess::Result override;
		DURINED_API auto SetValue(const FVector2& Value, EPropertyValueSetFlags::Type Flags = EPropertyValueSetFlags::DefaultFlags) -> FPropertyAccess::Result override;
		DURINED_API auto SetValue(const FVector3& Value, EPropertyValueSetFlags::Type Flags = EPropertyValueSetFlags::DefaultFlags) -> FPropertyAccess::Result override;
		DURINED_API auto SetValue(const FVector4& Value, EPropertyValueSetFlags::Type Flags = EPropertyValueSetFlags::DefaultFlags) -> FPropertyAccess::Result override;
		DURINED_API auto SetValue(const FLinearColor& Value, EPropertyValueSetFlags::Type Flags = EPropertyValueSetFlags::DefaultFlags) -> FPropertyAccess::Result override;

		template<typename T> requires requires { T::StaticStruct(); }
		auto SetValue(const T& Value, EPropertyValueSetFlags::Type Flags = EPropertyValueSetFlags::DefaultFlags) -> FPropertyAccess::Result
		{
			return SetStructValue(T::StaticStruct(), &Value, Flags);
		}

	private:
		friend class FPropertyControlFrame;
		auto ResolveTarget() const -> FPropertyEditTarget;
		auto ResolveMember(DObject*& OutOwner) const -> const FProperty*;
		template<typename TValue>
		auto ReadValue(TValue& OutValue) const -> bool;
		template<typename TValue>
		auto SetValueWithFlags(const TValue& Value, EPropertyValueSetFlags::Type Flags) -> FPropertyAccess::Result;
		DURINED_API auto SetStructValue(const DStruct* Type, const void* Value, EPropertyValueSetFlags::Type Flags) -> FPropertyAccess::Result;
		bool bInteractiveChangeInProgress = false;
		std::string LastError;

		TWeakObjectPtr<DObject> Object;
		FName Member;
		uint32 ArrayIndex = 0;
		std::optional<FPropertyEditTarget> PathTarget;
	};
}
