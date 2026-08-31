#pragma once

#include "Asset/Result.h"
#include "AssetRegistry/RegistryResult.h"

namespace Durin::Asset::Private
{
	inline auto ToAssetResult(FAssetRegistryResult Result) -> FAssetResult
	{
		EAssetError Error = EAssetError::None;
		switch (Result.Error)
		{
		case EAssetRegistryError::None: Error = EAssetError::None; break;
		case EAssetRegistryError::InvalidPath: Error = EAssetError::InvalidPath; break;
		case EAssetRegistryError::AlreadyExists: Error = EAssetError::AlreadyExists; break;
		case EAssetRegistryError::NotFound: Error = EAssetError::NotFound; break;
		case EAssetRegistryError::IoError: Error = EAssetError::IoError; break;
		case EAssetRegistryError::CorruptFile: Error = EAssetError::CorruptFile; break;
		case EAssetRegistryError::UnsupportedVersion: Error = EAssetError::UnsupportedVersion; break;
		case EAssetRegistryError::MissingDependency: Error = EAssetError::MissingDependency; break;
		case EAssetRegistryError::StaleData: Error = EAssetError::StaleData; break;
		}
		return {Error, std::move(Result.Message)};
	}
}
