#pragma once

#include "CoreMinimal.h"

#include "DerivedDataCache/DerivedDataCacheTypes.h"

namespace Durin::DerivedData
{
	struct FCacheStorageGetRequest
	{
		FCacheKey Key;
		uint64 MaximumValueBytes = 0;
	};

	using FCacheStorageGetResult =
		std::expected<std::optional<FSharedByteBuffer>, FCacheError>;

	struct FCacheStoragePutRequest
	{
		FCacheKey Key;
		FByteView Value;
		uint64 MaximumValueBytes = 0;
	};

	using FCacheStoragePutResult = std::expected<void, FCacheError>;

	// Private bounded byte persistence; the record codec owns content integrity.
	class FCacheStorage
	{
	public:
		DERIVEDDATACACHE_API auto Get(
			const FCacheStorageGetRequest& Request) const -> FCacheStorageGetResult;
		DERIVEDDATACACHE_API auto Put(
			const FCacheStoragePutRequest& Request) const -> FCacheStoragePutResult;
	};

	DERIVEDDATACACHE_API auto GetCacheStorage() -> FCacheStorage&;
}
