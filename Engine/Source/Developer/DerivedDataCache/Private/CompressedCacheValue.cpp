#include "CompressedCacheValue.h"
#include "Serialization/BinaryFormat.h"
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#include <zstd_errors.h>

namespace Durin::DerivedData
{
	namespace
	{
		constexpr uint32 BlobMagic = 0x56424444; // DDBV
		constexpr uint64 EnvelopeBytes = 64;
		auto TooLarge() { return std::unexpected(FCacheError{ECacheError::ValueTooLarge, "Value exceeds cache byte or workspace budget."}); }
		auto Corrupt() { return std::unexpected(FCacheError{ECacheError::Corrupt, "Cache value blob is malformed or has invalid content."}); }
	}

	auto FCompressedCacheValue::Compress(const FValue& Value, uint64 MaximumEncodedBytes,
		uint64 MaximumWorkingBytes, uint64* WorkingBytes) -> std::expected<FCompressedCacheValue, FCacheError>
	try
	{
		if (!Value.HasData() || Value.GetRawHash().IsZero())
			return std::unexpected(FCacheError{ECacheError::InvalidRequest, "Compression requires resident value data."});
		if (WorkingBytes) *WorkingBytes = 0;
		const uint64 RawSize = Value.GetRawSize();
		if (MaximumEncodedBytes < EnvelopeBytes || MaximumWorkingBytes < EnvelopeBytes) return TooLarge();
		FByteBuffer Compressed;
		FByteView Payload = Value.GetData().GetBytes();
		bool UseCompression = false;
		const uint64 ContextBytes = ZSTD_estimateCCtxSize(3);
		const uint64 CandidateBytes = std::min<uint64>(RawSize, MaximumEncodedBytes - EnvelopeBytes);
		// Reserve candidate + final envelope + the estimated Zstd context before compression.
		if (RawSize >= 256 && !ZSTD_isError(ContextBytes) && ContextBytes <= MaximumWorkingBytes - EnvelopeBytes
			&& CandidateBytes <= (MaximumWorkingBytes - EnvelopeBytes - ContextBytes) / 2)
		{
			if (WorkingBytes) *WorkingBytes = CandidateBytes + ContextBytes;
			Compressed.resize(CandidateBytes);
			const auto Size = ZSTD_compress(Compressed.data(), Compressed.size(), Payload.data(), Payload.size(), 3);
			if (!ZSTD_isError(Size) && Size < RawSize)
			{ Compressed.resize(Size); Payload = Compressed; UseCompression = true; }
			else if (ZSTD_isError(Size) && ZSTD_getErrorCode(Size) != ZSTD_error_dstSize_tooSmall)
				return std::unexpected(FCacheError{ECacheError::StorageFailure, "Zstd value compression failed."});
		}
		if (!UseCompression) { FByteBuffer{}.swap(Compressed); }
		const uint64 Scratch = UseCompression ? CandidateBytes : 0;
		if (Payload.size() > MaximumEncodedBytes - EnvelopeBytes || Scratch > MaximumWorkingBytes - EnvelopeBytes
			|| Payload.size() > MaximumWorkingBytes - EnvelopeBytes - Scratch) return TooLarge();
		if (WorkingBytes) *WorkingBytes = std::max(*WorkingBytes, Scratch + Payload.size() + EnvelopeBytes);
		FBinaryWriter W({.MaximumTotalBytes = MaximumEncodedBytes}); W.Reserve(Payload.size() + EnvelopeBytes);
		W.WriteHeader({BlobMagic, 1, uint32(UseCompression)});
		W.WriteHash128(Value.GetRawHash()); W.WriteU64(RawSize); W.WriteU64(Payload.size()); W.WriteBytes(Payload);
		W.WriteHash128(FXxHash128::HashBuffer(W.GetBytes()));
		if (W.HasError()) return TooLarge();
		FCompressedCacheValue Result; Result.Encoded = FSharedByteBuffer::Take(W.TakeBytes());
		Result.RawHash = Value.GetRawHash(); Result.RawSize = RawSize; Result.Compressed = UseCompression; return Result;
	}
	catch (const std::bad_alloc&) { return std::unexpected(FCacheError{ECacheError::StorageFailure, "Value compression allocation failed."}); }

	auto FCompressedCacheValue::Load(FSharedByteBuffer Bytes, uint64 MaximumRawBytes,
		uint64 MaximumEncodedBytes) -> std::expected<FCompressedCacheValue, FCacheError>
	{
		if (Bytes.GetSize() > MaximumEncodedBytes) return TooLarge();
		if (Bytes.GetSize() < EnvelopeBytes) return Corrupt();
		const uint64 BodySize = Bytes.GetSize() - 16;
		FBinaryReader Trailer(Bytes.MakeView(BodySize, 16).GetBytes()); FXxHash128 Hash;
		if (!Trailer.ReadHash128(Hash) || Hash != FXxHash128::HashBuffer(Bytes.MakeView(0, BodySize).GetBytes())) return Corrupt();
		FBinaryReader R(Bytes.MakeView(0, BodySize).GetBytes()); FBinaryFormatHeader Header;
		uint32 Format = 0; if (!ReadLittleEndianAt(Bytes.GetBytes(), 8, Format) || Format > 1) return Corrupt();
		FCompressedCacheValue Result; uint64 PayloadSize = 0; FByteView Payload;
		if (!R.ReadAndValidateHeader(BlobMagic, 1, Format, &Header) || !R.ReadHash128(Result.RawHash)
			|| Result.RawHash.IsZero() || !R.ReadU64(Result.RawSize) || !R.ReadU64(PayloadSize)) return Corrupt();
		if (Result.RawSize > MaximumRawBytes) return TooLarge();
		if (!R.ReadRegion(Payload, PayloadSize, MaximumEncodedBytes) || !R.IsAtEnd()) return Corrupt();
		if (Format == 0 && PayloadSize != Result.RawSize) return Corrupt();
		if (Format == 1 && (ZSTD_getFrameContentSize(Payload.data(), Payload.size()) != Result.RawSize
			|| ZSTD_findFrameCompressedSize(Payload.data(), Payload.size()) != Payload.size())) return Corrupt();
		Result.Compressed = Format == 1; Result.Encoded = std::move(Bytes); return Result;
	}

	auto FCompressedCacheValue::Decompress(uint64 MaximumWorkingBytes) const -> std::expected<FValue, FCacheError>
	try
	{
		if (Encoded.IsEmpty()) return Corrupt();
		FSharedByteBuffer Raw;
		if (!Compressed) Raw = Encoded.MakeView(48, RawSize);
		else
		{
			const uint64 ContextBytes = ZSTD_estimateDCtxSize();
			if (ContextBytes > MaximumWorkingBytes || RawSize > MaximumWorkingBytes - ContextBytes) return TooLarge();
			FByteBuffer Bytes(RawSize); const auto Frame = Encoded.MakeView(48, Encoded.GetSize() - EnvelopeBytes);
			if (ZSTD_decompress(Bytes.data(), Bytes.size(), Frame.data(), Frame.size()) != RawSize) return Corrupt();
			Raw = FSharedByteBuffer::Take(std::move(Bytes));
		}
		FValue Value(std::move(Raw)); if (Value.GetRawHash() != RawHash) return Corrupt(); return Value;
	}
	catch (const std::bad_alloc&) { return std::unexpected(FCacheError{ECacheError::StorageFailure, "Value decompression allocation failed."}); }
	auto FCompressedCacheValue::GetDecompressionWorkingBytes() const -> uint64
	{ return Compressed ? RawSize + ZSTD_estimateDCtxSize() : 0; }
}
