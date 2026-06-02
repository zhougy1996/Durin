#pragma once

#include "CoreAPI.h"
#include "Misc/CoreTypes.h"

namespace Durin
{
	namespace String
	{
		CORE_API auto WideToUtf8(std::wstring_view WideStr) -> std::string;

		CORE_API auto Utf8ToWide(std::string_view Utf8Str) -> std::wstring;

		CORE_API auto CodepointToUtf8(uint32 Codepoint) -> std::string;

		[[nodiscard]] CORE_API auto IsHex(std::string_view Value, std::optional<size_t> ExpectedLength = std::nullopt) -> bool;

		[[nodiscard]] CORE_API auto BytesToHex(std::span<const uint8> Bytes) -> std::string;

		[[nodiscard]] FORCEINLINE auto BytesToHex(std::span<const std::byte> Bytes) -> std::string
		{
			return BytesToHex(std::span<const uint8>(reinterpret_cast<const uint8*>(Bytes.data()), Bytes.size()));
		}

		CORE_API auto HexToBytes(std::string_view Hex, std::span<uint8> OutBytes) -> void;
	}
}
