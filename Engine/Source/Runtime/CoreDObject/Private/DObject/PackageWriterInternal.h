#pragma once

#include "DObject/PackageFormat.h"

namespace Durin::ObjectPackage
{
	// Canonical verification preserves the input section version. Public saves use the current version.
	COREDOBJECT_API auto WritePackageForRegistryVersion(const FLinkerTables& Linker,
		uint32 RegistryVersion, FByteBuffer& OutPackageBytes, FByteBuffer& OutBulkBytes)
		-> FPackageWriterResult;
	auto WritePackageMainForRegistryVersion(const FLinkerTables& Linker,
		uint32 RegistryVersion, uint64 ExternalBulkBytes, FXxHash128 ExternalBulkHash,
		FByteBuffer& OutPackageBytes) -> FPackageWriterResult;
}
