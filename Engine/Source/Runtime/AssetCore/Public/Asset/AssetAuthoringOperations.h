#pragma once

#include "AssetCoreAPI.h"
#include "Asset/Result.h"
#include "DObject/CoreDObject.h"

namespace Durin::Asset
{
	ASSETCORE_API auto CreateAsset(
		const FAssetPath& Path,
		DClass* Class,
		size_t Size,
		DObject*& OutAsset
	) -> FAssetResult;
	ASSETCORE_API auto DuplicateAsset(
		const FAssetPath& SourcePath,
		const FAssetPath& DestinationPath,
		DObject*& OutAsset
	) -> FAssetResult;

	template<typename T>
	auto CreateAsset(const FAssetPath& Path, T*& OutAsset) -> FAssetResult
	{
		static_assert(std::is_base_of_v<DObject, T>);
		DObject* Object = nullptr;
		FAssetResult Result = CreateAsset(
			Path, T::StaticClass(), sizeof(T), Object
		);
		OutAsset = Result ? Cast<T>(Object) : nullptr;
		return Result;
	}

	ASSETCORE_API auto SavePackage(DPackage* Package) -> FAssetResult;
} // namespace Durin::Asset
