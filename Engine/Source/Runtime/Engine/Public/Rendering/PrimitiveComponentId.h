#pragma once

#include "EngineAPI.h"

namespace Durin
{
	// Runtime component identity, stable across scene removal and proxy recreation.
	// Zero is invalid; this value is neither a scene index nor a serialized identity.
	struct FPrimitiveComponentId
	{
		uint64 Value = 0;
		explicit constexpr FPrimitiveComponentId(uint64 InValue = 0) : Value(InValue) {}
		constexpr auto IsValid() const -> bool { return Value != 0; }
		auto operator<=>(const FPrimitiveComponentId&) const = default;
	};

	inline constexpr FPrimitiveComponentId InvalidPrimitiveComponentId;

	struct FPrimitiveComponentIdHash
	{
		auto operator()(FPrimitiveComponentId Id) const -> size_t
		{
			return std::hash<uint64>{}(Id.Value);
		}
	};
}
