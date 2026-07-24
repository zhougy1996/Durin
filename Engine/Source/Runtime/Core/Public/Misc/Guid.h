#pragma once

#include "CoreAPI.h"

namespace Durin
{
	// Stores a stable 128-bit identifier; the all-zero value is invalid.
	struct FGuid
	{
		uint32 A = 0;
		uint32 B = 0;
		uint32 C = 0;
		uint32 D = 0;

		constexpr FGuid() = default;

		constexpr FGuid(uint32 InA, uint32 InB, uint32 InC, uint32 InD)
			: A(InA)
			, B(InB)
			, C(InC)
			, D(InD)
		{
		}

		[[nodiscard]] constexpr auto IsValid() const -> bool { return (A | B | C | D) != 0; }

		auto Invalidate() -> void { A = B = C = D = 0; }

		[[nodiscard]] CORE_API auto ToString() const -> std::string;

		[[nodiscard]] CORE_API static auto Parse(std::string_view Text, FGuid& OutGuid) -> bool;

		[[nodiscard]] CORE_API static auto NewGuid() -> FGuid;

		auto operator<=>(const FGuid&) const = default;
	};

	static_assert(sizeof(FGuid) == 16);
	static_assert(std::is_trivially_copyable_v<FGuid>);

	[[nodiscard]] constexpr auto GetTypeHash(const FGuid& Guid) -> uint64
	{
		uint64 Hash = (static_cast<uint64>(Guid.A) << 32) | Guid.B;
		const uint64 Tail = (static_cast<uint64>(Guid.C) << 32) | Guid.D;
		Hash ^= Tail + 0x9e3779b97f4a7c15ull + (Hash << 6) + (Hash >> 2);
		Hash ^= Hash >> 33;
		Hash *= 0xff51afd7ed558ccdull;
		Hash ^= Hash >> 33;
		return Hash;
	}
}

template<>
struct std::hash<Durin::FGuid>
{
	auto operator()(const Durin::FGuid& Guid) const noexcept -> size_t
	{
		return static_cast<size_t>(Durin::GetTypeHash(Guid));
	}
};
