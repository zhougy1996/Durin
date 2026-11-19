#pragma once

#include "CoreMinimal.h"

#include "DObject/WeakObjectPtr.h"
#include "Math/MathFwd.h"
#include "PropertyEditor/PropertyEditing.h"

namespace Durin::PropertyEditor
{
	class FPropertyEdit
	{
	public:
		DURINED_API auto SetValue(int32 Value, std::string* OutError = nullptr) -> bool;
		DURINED_API auto SetValue(const FLinearColor& Value, std::string* OutError = nullptr) -> bool;
		DURINED_API auto Commit(std::string* OutError = nullptr) -> bool;
		DURINED_API auto Cancel(std::string* OutError = nullptr) -> bool;
		auto IsActive() const -> bool { return Session.IsActive(); }
		auto IsEditingObject(const DObject* Object) const -> bool { return IsActive() && Target.Object == Object; }
		auto MatchesTarget(const FPropertyEditTarget& Other) const -> bool { return Session.MatchesTarget(Other); }

	private:
		friend class FReflectedPropertyHandle;
		friend class FPropertyControlFrame;
		auto WriteControlValue(const std::variant<int32, FLinearColor>& Value, std::string* OutError,
			bool& bOutChanged) -> bool;
		template<typename TValue>
		auto WriteValue(const TValue& Value, std::string* OutError, bool* OutChanged = nullptr) -> bool;
		FPropertyEditTarget Target;
		FPropertyEditSession Session;
	};

	class IPropertyHandle
	{
	public:
		virtual ~IPropertyHandle() = default;
		virtual auto GetValue(int32& OutValue) const -> bool = 0;
		virtual auto GetValue(FLinearColor& OutValue) const -> bool = 0;
		virtual auto IsEditable() const -> bool = 0;
		virtual auto SetValue(int32 Value, ETransactionMode Mode = ETransactionMode::Record) const -> bool = 0;
		virtual auto SetValue(const FLinearColor& Value, ETransactionMode Mode = ETransactionMode::Record) const -> bool = 0;
		virtual auto BeginEdit(FPropertyEdit& Edit, ETransactionMode Mode = ETransactionMode::Record,
			std::string* OutError = nullptr) const -> bool = 0;
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
		DURINED_API auto SetValue(int32 Value, ETransactionMode Mode = ETransactionMode::Record) const -> bool override;
		DURINED_API auto SetValue(const FLinearColor& Value, ETransactionMode Mode = ETransactionMode::Record) const -> bool override;
		DURINED_API auto BeginEdit(FPropertyEdit& Edit, ETransactionMode Mode = ETransactionMode::Record,
			std::string* OutError = nullptr) const -> bool override;

	private:
		auto ResolveMember(DObject*& OutOwner) const -> const FProperty*;
		template<typename TValue>
		auto ReadValue(TValue& OutValue) const -> bool;
		template<typename TValue>
		auto SetValueAndCommit(const TValue& Value, ETransactionMode Mode) const -> bool;

		TWeakObjectPtr<DObject> Object;
		FName Member;
		uint32 ArrayIndex = 0;
		std::optional<FPropertyEditTarget> PathTarget;
	};
}
