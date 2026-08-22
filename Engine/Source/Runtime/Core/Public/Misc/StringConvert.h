#pragma once

#include "CoreAPI.h"
#include "Misc/CoreTypes.h"

namespace Durin::StringUtils
{
	CORE_API auto WideToUtf8(std::wstring_view WideStr) -> std::string;

	CORE_API auto Utf8ToWide(std::string_view Utf8Str) -> std::wstring;

	CORE_API auto CodepointToUtf8(uint32 Codepoint) -> std::string;

	[[nodiscard]] CORE_API auto IsHex(std::string_view Value, std::optional<size_t> ExpectedLength = std::nullopt) -> bool;

	[[nodiscard]] CORE_API auto BytesToHex(std::span<const std::byte> Bytes) -> std::string;

	[[nodiscard]] CORE_API auto SanitizeFileName(std::string_view Value, std::string_view Fallback = "File") -> std::string;

	CORE_API auto HexToBytes(std::string_view Hex, std::span<std::byte> OutBytes) -> void;
}
