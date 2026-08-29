#pragma once

#include "EngineAPI.h"
#include "AssetRegistry/Result.h"
#include "DObject/AssetPath.h"
#include "DObject/Object.h"

namespace Durin::Asset
{
	ENGINE_API auto CreateAsset(
		const FAssetPath& Path,
		DClass* Class,
		size_t Size,
		DObject*& OutAsset
	) -> FAssetResult;
	ENGINE_API auto DuplicateAsset(
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

	ENGINE_API auto SavePackage(DPackage* Package) -> FAssetResult;
} // namespace Durin::Asset
