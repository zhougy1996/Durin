#pragma once

#include "AssetRegistryAPI.h"
#include "AssetRegistry/ObjectStream.h"
#include "DObject/PackageLinker.h"

namespace Durin::Asset
{
	enum class ELegacyPackageConversionFailure : uint8
	{
		None,
		InvalidIdentity,
		InvalidV7Package,
		UnsupportedV7Value,
		InvalidBulkData,
		V8WriteFailure,
		V8VerificationFailure,
	};

	struct FLegacyPackageConversionDiagnostic
	{
		ELegacyPackageConversionFailure Failure =
			ELegacyPackageConversionFailure::None;
		std::string LogicalPath;
		std::string Message;

		auto Reset() -> void { *this = {}; }
	};

	// Temporary offline migration capability. It never constructs objects and
	// replaces outputs only after complete v7 decode and canonical v8 verification.
	ASSETREGISTRY_API auto ConvertDastV7PackageToV8(
		std::span<const std::byte> V7PackageBytes,
		std::span<const std::byte> V7BulkBytes,
		std::string_view PackageName,
		std::vector<std::byte>& OutV8PackageBytes,
		std::vector<std::byte>& OutV8BulkBytes,
		FLegacyPackageConversionDiagnostic* OutDiagnostic = nullptr) -> bool;

	// Temporary bridge for the P5 Engine capture adapter. Resolves legacy
	// BulkData descriptors already detached into linker values; no wire output or
	// object construction occurs.
	ASSETREGISTRY_API auto ResolveDastV7LinkerBulkDataForV8(
		ObjectPackage::FLinkerTables& Linker,
		std::span<const std::byte> V7BulkBytes,
		FLegacyPackageConversionDiagnostic* OutDiagnostic = nullptr) -> bool;

	ASSETREGISTRY_API auto AdaptDecodedDastV7ToLinker(
		const PackageObjectStream::FDecodedPackage& Package,
		std::string_view PackageName,
		ObjectPackage::FLinkerTables& OutLinker,
		FLegacyPackageConversionDiagnostic* OutDiagnostic = nullptr) -> bool;
}
