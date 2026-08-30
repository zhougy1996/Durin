#pragma once

#include "AssetRegistry/ObjectStream.h"
#include "EngineAPI.h"
#include "Asset/PackageSerialization.h"
#include "PackageVersionPolicy.h"

namespace Durin::Asset::PackageObjectStream
{
	struct FAssetPackageWriteOptions
	{
		EDefaultDeltaMode DeltaMode = EDefaultDeltaMode::Enabled;
		FAssetPackageSerializationOptions Serialization;
		bool bVerifyRepeatedEncoding = false;
	};

	// Production integration boundary shared by ordinary serialization, saves,
	// and explicit callers that need writer diagnostics or delta-mode control.
	ENGINE_API auto WriteAssetPackage(
		DPackage* Package,
		std::vector<std::byte>& OutBytes,
		const FAssetPackageWriteOptions& Options = {},
		FWriterDiagnostic* OutDiagnostic = nullptr) -> FAssetResult;

	ENGINE_API auto WriteRedirectorPackage(
		const FAssetPath& SourcePath,
		const FAssetPath& DestinationPath,
		std::vector<std::byte>& OutBytes) -> FAssetResult;
}
