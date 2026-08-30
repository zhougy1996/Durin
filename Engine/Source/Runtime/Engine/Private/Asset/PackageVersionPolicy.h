#pragma once

#include "EngineAPI.h"
#include "AssetRegistry/PackageFormat.h"

namespace Durin::Asset
{
	// Permanent wire identities for the authored- and cooked-bulk DURF branches.
	// These random GUIDs are checked in once and are never derived from the
	// diagnostic names or the historical four-byte magics.
	inline constexpr FGuid DabkBinaryFormatId{
		0x49efbbb4, 0xe2434e35, 0xa7c01c34, 0x9ed84ea0};
	inline constexpr std::string_view DabkBinaryFormatName = "Durin.BinaryFormat.DABK";
	inline constexpr FGuid DblkBinaryFormatId{
		0x76c5d46c, 0xa3744b7e, 0x9cda6c8f, 0xe0dbcd17};
	inline constexpr std::string_view DblkBinaryFormatName = "Durin.BinaryFormat.DBLK";
	inline constexpr uint32 OrdinaryAssetPackageWriterVersion = AssetPackageV7FormatVersion;

	ENGINE_API auto ValidateAssetPackageVersionPolicy(std::string& OutError) -> bool;
	ENGINE_API auto GetAssetPackageReaderPolicyIdentity() -> uint32;
}
