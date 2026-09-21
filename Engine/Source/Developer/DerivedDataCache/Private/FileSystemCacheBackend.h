#pragma once

#include "DerivedDataCache/DerivedDataCache.h"
#include "Misc/FilePath.h"

namespace Durin::DerivedData
{
	class FFileSystemCacheBackend
	{
	public:
		auto Get(const FCacheGetRequest& Request) const -> FCacheGetResult;
		auto Put(const FCachePutRequest& Request) const -> FCachePutResult;

	private:
		auto GetBucketDirectory(const FCacheBucket& Bucket) const -> FFilePath;
		auto GetEntryPath(const FCacheKey& Key,
			FFilePath& OutPath, std::string& OutError) const -> bool;
	};
}
