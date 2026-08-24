#pragma once

#include "AssetCoreAPI.h"
#include "Hash/XxHash.h"
#include "Misc/Guid.h"

namespace Durin::Asset::PackageTrailer
{
	inline constexpr uint32 TrailerMagic = 0x4c525444; // DTRL
	inline constexpr uint32 FooterMagic = 0x46525444; // DTRF
	inline constexpr uint32 TrailerVersion = 1;
	inline constexpr uint32 FooterVersion = 1;
	inline constexpr uint32 TrailerHeaderBytes = 64;
	inline constexpr uint32 TrailerEntryBytes = 80;
	inline constexpr uint32 FooterBytes = 64;
	inline constexpr uint64 MaximumEntryCount = 65'536;
	inline constexpr uint64 MaximumObjectStreamBytes = 256ull * 1024ull * 1024ull;
	inline constexpr uint64 MaximumPackageBytes = 1024ull * 1024ull * 1024ull;

	enum class EPlacement : uint32
	{
		ExternalDabkV1 = 1,
	};

	struct FEntry
	{
		FGuid PayloadId;
		EPlacement Placement = EPlacement::ExternalDabkV1;
		uint64 LogicalByteCount = 0;
		uint64 StoredByteCount = 0;
		FXxHash128 ContentHash;
		FXxHash128 ContainerHash;

		auto operator==(const FEntry&) const -> bool = default;
	};

	struct FInspection
	{
		uint64 ObjectStreamEnd = 0;
		uint64 TrailerOffset = 0;
		uint64 TrailerSize = 0;
		FXxHash128 DirectoryHash;
		FXxHash128 TrailerHash;
		std::vector<FEntry> Entries;

		auto operator==(const FInspection&) const -> bool = default;
	};

	// Builds detached TrailerV1 || FooterV1 bytes. The caller still owns the
	// opaque object-stream prefix and any eventual file publication.
	ASSETCORE_API auto Build(
		std::span<const FEntry> Entries,
		uint64 ObjectStreamEnd,
		std::vector<std::byte>& OutBytes,
		std::string* OutError = nullptr) -> bool;

	// Discovers the footer at physical EOF and validates the complete trailer
	// without interpreting or constructing the opaque object-stream prefix.
	ASSETCORE_API auto Inspect(
		std::span<const std::byte> PackageBytes,
		FInspection& OutInspection,
		std::string* OutError = nullptr) -> bool;
}
