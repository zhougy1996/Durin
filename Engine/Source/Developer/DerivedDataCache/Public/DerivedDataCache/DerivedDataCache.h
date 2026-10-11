#pragma once

#include "CoreMinimal.h"

#include "DerivedDataBuildOutput.h"

namespace Durin::DerivedData
{
	struct FCacheGetRequest
	{
		FCacheKey Key;
		FBuildOutputLimits OutputLimits;
		// Manifest plus each referenced blob, including repeated references.
		uint64 MaximumEncodedBytes = FCacheRecord::DefaultMaximumEncodedBytes;
		// Retained payload/codec storage; excludes descriptor and allocator overhead.
		uint64 MaximumWorkingSetBytes = 4ull * 1024 * 1024 * 1024;
	};

	using FCacheGetResult = std::expected<std::optional<FCacheRecord>, FCacheError>;

	struct FCachePutRequest
	{
		FCacheRecord Record;
		uint64 MaximumEncodedBytes = FCacheRecord::DefaultMaximumEncodedBytes;
		// Logical output value, metadata, and message bytes before compression.
		uint64 MaximumRawBytes = 2ull * 1024 * 1024 * 1024;
		uint64 MaximumWorkingSetBytes = 4ull * 1024 * 1024 * 1024;
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
