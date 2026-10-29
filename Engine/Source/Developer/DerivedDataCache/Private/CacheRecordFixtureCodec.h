#pragma once

#include "CoreMinimal.h"
#include "BuildCacheRecordPrivate.h"
#include "DerivedDataCache/DerivedDataCache.h"

namespace Durin::DerivedData
{
	// Historical whole-record format for corruption and legacy fixture coverage only.
	class FCacheRecordFixtureCodec
	{
	public:
		DERIVEDDATACACHE_API static auto Encode(const FCacheRecord& Record,
			uint64 MaximumBytes = FCacheResourceLimits{}.MaximumEncodedBytes) -> std::expected<FSharedByteBuffer, FCacheError>;
		DERIVEDDATACACHE_API static auto CompressEncoded(const FSharedByteBuffer& Raw,
			uint64 MaximumBytes = FCacheResourceLimits{}.MaximumEncodedBytes) -> std::expected<FSharedByteBuffer, FCacheError>;
		DERIVEDDATACACHE_API static auto Decode(const FCacheKey& Key, FSharedByteBuffer Bytes,
			FBuildOutputLimits Limits = {}, uint64 MaximumBytes = FCacheResourceLimits{}.MaximumEncodedBytes) -> std::expected<FCacheRecord, FCacheError>;
	};
}
