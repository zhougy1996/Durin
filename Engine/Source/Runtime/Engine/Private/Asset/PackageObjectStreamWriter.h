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
	};

	// Captures one live package into the format-neutral reflected logical model.
	// The caller owns package-format encoding and publication.
	ENGINE_API auto CaptureAssetPackage(
		DPackage* Package,
		FPackageInput& OutInput,
		const FAssetPackageWriteOptions& Options = {},
		FWriterDiagnostic* OutDiagnostic = nullptr) -> FAssetResult;

}
