#pragma once

#include "EngineAPI.h"
#include "Asset/AssetDefinitions.h"
#include "DObject/AssetPath.h"
#include "DObject/Object.h"

namespace Durin
{
	ENGINE_API auto CreateAsset(
		const FTopLevelAssetPath& Path,
		DClass* Class,
		size_t Size,
		DObject*& OutAsset
	) -> FAssetResult;
	ENGINE_API auto DuplicateAsset(
		const FTopLevelAssetPath& SourcePath,
		const FTopLevelAssetPath& DestinationPath,
		DObject*& OutAsset
	) -> FAssetResult;

	template<typename T>
	auto CreateAsset(const FTopLevelAssetPath& Path, T*& OutAsset) -> FAssetResult
	{
		static_assert(std::is_base_of_v<DObject, T>);
		DObject* Object = nullptr;
		FAssetResult Result = CreateAsset(
			Path, T::StaticClass(), sizeof(T), Object
		);
		OutAsset = Result ? Cast<T>(Object) : nullptr;
		return Result;
	}

	// Fixtures that intentionally use package-leaf naming opt into it explicitly.
	template<typename T>
	auto CreatePackageLeafAssetForTesting(
		const FPackagePath& PackagePath, T*& OutAsset) -> FAssetResult
	{
		FTopLevelAssetPath AssetPath;
		if (!FTopLevelAssetPath::TryCreate(
			PackagePath, PackagePath.GetPackageName(), AssetPath))
			return {EAssetError::InvalidPath,
				"The package-leaf test asset path is invalid."};
		return CreateAsset(AssetPath, OutAsset);
	}

	ENGINE_API auto SavePackage(DPackage* Package) -> FAssetResult;
} // namespace Durin
