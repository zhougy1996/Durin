#pragma once

#include "CoreMinimal.h"

#include "DerivedDataCacheStorage.h"
#include "Misc/FilePath.h"

namespace Durin::DerivedData
{
	class FFileSystemCacheBackend
	{
	public:
		explicit FFileSystemCacheBackend(const Private::FCacheStorageLease& Lease) : Lease(Lease) {}
		auto Exists(const FCacheKey& Key, ECacheStorageNamespace Namespace) const -> std::expected<bool, FCacheError>;
		auto Get(const FCacheStorageGetRequest& Request) const -> FCacheStorageGetResult;
		auto Put(const FCacheStoragePutRequest& Request) const -> FCacheStoragePutResult;

	private:
		const Private::FCacheStorageLease& Lease;
		auto FindEntry(const FCacheKey& Key, ECacheStorageNamespace Namespace) const
			-> std::expected<std::optional<FFilePath>, FCacheError>;
		auto GetBucketDirectory(const FCacheBucket& Bucket, ECacheStorageNamespace Namespace) const -> FFilePath;
		auto GetEntryPath(const FCacheKey& Key, ECacheStorageNamespace Namespace) const
			-> std::expected<FFilePath, FCacheError>;
	};
}
