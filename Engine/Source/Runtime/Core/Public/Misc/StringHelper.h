#pragma once

#include "CoreAPI.h"

namespace Durin::StringUtils
{
	// Lowercases one ASCII code unit and leaves every other byte unchanged.
	[[nodiscard]] constexpr auto ToLowerAscii(char Character) -> char
	{
		return Character >= 'A' && Character <= 'Z'
			? static_cast<char>(Character + ('a' - 'A')) : Character;
	}

	// Returns a locale-independent ASCII-lowercase copy of Text.
	[[nodiscard]] CORE_API auto FoldAscii(std::string_view Text) -> std::string;
	[[nodiscard]] CORE_API auto ContainsInsensitive(std::string_view Text, std::string_view Filter) -> bool;
	[[nodiscard]] CORE_API auto HumanizeName(std::string_view Name) -> std::string;
	// Formats a byte count with B or the largest applicable IEC binary unit.
	[[nodiscard]] CORE_API auto FormatByteSize(uint64 Bytes) -> std::string;
}
