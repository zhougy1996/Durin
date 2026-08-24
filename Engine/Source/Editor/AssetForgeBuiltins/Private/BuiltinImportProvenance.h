#pragma once

#include "AssetForge/Persistence/ImportProvenance.h"

namespace Durin::AssetForge::Builtins
{
	namespace
	{
		auto DecodeStoredImportProvenance(std::string_view Hex,
			FImportProvenance& OutProvenance, std::string& OutError) -> bool
		{
			if (Hex.empty() || (Hex.size() & 1) != 0)
			{
				OutError = "AssetForge provenance encoding is malformed.";
				return false;
			}
			auto DecodeNibble = [](char Value) -> int32 {
				if (Value >= '0' && Value <= '9') return Value - '0';
				if (Value >= 'a' && Value <= 'f') return Value - 'a' + 10;
				return -1;
			};
			std::vector<std::byte> Bytes(Hex.size() / 2);
			for (size_t Index = 0; Index < Bytes.size(); ++Index)
			{
				const int32 High = DecodeNibble(Hex[Index * 2]);
				const int32 Low = DecodeNibble(Hex[Index * 2 + 1]);
				if (High < 0 || Low < 0)
				{
					OutError = "AssetForge provenance encoding is malformed.";
					return false;
				}
				Bytes[Index] = static_cast<std::byte>((High << 4) | Low);
			}
			return DeserializeImportProvenance(Bytes, OutProvenance, OutError);
		}
	}
}
