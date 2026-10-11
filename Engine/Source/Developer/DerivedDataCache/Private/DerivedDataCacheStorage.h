#pragma once

#include "CoreMinimal.h"

#include "DerivedDataCache/DerivedDataCacheTypes.h"

namespace Durin::DerivedData
{
	enum class ECacheStorageNamespace : uint8 { Legacy, Records, Values };
	struct FCacheStorageGetRequest
	{
		FCacheKey Key;
		uint64 MaximumValueBytes = 0;
		ECacheStorageNamespace Namespace = ECacheStorageNamespace::Legacy;
	};

	using FCacheStorageGetResult =
		std::expected<std::optional<FSharedByteBuffer>, FCacheError>;

	struct FCacheStoragePutRequest
	{
		FCacheKey Key;
		FByteView Value;
		uint64 MaximumValueBytes = 0;
		ECacheStorageNamespace Namespace = ECacheStorageNamespace::Legacy;
	};

	using FCacheStoragePutResult = std::expected<void, FCacheError>;

	// Private bounded byte persistence; manifest and blob codecs own integrity.
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
