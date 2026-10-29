#pragma once
#include "CoreMinimal.h"
#include "DerivedDataCacheRecord.h"
namespace Durin::DerivedData::Private
{
	// Wire identity/versioning belongs to the local storage implementation.
	struct FCacheBlobIdentity
	{
		static constexpr uint32 Version = 1;
		static constexpr std::string_view Algorithm = "xxh3-128";
		static constexpr std::string_view Namespace = "values/v1";
		FXxHash128 Hash;
		auto operator==(const FCacheBlobIdentity&) const -> bool = default;
	};
	inline auto BlobIdentity(const FValue& Value) -> FCacheBlobIdentity { return {Value.GetRawHash()}; }
	inline constexpr std::string_view CacheRecordNamespace = "records/v5";
	DERIVEDDATACACHE_API auto EncodeCacheManifest(const FCacheRecord& Record, uint64 MaximumBytes = FCacheRecord::DefaultMaximumEncodedBytes, uint64 MaximumWorkingBytes = UINT64_MAX) -> std::expected<FSharedByteBuffer, FCacheError>;
	DERIVEDDATACACHE_API auto DecodeCacheManifest(const FCacheKey& Key, FSharedByteBuffer Bytes, FCacheRecordLimits Limits = {}, uint64 MaximumBytes = FCacheRecord::DefaultMaximumEncodedBytes) -> std::expected<FCacheRecord, FCacheError>;
} // namespace Durin::DerivedData::Private
