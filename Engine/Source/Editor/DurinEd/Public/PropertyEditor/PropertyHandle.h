#pragma once

#include "CoreMinimal.h"

#include "DObject/StrongObjectPtr.h"
#include "DObject/WeakObjectPtr.h"
#include "Math/MathFwd.h"
#include "PropertyEditor/PropertyEditing.h"
#include "Transactions/Transaction.h"

namespace Durin { class DEditorEngine; class FPropertyView; }

namespace Durin::PropertyEditor
{
	// Flags for one property write. The caller can own the surrounding gesture transaction.
	namespace EPropertyValueSetFlags
	{
		using Type = uint32;
		inline constexpr Type DefaultFlags = 0;
		inline constexpr Type NotTransactable = 1 << 0;
		inline constexpr Type InteractiveChange = 1 << 1;
	}

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
		DURINED_API auto SetValue(int32 Value, std::string* OutError = nullptr) -> bool;
		DURINED_API auto SetValue(const FLinearColor& Value, std::string* OutError = nullptr) -> bool;
		DURINED_API auto Commit(std::string* OutError = nullptr) -> bool;
		DURINED_API auto Cancel(std::string* OutError = nullptr) -> bool;
		// Close a rejected/no-change edit without discarding an enclosing recording.
		// Applied previews must use Cancel instead.
		DURINED_API auto CancelUnchanged(std::string* OutError = nullptr) -> bool;

		auto IsActive() const -> bool { return bActive; }
		DURINED_API auto MatchesTarget(const FPropertyEditTarget& Other) const -> bool;
		auto IsEditingObject(const DObject* Object) const -> bool { return bActive && Target.Object == Object; }
		auto HasChanges() const -> bool { return bActive && !(OriginalValue == CurrentValue); }
		auto GetDescription() const -> std::string_view { return Description; }
		auto GetOriginalValue() const -> const FPropertyValueSnapshotPayload& { return OriginalValue; }
		auto GetCurrentValue() const -> const FPropertyValueSnapshotPayload& { return CurrentValue; }

	private:
		auto BeginInternal(const FPropertyEditTarget& Target, std::string_view Description,
			DTransactor* Transactor, std::string* OutError, bool bOpenScope = true) -> bool;
		friend class ::Durin::DEditorEngine;
		friend class ::Durin::FPropertyView;
		friend class FReflectedPropertyHandle;
		friend class FPropertyControlFrame;
		auto ApplyInternal(const FPropertyValueSnapshotPayload& ProposedValue,
			std::string* OutError, bool* OutChanged, EPropertyChangePhase Phase = EPropertyChangePhase::Interactive) -> bool;
		auto Reset(bool bCancelRecording = true) -> void;

		auto WriteControlValue(const std::variant<int32, FLinearColor>& Value, std::string* OutError,
			bool& bOutChanged) -> bool;
		template<typename TValue>
		auto WriteValue(const TValue& Value, std::string* OutError, bool* OutChanged = nullptr,
			EPropertyChangePhase Phase = EPropertyChangePhase::Interactive) -> bool;
		FPropertyEditTarget Target;
		TStrongObjectPtr<DObject> TargetObject;
		FPropertyValueSnapshotPayload OriginalValue;
		FPropertyValueSnapshotPayload CurrentValue;
		std::string Description;
		std::optional<FScopedTransaction> TransactionScope;
		bool bActive = false;
	};

	class IPropertyHandle
	{
	public:
		virtual ~IPropertyHandle() = default;
		virtual auto GetValue(int32& OutValue) const -> bool = 0;
		virtual auto GetValue(FLinearColor& OutValue) const -> bool = 0;
		virtual auto IsEditable() const -> bool = 0;
		virtual auto SetValue(int32 Value, EPropertyValueSetFlags::Type Flags = EPropertyValueSetFlags::DefaultFlags) const -> bool = 0;
		virtual auto SetValue(const FLinearColor& Value, EPropertyValueSetFlags::Type Flags = EPropertyValueSetFlags::DefaultFlags) const -> bool = 0;
	};

	// Resolves a reflected member or nested path on the bound object generation.
	class FReflectedPropertyHandle final : public IPropertyHandle
	{
	public:
		DURINED_API FReflectedPropertyHandle(DObject& Object, FName Member, uint32 ArrayIndex = 0);
		DURINED_API explicit FReflectedPropertyHandle(FPropertyEditTarget Target);
		DURINED_API auto GetValue(int32& OutValue) const -> bool override;
		DURINED_API auto GetValue(FLinearColor& OutValue) const -> bool override;
		DURINED_API auto IsEditable() const -> bool override;
		DURINED_API auto SetValue(int32 Value, EPropertyValueSetFlags::Type Flags = EPropertyValueSetFlags::DefaultFlags) const -> bool override;
		DURINED_API auto SetValue(const FLinearColor& Value, EPropertyValueSetFlags::Type Flags = EPropertyValueSetFlags::DefaultFlags) const -> bool override;

	private:
		friend class FPropertyControlFrame;
		auto BeginEdit(FPropertyEdit& Edit, bool bShouldTransact, std::string* OutError = nullptr) const -> bool;
		auto ResolveTarget() const -> FPropertyEditTarget;
		auto ResolveMember(DObject*& OutOwner) const -> const FProperty*;
		template<typename TValue>
		auto ReadValue(TValue& OutValue) const -> bool;
		template<typename TValue>
		auto SetValueWithFlags(const TValue& Value, EPropertyValueSetFlags::Type Flags) const -> bool;

		TWeakObjectPtr<DObject> Object;
		FName Member;
		uint32 ArrayIndex = 0;
		std::optional<FPropertyEditTarget> PathTarget;
	};
}
