#pragma once

#include "CoreMinimal.h"
#include "DerivedDataValue.h"

namespace Durin::DerivedData
{
	// Immutable, self-validating blob. Codec versions do not participate in action identity.
	class FCompressedCacheValue
	{
	public:
		DERIVEDDATACACHE_API static auto Compress(const FValue& Value, uint64 MaximumEncodedBytes,
			uint64 MaximumWorkingBytes, uint64* WorkingBytes = nullptr) -> std::expected<FCompressedCacheValue, FCacheError>;
		DERIVEDDATACACHE_API static auto Load(FSharedByteBuffer Bytes, uint64 MaximumRawBytes,
			uint64 MaximumEncodedBytes) -> std::expected<FCompressedCacheValue, FCacheError>;
		DERIVEDDATACACHE_API auto Decompress(uint64 MaximumWorkingBytes) const -> std::expected<FValue, FCacheError>;
		DERIVEDDATACACHE_API auto GetDecompressionWorkingBytes() const -> uint64;
		auto GetEncoded() const -> FSharedByteBuffer { return Encoded; }
		auto GetRawSize() const -> uint64 { return RawSize; }
		auto GetRawHash() const -> FXxHash128 { return RawHash; }
		auto IsCompressed() const -> bool { return Compressed; }
	private:
		FSharedByteBuffer Encoded;
		FXxHash128 RawHash;
		uint64 RawSize = 0;
		bool Compressed = false;
	};
}
