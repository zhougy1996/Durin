#include "Asset/AssetWriteResult.h"
#include "DObject/PackageFormat.h"

namespace Durin
{
	auto AssetWriteResultFromRead(const UnifiedError::FError& Error) -> FAssetWriteResult
	{
		auto Code = EAssetWriteError::InvalidData;
		if (Error.Is(PackageReadErrors::IoError) || Error.Is({"AssetRegistry", "IoError"})) Code = EAssetWriteError::IoError;
		else if (Error.Is(PackageReadErrors::UnsupportedVersion) || Error.Is({"AssetRegistry", "UnsupportedVersion"})) Code = EAssetWriteError::UnsupportedVersion;
		else if (Error.Is(AssetLoadErrors::InvalidPath) || Error.Is({"AssetRegistry", "InvalidPath"})) Code = EAssetWriteError::InvalidPath;
		else if (Error.Is(AssetLoadErrors::NotFound) || Error.Is({"AssetRegistry", "NotFound"})) Code = EAssetWriteError::NotFound;
		else if (Error.Is(AssetLoadErrors::AlreadyExists) || Error.Is({"AssetRegistry", "AlreadyExists"})) Code = EAssetWriteError::AlreadyExists;
		else if (Error.Is(AssetLoadErrors::InUse) || Error.Is(AssetLoadErrors::NotInitialized)) Code = EAssetWriteError::InUse;
		else if (Error.Is(AssetLoadErrors::StaleData) || Error.Is({"AssetRegistry", "StaleData"})) Code = EAssetWriteError::StaleData;
		else if (Error.Is(AssetLoadErrors::Cancelled)) Code = EAssetWriteError::Cancelled;
		else if (Error.Is(AssetLoadErrors::ShuttingDown)) Code = EAssetWriteError::ShuttingDown;
		else if (Error.Is(AssetLoadErrors::ProjectionPending)) Code = EAssetWriteError::ProjectionPending;
		return {.Error = Code, .Message = Error.ToString(), .Cause = Error};
	}

	auto AssetWriteResultFromEncoding(const ObjectPackage::FPackageWriterResult& Result) -> FAssetWriteResult
	{
		if (Result) return {};
		return {Result.Reason == ObjectPackage::EPackageWriterReason::UnsupportedVersion
			? EAssetWriteError::UnsupportedVersion : EAssetWriteError::InvalidData,
			ObjectPackage::FormatPackageError(Result), EAssetWriteEffect::None, Result.Cause};
	}

}
