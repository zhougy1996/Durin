#include "Asset/AssetWriteResult.h"
#include "Asset/AssetReadResult.h"

namespace Durin
{
	FAssetReadResult::operator FAssetWriteResult() const
	{
		switch (Error)
		{
		case EAssetReadError::None: return {EAssetWriteError::None, Message};
		case EAssetReadError::InvalidPath: return {EAssetWriteError::InvalidPath, Message};
		case EAssetReadError::AlreadyExists: return {EAssetWriteError::AlreadyExists, Message};
		case EAssetReadError::NotFound: return {EAssetWriteError::NotFound, Message};
		case EAssetReadError::IoError: return {EAssetWriteError::IoError, Message};
		case EAssetReadError::CorruptFile: return {EAssetWriteError::InvalidData, Message};
		case EAssetReadError::UnsupportedVersion: return {EAssetWriteError::UnsupportedVersion, Message};
		case EAssetReadError::UnknownClass: return {EAssetWriteError::InvalidData, Message};
		case EAssetReadError::TypeMismatch: return {EAssetWriteError::InvalidData, Message};
		case EAssetReadError::MissingDependency: return {EAssetWriteError::InvalidData, Message};
		case EAssetReadError::CircularDependency: return {EAssetWriteError::InvalidData, Message};
		case EAssetReadError::InvalidObjectGraph: return {EAssetWriteError::InvalidData, Message};
		case EAssetReadError::UnsupportedProperty: return {EAssetWriteError::InvalidData, Message};
		case EAssetReadError::InvalidPackageType: return {EAssetWriteError::InvalidData, Message};
		case EAssetReadError::InUse: return {EAssetWriteError::InUse, Message};
		case EAssetReadError::StaleData: return {EAssetWriteError::StaleData, Message};
		case EAssetReadError::ShuttingDown: return {EAssetWriteError::ShuttingDown, Message};
		case EAssetReadError::Cancelled: return {EAssetWriteError::Cancelled, Message};
		case EAssetReadError::ProjectionPending: return {EAssetWriteError::ProjectionPending, Message};
		}
		return {EAssetWriteError::InvalidData, Message};
	}

}
