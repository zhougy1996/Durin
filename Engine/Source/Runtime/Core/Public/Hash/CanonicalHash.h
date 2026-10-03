#pragma once

#include "Misc/CoreStd.h"
#include "Misc/CoreTypes.h"

#include "Hash/XxHash.h"
#include "Serialization/BinaryEncoding.h"

namespace Durin
{
	// Callers choose field order and domain/version tags. No native object representation is hashed.
	template<typename TBuilder, CBinaryInteger T>
	auto UpdateCanonicalHash(TBuilder& Builder, T Value) -> void
	{
		Builder.Update(EncodeBinaryInteger(Value));
	}
	template<typename TBuilder, typename T> requires std::is_enum_v<T>
	auto UpdateCanonicalHash(TBuilder& Builder, T Value) -> void
	{
		UpdateCanonicalHash(Builder, static_cast<std::underlying_type_t<T>>(Value));
	}
	template<typename TBuilder, typename T> requires std::is_same_v<T, bool>
	auto UpdateCanonicalHash(TBuilder& Builder, T Value) -> void
	{
		UpdateCanonicalHash(Builder, static_cast<uint8>(Value));
	}
	template<typename TBuilder>
	auto UpdateCanonicalHash(TBuilder& Builder, FXxHash64 Value) -> void
	{
		UpdateCanonicalHash(Builder, Value.HashValue);
	}
	template<typename TBuilder>
	auto UpdateCanonicalHash(TBuilder& Builder, FXxHash128 Value) -> void
	{
		UpdateCanonicalHash(Builder, Value.HashLow);
		UpdateCanonicalHash(Builder, Value.HashHigh);
	}
	// uint64 byte length followed by exact bytes, including embedded NULs.
	template<typename TBuilder>
	auto UpdateCanonicalHashString(TBuilder& Builder, std::string_view Value) -> void
	{
		UpdateCanonicalHash(Builder, static_cast<uint64>(Value.size()));
		Builder.Update(Value);
	}
}
