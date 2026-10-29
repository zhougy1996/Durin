#pragma once

#include "CoreMinimal.h"

#include "DerivedDataCache/DerivedDataCache.h"

namespace Durin::DerivedData
{
	struct FCacheQueryStats
	{
		// Successful physical reads, validated raw bytes, and peak accounted payload/
		// codec storage (including estimated Zstd context), excluding container overhead.
		uint64 ReadBytes = 0;
		uint64 DecodedBytes = 0;
		uint64 PeakWorkingSetBytes = 0;
		uint64 DurationNanoseconds = 0;
		// Distinct requested blobs resolved; coalesced reads can charge zero ReadBytes.
		uint32 ValuesRead = 0;
	};
	struct FCacheGetBackendRequest
	{
		FCacheKey Key;
		FBuildOutputLimits OutputLimits;
		// Manifest plus each distinct requested blob identity and raw size.
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

	struct FCacheStoreStats
	{
		// Successful physical I/O and per-identity codec calls. Peak covers retained
		// inputs plus blob verification/encoding after manifest construction; it
		// excludes transient CB assembly, which has a separate workspace preflight.
		uint64 ReadBytes = 0, WrittenBytes = 0, CompressedBytes = 0;
		uint64 PeakWorkingSetBytes = 0;
		uint32 BlobsReused = 0, BlobsCompressed = 0, BlobsWritten = 0;
	};

	struct FCachePutBackendRequest
	{
		FCacheRecord Record;
		uint64 MaximumEncodedBytes = FCacheRecord::DefaultMaximumEncodedBytes;
		// Logical output value, metadata, and message bytes before compression.
		uint64 MaximumRawBytes = 2ull * 1024 * 1024 * 1024;
		uint64 MaximumWorkingSetBytes = 4ull * 1024 * 1024 * 1024;
		std::function<bool()> ShouldCancel;
		// Borrowed only by synchronous Put; async dispatch does not retain this pointer.
		FCacheStoreStats* Stats = nullptr;
	};

	using FCachePutResult = std::expected<void, FCacheError>;

	class ICacheBackend
	{
	public:
		virtual ~ICacheBackend() = default;
		virtual auto Get(const FCacheGetBackendRequest& Request) const -> FCacheGetResult = 0;
		virtual auto Put(const FCachePutBackendRequest& Request) const -> FCachePutResult = 0;
	};

	// Returns the process-owned structured record cache. Serialization,
	// compression, validation, and physical storage remain behind this boundary.
	DERIVEDDATACACHE_API auto GetCacheBackend() -> ICacheBackend&;
} // namespace Durin::DerivedData
