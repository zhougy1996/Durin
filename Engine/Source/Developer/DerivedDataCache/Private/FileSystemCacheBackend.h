#pragma once

#include "CoreMinimal.h"

#include "DerivedDataCacheStorage.h"
#include "Misc/FilePath.h"

namespace Durin::DerivedData
{
	class FFileSystemCacheBackend
	{
	public:
		auto Get(const FCacheStorageGetRequest& Request) const -> FCacheStorageGetResult;
		auto Put(const FCacheStoragePutRequest& Request) const -> FCacheStoragePutResult;

	private:
		auto GetBucketDirectory(const FCacheBucket& Bucket) const -> FFilePath;
		auto GetEntryPath(const FCacheKey& Key) const
			-> std::expected<FFilePath, FCacheError>;
	};
}
