#pragma once

#include "CoreMinimal.h"

#include "DObject/StrongObjectPtr.h"
#include "DObject/WeakObjectPtr.h"
#include "Math/Color.h"
#include "Math/Vector.h"
#include "PropertyEditor/PropertyEditing.h"
#include "Transactions/Transaction.h"

namespace Durin { class FPropertyView; }

namespace Durin::PropertyEditor
{
	namespace FPropertyAccess
	{
		enum Result { MultipleValues, Fail, Success };
	}

	enum class EPropertyWriteFailure { Access, Value };

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
		virtual auto SetValueFromSnapshot(const FPropertyValueSnapshot& Value, EPropertyValueSetFlags::Type Flags = EPropertyValueSetFlags::DefaultFlags)
			-> std::expected<bool, EPropertyWriteFailure> = 0;
		virtual auto GetLastError() const -> const std::string& = 0;
		template<typename T> requires requires { T::StaticStruct(); }
		auto SetValue(const T& Value, EPropertyValueSetFlags::Type Flags = EPropertyValueSetFlags::DefaultFlags) -> FPropertyAccess::Result
		{
			return SetStructValue(T::StaticStruct(), &Value, Flags);
		}

	private:
		friend class FPropertyControl;
		friend class ::Durin::FPropertyView;
		virtual auto FinishInteractiveChange() -> void = 0;
		virtual auto ResolveTarget() const -> FPropertyEditTarget = 0;
		virtual auto SetStructValue(const DStruct* Type, const void* Value, EPropertyValueSetFlags::Type Flags) -> FPropertyAccess::Result = 0;
	};

	DURINED_API auto MakePropertyHandle(FPropertyEditTarget Target) -> std::shared_ptr<IPropertyHandle>;
	DURINED_API auto MakePropertyHandle(DObject& Object, FName Member, uint32 ArrayIndex = 0) -> std::shared_ptr<IPropertyHandle>;

}
