#pragma once

#include <utility>

// Defines all bitwise operators for enum classes so it can be (mostly) used as a regular flags enum
#define ENUM_CLASS_FLAGS(Enum) \
	inline Enum& operator|=(Enum& Lhs, Enum Rhs) \
	{ \
		return Lhs = static_cast<Enum>(std::to_underlying(Lhs) | std::to_underlying(Rhs)); \
	} \
	inline Enum& operator&=(Enum& Lhs, Enum Rhs) { return Lhs = static_cast<Enum>(std::to_underlying(Lhs) & std::to_underlying(Rhs)); } \
	inline Enum& operator^=(Enum& Lhs, Enum Rhs) { return Lhs = static_cast<Enum>(std::to_underlying(Lhs) ^ std::to_underlying(Rhs)); } \
	inline constexpr Enum operator|(Enum Lhs, Enum Rhs) { return static_cast<Enum>(std::to_underlying(Lhs) | std::to_underlying(Rhs)); } \
	inline constexpr Enum operator&(Enum Lhs, Enum Rhs) { return static_cast<Enum>(std::to_underlying(Lhs) & std::to_underlying(Rhs)); } \
	inline constexpr Enum operator^(Enum Lhs, Enum Rhs) { return static_cast<Enum>(std::to_underlying(Lhs) ^ std::to_underlying(Rhs)); } \
	inline constexpr bool operator!(Enum E) { return !std::to_underlying(E); } \
	inline constexpr Enum operator~(Enum E) { return static_cast<Enum>(~std::to_underlying(E)); }

// Friends all bitwise operators for enum classes so the definition can be kept private / protected.
#define FRIEND_ENUM_CLASS_FLAGS(Enum) \
	friend Enum& operator|=(Enum& Lhs, Enum Rhs); \
	friend Enum& operator&=(Enum& Lhs, Enum Rhs); \
	friend Enum& operator^=(Enum& Lhs, Enum Rhs); \
	friend constexpr Enum operator|(Enum Lhs, Enum Rhs); \
	friend constexpr Enum operator&(Enum Lhs, Enum Rhs); \
	friend constexpr Enum operator^(Enum Lhs, Enum Rhs); \
	friend constexpr bool operator!(Enum E); \
	friend constexpr Enum operator~(Enum E);

namespace Durin
{
	template<typename Enum>
	constexpr bool EnumHasAllFlags(Enum Flags, Enum Contains)
	{
		return (std::to_underlying(Flags) & std::to_underlying(Contains)) == std::to_underlying(Contains);
	}

	template<typename Enum>
	constexpr bool EnumHasAnyFlags(Enum Flags, Enum Contains)
	{
		return (std::to_underlying(Flags) & std::to_underlying(Contains)) != 0;
	}

	template<typename Enum>
	void EnumAddFlags(Enum& Flags, Enum FlagsToAdd)
	{
		Flags = static_cast<Enum>(std::to_underlying(Flags) | std::to_underlying(FlagsToAdd));
	}

	template<typename Enum>
	void EnumRemoveFlags(Enum& Flags, Enum FlagsToRemove)
	{
		Flags = static_cast<Enum>(std::to_underlying(Flags) & ~std::to_underlying(FlagsToRemove));
	}
} // namespace Durin