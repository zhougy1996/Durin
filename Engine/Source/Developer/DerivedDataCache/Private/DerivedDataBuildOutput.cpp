#include "DerivedDataBuildOutput.h"
#include "DerivedDataBuildValidation.h"
#include "Serialization/BinaryFormat.h"
#include <zstd.h>

namespace Durin::DerivedData
{
	namespace
	{
		constexpr uint32 CacheRecordMagic = 0x52424444; // DDBR

		auto IsIdentifier(std::string_view Id) -> bool { return Private::IsBuildValueIdentifier(Id); }

		auto Validate(const FBuildOutputData& Data, FBuildOutputLimits Limits)
			-> std::expected<void, std::string>
		{
			if (!IsIdentifier(Data.Schema) || Data.SchemaVersion == 0)
				return std::unexpected("Build output schema is invalid.");
			if (Data.Metadata.GetSize() > std::min<uint64>(Limits.MaximumMetadataBytes, 4ull * 1024 * 1024)
				|| Data.Values.size() > std::min<uint32>(Limits.MaximumValues, 4096))
				return std::unexpected("Build output table or metadata limit exceeded.");
			uint64 Total = 0;
			auto Add = [&](uint64 Bytes) {
				if (Bytes > Limits.MaximumTotalBytes - Total) return false;
				Total += Bytes;
				return true;
			};
			if (!Add(Data.Metadata.GetSize())) return std::unexpected("Build output byte limit exceeded.");
			std::string_view Previous;
			for (const auto& Value : Data.Values)
			{
				if (!IsIdentifier(Value.Id) || (!Previous.empty() && Previous >= Value.Id))
					return std::unexpected("Build output values have invalid or duplicate IDs.");
				Previous = Value.Id;
				if (!Add(Value.Data.GetSize())) return std::unexpected("Build output byte limit exceeded.");
			}
			return {};
		}
	}

	auto FBuildOutput::TryCreate(FBuildOutputData Data, FBuildOutputLimits Limits)
		-> std::expected<FBuildOutput, std::string>
	try
	{
		if (Data.Values.size() > std::min<uint32>(Limits.MaximumValues, 4096))
			return std::unexpected("Build output value limit exceeded.");
		// Canonical lexical ordering is independent of construction/container order.
		std::ranges::sort(Data.Values, {}, &FBuildValue::Id);
		if (auto Valid = Validate(Data, Limits); !Valid) return std::unexpected(std::move(Valid.error()));
		FBuildOutput Result;
		Result.State = std::make_shared<const FBuildOutputData>(std::move(Data));
		return Result;
	}

	catch (const std::bad_alloc&) { return std::unexpected("Allocation"); }

	auto FBuildOutput::CheckLimits(FBuildOutputLimits Limits) const -> std::expected<void, std::string>
	{
		if (!State) return std::unexpected("Build output is empty.");
		return Validate(*State, Limits);
	}

	auto FBuildOutput::FindValue(std::string_view Id) const -> const FBuildValue*
	{
		const auto Values = GetValues();
		const auto Found = std::ranges::lower_bound(Values, Id, {}, &FBuildValue::Id);
		return Found != Values.end() && Found->Id == Id ? &*Found : nullptr;
	}

	struct FCacheRecord::FState
	{
		FCacheKey Key;
		FBuildOutputData Data;
		FXxHash128 MetadataHash;
		std::vector<FXxHash128> ValueHashes;
	};

	auto FCacheRecord::FromOutput(const FCacheKey& Key, const FBuildOutput& Output, FBuildOutputLimits Limits)
		-> std::expected<FCacheRecord, FCacheError>
	try
	{
		if (!Key.IsValid() || !Output.State)
			return std::unexpected(FCacheError{ECacheError::InvalidRequest, "Cache record requires a key and complete output."});
		if (auto Valid = Validate(*Output.State, Limits); !Valid)
			return std::unexpected(FCacheError{ECacheError::ValueTooLarge, std::move(Valid.error())});
		auto State = std::make_shared<FState>();
		State->Key = Key;
		State->Data = *Output.State;
		State->MetadataHash = FXxHash128::HashBuffer(State->Data.Metadata.GetBytes());
		State->ValueHashes.reserve(State->Data.Values.size());
		for (const auto& Value : State->Data.Values)
			State->ValueHashes.push_back(FXxHash128::HashBuffer(Value.Data.GetBytes()));
		FCacheRecord Record;
		Record.State = std::move(State);
		return Record;
	}

	catch (const std::bad_alloc&) { return std::unexpected(FCacheError{ECacheError::StorageFailure, "Allocation"}); }

	auto FCacheRecord::ToOutput(const FCacheKey& ExpectedKey, FBuildOutputLimits Limits) const
		-> std::expected<FBuildOutput, FCacheError>
	{
		if (!State || !ExpectedKey.IsValid() || State->Key != ExpectedKey)
			return std::unexpected(FCacheError{ECacheError::Corrupt, "Cache record key does not match the requested action."});
		if (auto Valid = Validate(State->Data, Limits); !Valid)
			return std::unexpected(FCacheError{ECacheError::ValueTooLarge, std::move(Valid.error())});
		if (FXxHash128::HashBuffer(State->Data.Metadata.GetBytes()) != State->MetadataHash)
			return std::unexpected(FCacheError{ECacheError::Corrupt, "Cache record metadata hash mismatch."});
		for (size_t Index = 0; Index < State->Data.Values.size(); ++Index)
			if (FXxHash128::HashBuffer(State->Data.Values[Index].Data.GetBytes()) != State->ValueHashes[Index])
				return std::unexpected(FCacheError{ECacheError::Corrupt, "Cache record value hash mismatch."});
		FBuildOutput Output;
		// Share the record's immutable descriptor state without retaining a product.
		Output.State = std::shared_ptr<const FBuildOutputData>(State, &State->Data);
		return Output;
	}

	auto FCacheRecord::Encode(uint64 MaximumBytes) const -> std::expected<FSharedByteBuffer, FCacheError>
	try
	{
		if (!State) return std::unexpected(FCacheError{ECacheError::InvalidRequest, "Cache record is empty."});
		if (auto Output = ToOutput(State->Key); !Output) return std::unexpected(std::move(Output.error()));
		FBinaryWriter Writer({.MaximumTotalBytes = MaximumBytes});
		Writer.WriteHeader({CacheRecordMagic, 1, 0}); // Envelope schema 1, raw blocks.
		Writer.WriteString(State->Key.GetBucket().ToString());
		Writer.WriteHash128(State->Key.GetHash());
		Writer.WriteString(State->Data.Schema);
		Writer.WriteU32(State->Data.SchemaVersion);
		Writer.WriteU32(static_cast<uint32>(State->Data.Values.size()));
		Writer.WriteU32(0); // Reserved v1 message count; existing message-free records remain readable.
		Writer.WriteU64(State->Data.Metadata.GetSize());
		Writer.WriteHash128(State->MetadataHash);
		uint64 Offset = State->Data.Metadata.GetSize();
		for (size_t Index = 0; Index < State->Data.Values.size(); ++Index)
		{
			const auto& Value = State->Data.Values[Index];
			Writer.WriteString(Value.Id);
			Writer.WriteU64(Offset);
			Writer.WriteU64(Value.Data.GetSize());
			Writer.WriteHash128(State->ValueHashes[Index]);
			Offset += Value.Data.GetSize(); // ToOutput checked the aggregate bound.
		}
		// The table is complete; reserve the exact payload/trailer footprint so
		// mip-sized appends never reallocate and copy an already encoded payload.
		if (Writer.HasError() || Writer.Tell() > MaximumBytes || Offset > MaximumBytes - Writer.Tell()
			|| 16 > MaximumBytes - Writer.Tell() - Offset)
			return std::unexpected(FCacheError{ECacheError::ValueTooLarge, "Cache record encoding exceeds its byte budget."});
		Writer.Reserve(Writer.Tell() + Offset + 16);
		Writer.WriteBytes(State->Data.Metadata.GetBytes());
		for (const auto& Value : State->Data.Values) Writer.WriteBytes(Value.Data.GetBytes());
		if (Writer.HasError())
			return std::unexpected(FCacheError{ECacheError::ValueTooLarge, "Cache record encoding exceeds its byte budget."});
		const auto EnvelopeHash = FXxHash128::HashBuffer(Writer.GetBytes());
		Writer.WriteHash128(EnvelopeHash);
		if (Writer.HasError())
			return std::unexpected(FCacheError{ECacheError::ValueTooLarge, "Cache record integrity trailer exceeds its byte budget."});
		return FSharedByteBuffer::Take(Writer.TakeBytes());
	}
	catch (const std::bad_alloc&) { return std::unexpected(FCacheError{ECacheError::StorageFailure, "Allocation"}); }

	auto FCacheRecord::CompressEncoded(const FSharedByteBuffer& RawRecord, uint64 MaximumBytes)
		-> std::expected<FSharedByteBuffer, FCacheError>
	try
	{
		FBinaryReader Reader(RawRecord.GetBytes());
		if (!Reader.ReadAndValidateHeader(CacheRecordMagic, 1, 0))
			return std::unexpected(FCacheError{ECacheError::InvalidRequest, "Compression requires a raw cache record."});
		constexpr uint64 FramingBytes = 40;
		if (MaximumBytes <= FramingBytes || RawRecord.GetSize() > MaximumBytes)
			return std::unexpected(FCacheError{ECacheError::ValueTooLarge, "Cache record compression exceeds its byte budget."});
		const size_t Bound = ZSTD_compressBound(RawRecord.size());
		if (ZSTD_isError(Bound))
			return std::unexpected(FCacheError{ECacheError::StorageFailure, "Cache record compression bound failed."});
		FByteBuffer Compressed(static_cast<size_t>(std::min<uint64>(Bound, MaximumBytes - FramingBytes)));
		const size_t Size = ZSTD_compress(Compressed.data(), Compressed.size(), RawRecord.data(), RawRecord.size(), 3);
		if (ZSTD_isError(Size))
			return std::unexpected(FCacheError{ECacheError::StorageFailure, "Cache record compression failed."});
		Compressed.resize(Size);
		FBinaryWriter Writer({.MaximumTotalBytes = MaximumBytes});
		Writer.WriteHeader({CacheRecordMagic, 1, 1});
		Writer.WriteU64(RawRecord.GetSize());
		Writer.WriteBytes(Compressed);
		const auto Hash = FXxHash128::HashBuffer(Writer.GetBytes());
		Writer.WriteHash128(Hash);
		if (Writer.HasError())
			return std::unexpected(FCacheError{ECacheError::ValueTooLarge, "Compressed cache record exceeds its byte budget."});
		return FSharedByteBuffer::Take(Writer.TakeBytes());
	}
	catch (const std::bad_alloc&) { return std::unexpected(FCacheError{ECacheError::StorageFailure, "Allocation"}); }

	auto FCacheRecord::Decode(const FCacheKey& ExpectedKey, FSharedByteBuffer Bytes,
		FBuildOutputLimits Limits, uint64 MaximumEncodedBytes) -> std::expected<FCacheRecord, FCacheError>
	try
	{
		auto Corrupt = [] { return std::unexpected(FCacheError{ECacheError::Corrupt, "Cache record envelope is malformed or unsupported."}); };
		if (!ExpectedKey.IsValid())
			return std::unexpected(FCacheError{ECacheError::InvalidRequest, "Cache record requires an action key."});
		if (Bytes.GetSize() > MaximumEncodedBytes)
			return std::unexpected(FCacheError{ECacheError::ValueTooLarge, "Cache record exceeds its encoded byte budget."});
		if (Bytes.GetSize() < 32) return Corrupt();
		const uint64 BodySize = Bytes.GetSize() - 16;
		FBinaryReader Trailer(Bytes.MakeView(BodySize, 16).GetBytes());
		FXxHash128 EnvelopeHash;
		if (!Trailer.ReadHash128(EnvelopeHash)
			|| FXxHash128::HashBuffer(Bytes.MakeView(0, BodySize).GetBytes()) != EnvelopeHash) return Corrupt();
		FBinaryReader Reader(Bytes.MakeView(0, BodySize).GetBytes(), {.MaximumTotalBytes = MaximumEncodedBytes});
		uint32 Format = 0;
		if (!ReadLittleEndianAt(Bytes.GetBytes(), 8, Format)) return Corrupt();
		if (Format == 1)
		{
			uint64 DecodedSize = 0;
			FByteView Frame;
			constexpr uint64 MaximumFramingBytes = 2ull * 1024 * 1024;
			const uint64 InflatedLimit = Limits.MaximumTotalBytes > std::numeric_limits<uint64>::max() - MaximumFramingBytes
				? MaximumEncodedBytes : std::min(MaximumEncodedBytes, Limits.MaximumTotalBytes + MaximumFramingBytes);
			if (!Reader.ReadAndValidateHeader(CacheRecordMagic, 1, 1) || !Reader.ReadU64(DecodedSize)
				|| DecodedSize < 32 || DecodedSize > InflatedLimit || DecodedSize > std::numeric_limits<size_t>::max()
				|| !Reader.ReadRegion(Frame, Reader.GetRemainingBytes(), MaximumEncodedBytes)
				|| ZSTD_getFrameContentSize(Frame.data(), Frame.size()) != DecodedSize) return Corrupt();
			const size_t FrameSize = ZSTD_findFrameCompressedSize(Frame.data(), Frame.size());
			if (ZSTD_isError(FrameSize) || FrameSize != Frame.size()) return Corrupt();
			FByteBuffer Inflated(static_cast<size_t>(DecodedSize));
			const size_t Size = ZSTD_decompress(Inflated.data(), Inflated.size(), Frame.data(), Frame.size());
			if (ZSTD_isError(Size) || Size != DecodedSize
				|| !ReadLittleEndianAt(FByteView(Inflated), 8, Format) || Format != 0) return Corrupt();
			return Decode(ExpectedKey, FSharedByteBuffer::Take(std::move(Inflated)), Limits, MaximumEncodedBytes);
		}
		auto State = std::make_shared<FState>();
		std::string Bucket;
		FXxHash128 KeyHash;
		uint32 ValueCount = 0, MessageCount = 0;
		uint64 MetadataSize = 0;
		if (!Reader.ReadAndValidateHeader(CacheRecordMagic, 1, 0)
			|| !Reader.ReadString(Bucket, FCacheBucket::MaximumNameLength) || !Reader.ReadHash128(KeyHash)
			|| Bucket != ExpectedKey.GetBucket().ToString() || KeyHash != ExpectedKey.GetHash()
			|| !Reader.ReadString(State->Data.Schema, 96) || !Reader.ReadU32(State->Data.SchemaVersion)
			|| !Reader.ReadU32(ValueCount) || !Reader.ReadU32(MessageCount)
			|| ValueCount > std::min<uint32>(Limits.MaximumValues, 4096)
			|| MessageCount != 0
			|| !Reader.ReadU64(MetadataSize) || !Reader.ReadHash128(State->MetadataHash)) return Corrupt();
		if (MetadataSize > std::min<uint64>(Limits.MaximumMetadataBytes, 4ull * 1024 * 1024)
			|| MetadataSize > Limits.MaximumTotalBytes)
			return std::unexpected(FCacheError{ECacheError::ValueTooLarge, "Cache record metadata exceeds its byte budget."});
		State->Key = ExpectedKey;
		struct FRegion { uint64 Offset = 0, Size = 0; };
		std::vector<FRegion> Regions(ValueCount);
		State->Data.Values.resize(ValueCount);
		State->ValueHashes.resize(ValueCount);
		uint64 Total = MetadataSize;
		for (uint32 Index = 0; Index < ValueCount; ++Index)
		{
			auto& Region = Regions[Index];
			if (!Reader.ReadString(State->Data.Values[Index].Id, 96)
				|| !Reader.ReadU64(Region.Offset) || !Reader.ReadU64(Region.Size)
				|| !Reader.ReadHash128(State->ValueHashes[Index]) || Region.Offset != Total
				|| Region.Size > Limits.MaximumTotalBytes - Total) return Corrupt();
			Total += Region.Size;
		}
		const uint64 DataOffset = Reader.Tell();
		if (Total != Reader.GetRemainingBytes()) return Corrupt();
		State->Data.Metadata = Bytes.MakeView(DataOffset, MetadataSize);
		for (uint32 Index = 0; Index < ValueCount; ++Index)
			State->Data.Values[Index].Data = Bytes.MakeView(DataOffset + Regions[Index].Offset, Regions[Index].Size);
		FCacheRecord Record;
		Record.State = std::move(State);
		// Validates canonical IDs and every block before returning any view.
		if (auto Output = Record.ToOutput(ExpectedKey, Limits); !Output)
			return std::unexpected(std::move(Output.error()));
		return Record;
	}
	catch (const std::bad_alloc&) { return std::unexpected(FCacheError{ECacheError::StorageFailure, "Allocation"}); }

	auto FCacheRecord::GetKey() const -> FCacheKey { return State ? State->Key : FCacheKey{}; }
	auto FCacheRecord::GetSchema() const -> std::string_view { return State ? State->Data.Schema : std::string_view{}; }
	auto FCacheRecord::GetSchemaVersion() const -> uint32 { return State ? State->Data.SchemaVersion : 0; }
	auto FCacheRecord::GetMetadata() const -> FSharedByteBuffer { return State ? State->Data.Metadata : FSharedByteBuffer{}; }
	auto FCacheRecord::GetMetadataHash() const -> FXxHash128 { return State ? State->MetadataHash : FXxHash128{}; }
	auto FCacheRecord::GetValues() const -> std::span<const FBuildValue>
	{
		return State ? std::span(State->Data.Values) : std::span<const FBuildValue>{};
	}
	auto FCacheRecord::GetValueHashes() const -> std::span<const FXxHash128>
	{
		return State ? std::span(State->ValueHashes) : std::span<const FXxHash128>{};
	}
}
