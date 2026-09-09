#pragma once

#include "EngineAPI.h"

namespace Durin
{
	// Runtime light component identity, stable across scene removal and proxy recreation.
	// Zero is invalid; this value is neither a scene index nor a serialized identity.
	struct FLightComponentId
	{
		uint64 Value = 0;
		explicit constexpr FLightComponentId(uint64 InValue = 0) : Value(InValue) {}
		constexpr auto IsValid() const -> bool { return Value != 0; }
		auto operator<=>(const FLightComponentId&) const = default;
	};

	inline constexpr FLightComponentId InvalidLightComponentId;

	struct FLightComponentIdHash
	{
		auto operator()(FLightComponentId Id) const -> size_t
		{
			return std::hash<uint64>{}(Id.Value);
		}
	};
}
