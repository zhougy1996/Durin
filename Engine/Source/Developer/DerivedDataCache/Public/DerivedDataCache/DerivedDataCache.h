#pragma once

#include "DerivedDataBuildOutput.h"

namespace Durin::DerivedData
{
	struct FCacheGetRequest
	{
		FCacheKey Key;
		FBuildOutputLimits OutputLimits;
		uint64 MaximumEncodedBytes = FCacheRecord::DefaultMaximumEncodedBytes;
	};

	using FCacheGetResult = std::expected<std::optional<FCacheRecord>, FCacheError>;

	struct FCachePutRequest
	{
		FCacheRecord Record;
		uint64 MaximumEncodedBytes = FCacheRecord::DefaultMaximumEncodedBytes;
	};

	using FCachePutResult = std::expected<void, FCacheError>;

	class ICache
	{
	public:
		virtual ~ICache() = default;
		virtual auto Get(const FCacheGetRequest& Request) const -> FCacheGetResult = 0;
		virtual auto Put(const FCachePutRequest& Request) const -> FCachePutResult = 0;
	};

	// Returns the process-owned structured record cache. Serialization,
	// compression, validation, and physical storage remain behind this boundary.
	DERIVEDDATACACHE_API auto GetCache() -> ICache&;
}
