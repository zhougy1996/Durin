#pragma once

#include "DerivedDataCache/DerivedDataCache.h"

namespace Durin::DerivedData
{
	class FFileSystemCacheBackend
	{
	public:
		auto Get(const FCacheGetRequest& Request) const -> FCacheGetResult;
		auto Put(const FCachePutRequest& Request) const -> FCachePutResult;

	private:
		auto GetBucketDirectory(const FCacheBucket& Bucket) const -> std::filesystem::path;
		auto GetEntryPath(const FCacheBucket& Bucket, const FCacheKey& Key,
			std::filesystem::path& OutPath, std::string& OutError) const -> bool;
	};
}
