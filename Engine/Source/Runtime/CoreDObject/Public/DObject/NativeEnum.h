#pragma once

#include "CoreMinimal.h"
#include "DObject/DObjectGlobals.h"
#include <typeinfo>

namespace Durin
{
	namespace Private
	{
		COREDOBJECT_API auto ResolveNativeEnum(const std::type_info& Type) -> DEnum*;
	}

	// Generated registrations resolve complete enum types without forward-declaring
	// unscoped enums or assuming that the hermetic parser owns their native ABI.
	template<typename T> auto StaticEnum() -> DEnum*
	{
		static_assert(std::is_enum_v<T>, "StaticEnum<T> requires an enum type.");
		return Private::ResolveNativeEnum(typeid(T));
	}

	namespace DurinCodeGen
	{
		class FNativeEnumRegistration final
		{
		public:
			COREDOBJECT_API FNativeEnumRegistration(const std::type_info& Type, DEnum* (*Resolver)());
			COREDOBJECT_API ~FNativeEnumRegistration();
			FNativeEnumRegistration(const FNativeEnumRegistration&) = delete;
			auto operator=(const FNativeEnumRegistration&) -> FNativeEnumRegistration& = delete;
		private:
			const std::type_info& Type;
		};

		template<typename T> constexpr auto NativeEnumUnderlyingType() -> EEnumUnderlyingType
		{
			using U = std::underlying_type_t<T>;
			static_assert(std::is_integral_v<U> && !std::is_same_v<U, bool>);
			if constexpr (std::is_signed_v<U>)
			{
				if constexpr (sizeof(U) == 1) return EEnumUnderlyingType::Int8;
				else if constexpr (sizeof(U) == 2) return EEnumUnderlyingType::Int16;
				else if constexpr (sizeof(U) == 4) return EEnumUnderlyingType::Int32;
				else if constexpr (sizeof(U) == 8) return EEnumUnderlyingType::Int64;
			}
			else
			{
				if constexpr (sizeof(U) == 1) return EEnumUnderlyingType::UInt8;
				else if constexpr (sizeof(U) == 2) return EEnumUnderlyingType::UInt16;
				else if constexpr (sizeof(U) == 4) return EEnumUnderlyingType::UInt32;
				else if constexpr (sizeof(U) == 8) return EEnumUnderlyingType::UInt64;
			}
			return EEnumUnderlyingType::Unknown;
		}
	}
}
