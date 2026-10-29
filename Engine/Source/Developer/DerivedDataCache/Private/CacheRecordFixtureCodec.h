#pragma once

#include "CoreMinimal.h"
#include "BuildCacheRecordPrivate.h"

namespace Durin::DerivedData
{
	// Historical whole-record format for corruption and legacy fixture coverage only.
	class FCacheRecordFixtureCodec
	{
	public:
		DERIVEDDATACACHE_API static auto Encode(const FCacheRecord& Record,
			uint64 MaximumBytes = FCacheRecord::DefaultMaximumEncodedBytes) -> std::expected<FSharedByteBuffer, FCacheError>;
		DERIVEDDATACACHE_API static auto CompressEncoded(const FSharedByteBuffer& Raw,
			uint64 MaximumBytes = FCacheRecord::DefaultMaximumEncodedBytes) -> std::expected<FSharedByteBuffer, FCacheError>;
		DERIVEDDATACACHE_API static auto Decode(const FCacheKey& Key, FSharedByteBuffer Bytes,
			FBuildOutputLimits Limits = {}, uint64 MaximumBytes = FCacheRecord::DefaultMaximumEncodedBytes) -> std::expected<FCacheRecord, FCacheError>;
	};
}
