#pragma once

#include "CoreMinimal.h"

#include "DerivedDataBuildOutput.h"

namespace Durin::DerivedData
{
	enum class ECacheGetMode : uint8 { FullRecord, MetadataOnly, SelectedValues };
	struct FCacheQueryStats
	{
		// Successful physical reads, validated raw bytes, and peak accounted payload/
		// codec storage (including estimated Zstd context), excluding container overhead.
		uint64 ReadBytes = 0;
		uint64 DecodedBytes = 0;
		uint64 PeakWorkingSetBytes = 0;
		uint64 DurationNanoseconds = 0;
		uint32 ValuesRead = 0;
	};
	struct FCacheGetRequest
	{
		FCacheKey Key;
		FBuildOutputLimits OutputLimits;
		// Manifest plus each requested blob, including repeated references.
		uint64 MaximumEncodedBytes = FCacheRecord::DefaultMaximumEncodedBytes;
		// Retained payload/codec storage; excludes descriptor and allocator overhead.
		uint64 MaximumWorkingSetBytes = 4ull * 1024 * 1024 * 1024;
		ECacheGetMode Mode = ECacheGetMode::FullRecord;
		// SelectedValues accepts unique IDs in any order. Empty means descriptors only.
		// Other modes require an empty list. All selected IDs are required.
		std::span<const FValueId> RequiredValues;
		std::function<bool()> ShouldCancel;
		// Borrowed for this synchronous call; records actual completed I/O and codec work.
		FCacheQueryStats* Stats = nullptr;
	};

	using FCacheGetResult = std::expected<std::optional<FCacheRecord>, FCacheError>;

	struct FCachePutRequest
	{
		FCacheRecord Record;
		uint64 MaximumEncodedBytes = FCacheRecord::DefaultMaximumEncodedBytes;
		// Logical output value, metadata, and message bytes before compression.
		uint64 MaximumRawBytes = 2ull * 1024 * 1024 * 1024;
		uint64 MaximumWorkingSetBytes = 4ull * 1024 * 1024 * 1024;
		std::function<bool()> ShouldCancel;
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
