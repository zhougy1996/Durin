#pragma once

#include "AssetRegistry/ObjectStream.h"
#include "DObject/CanonicalMapKey.h"
#include "DObject/PackageLinker.h"

namespace Durin::Asset::Private
{
	enum class EV7LinkerAdapterFailure : uint8
	{
		None,
		InvalidTable,
		InvalidTopology,
		InvalidValue,
		UnsupportedRetainedValue,
		UnsupportedCustomPayload,
		LimitExceeded,
	};

	struct FV7LinkerAdapterDiagnostic
	{
		EV7LinkerAdapterFailure Failure = EV7LinkerAdapterFailure::None;
		std::string LogicalPath;
		std::string Message;

		auto Reset() -> void { *this = {}; }
	};

	// Translates one fully decoded DAST v7 package without constructing DObjects.
	ASSETREGISTRY_API auto AdaptDecodedPackageV7(
		const PackageObjectStream::FDecodedPackage& Package,
		std::string_view PackageName,
		ObjectPackage::FLinkerTables& OutLinker,
		FV7LinkerAdapterDiagnostic* OutDiagnostic = nullptr) -> bool;

	// Projects a validated v7 key through the CoreDObject canonical token writer.
	ASSETREGISTRY_API auto BuildDecodedCanonicalMapKeyTokenV7(
		const PackageObjectStream::FDecodedPackage& Package,
		const PackageObjectStream::FDecodedType& Type,
		const PackageObjectStream::FValue& Value,
		std::vector<std::byte>& OutToken,
		FV7LinkerAdapterDiagnostic* OutDiagnostic = nullptr) -> bool;
}
