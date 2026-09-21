#pragma once

#include "Asset/AssetReadResult.h"
#include "AssetRegistry/RegistryResult.h"

namespace Durin::AssetPrivate
{
	inline auto ToAssetResult(FAssetRegistryResult Result) -> FAssetReadResult
	{
		EAssetReadError Error = EAssetReadError::None;
		switch (Result.Error)
		{
		case EAssetRegistryError::None: Error = EAssetReadError::None; break;
		case EAssetRegistryError::InvalidPath: Error = EAssetReadError::InvalidPath; break;
		case EAssetRegistryError::AlreadyExists: Error = EAssetReadError::AlreadyExists; break;
		case EAssetRegistryError::NotFound: Error = EAssetReadError::NotFound; break;
		case EAssetRegistryError::IoError: Error = EAssetReadError::IoError; break;
		case EAssetRegistryError::CorruptFile: Error = EAssetReadError::CorruptFile; break;
		case EAssetRegistryError::UnsupportedVersion: Error = EAssetReadError::UnsupportedVersion; break;
		case EAssetRegistryError::MissingDependency: Error = EAssetReadError::MissingDependency; break;
		case EAssetRegistryError::StaleData: Error = EAssetReadError::StaleData; break;
		}
		return {.Error = Error, .Message = FormatAssetRegistryError(Result)};
	}
}
