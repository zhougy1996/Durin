#pragma once

#include "Asset/PackageBulkData.h"

namespace Durin::Asset::Private
{
	inline constexpr uint32 PackageBulkDataDirectoryVersion = 2;
	inline constexpr uint32 PackageBulkDataDirectoryEntryBytes = 72;

	auto EncodePackageBulkDataDirectory(
		std::span<const FPackageBulkDataEntry> Entries,
		std::vector<std::byte>& OutBytes,
		std::string* OutError = nullptr) -> bool;

	auto DecodePackageBulkDataDirectory(
		std::span<const std::byte> Bytes,
		std::vector<FPackageBulkDataEntry>& OutEntries,
		std::string* OutError = nullptr) -> bool;
}
