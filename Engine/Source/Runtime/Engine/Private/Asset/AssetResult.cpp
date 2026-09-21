#include "Asset/AssetWriteResult.h"
#include "Asset/AssetReadResult.h"
#include "DObject/PackageFormat.h"

namespace Durin
{
	auto AssetWriteResultFromEncoding(const ObjectPackage::FPackageWriterResult& Result) -> FAssetWriteResult
	{
		if (Result) return {};
		return {Result.Reason == ObjectPackage::EPackageWriterReason::UnsupportedVersion
			? EAssetWriteError::UnsupportedVersion : EAssetWriteError::InvalidData,
			ObjectPackage::FormatPackageError(Result)};
	}

	auto AssetWriteResultFromRead(const FAssetReadResult& Result) -> FAssetWriteResult
	{
		switch (Result.Error)
		{
		case EAssetReadError::None: return {EAssetWriteError::None, Result.Message};
		case EAssetReadError::InvalidPath: return {EAssetWriteError::InvalidPath, Result.Message};
		case EAssetReadError::AlreadyExists: return {EAssetWriteError::AlreadyExists, Result.Message};
		case EAssetReadError::NotFound: return {EAssetWriteError::NotFound, Result.Message};
		case EAssetReadError::IoError: return {EAssetWriteError::IoError, Result.Message};
		case EAssetReadError::CorruptFile: return {EAssetWriteError::InvalidData, Result.Message};
		case EAssetReadError::UnsupportedVersion: return {EAssetWriteError::UnsupportedVersion, Result.Message};
		case EAssetReadError::UnknownClass: return {EAssetWriteError::InvalidData, Result.Message};
		case EAssetReadError::TypeMismatch: return {EAssetWriteError::InvalidData, Result.Message};
		case EAssetReadError::MissingDependency: return {EAssetWriteError::InvalidData, Result.Message};
		case EAssetReadError::CircularDependency: return {EAssetWriteError::InvalidData, Result.Message};
		case EAssetReadError::InvalidObjectGraph: return {EAssetWriteError::InvalidData, Result.Message};
		case EAssetReadError::UnsupportedProperty: return {EAssetWriteError::InvalidData, Result.Message};
		case EAssetReadError::InvalidPackageType: return {EAssetWriteError::InvalidData, Result.Message};
		case EAssetReadError::InUse: return {EAssetWriteError::InUse, Result.Message};
		case EAssetReadError::StaleData: return {EAssetWriteError::StaleData, Result.Message};
		case EAssetReadError::ShuttingDown: return {EAssetWriteError::ShuttingDown, Result.Message};
		case EAssetReadError::Cancelled: return {EAssetWriteError::Cancelled, Result.Message};
		case EAssetReadError::ProjectionPending: return {EAssetWriteError::ProjectionPending, Result.Message};
		}
		return {EAssetWriteError::InvalidData, Result.Message};
	}

}
