#include "CacheRecordFixtureCodec.h"
#include "BuildCacheRecordPrivate.h"
#include "Serialization/BinaryFormat.h"
#include <zstd.h>
namespace Durin::DerivedData
{
	namespace
	{
		constexpr uint32 CacheRecordMagic = 0x52424444;
		constexpr uint32 CacheRecordSchema = 2;
		auto CanonicalObjectBytes(const FSharedByteBuffer& Bytes) -> uint64
		{
			// An owned CB object may be a named field retained from a manifest.
			// Its logical metadata budget uses the canonical unnamed root size.
			uint32 NameBytes = 0;
			if (!ReadLittleEndianAt(Bytes.GetBytes(), 1, NameBytes)) return Bytes.GetSize();
			return Bytes.GetSize() - NameBytes;
		}
		auto CanonicalObjectEncoding(FSharedByteBuffer Bytes) -> FSharedByteBuffer
		{
			const auto Size = CanonicalObjectBytes(Bytes);
			if (Size == Bytes.GetSize()) return Bytes;
			FBinaryWriter W;
			W.WriteU8(uint8(ECbFieldType::Object));
			W.WriteU32(0);
			W.WriteU64(Size - 13);
			W.WriteBytes(Bytes.MakeView(Bytes.GetSize() - Size + 13, Size - 13).GetBytes());
			return FSharedByteBuffer::Take(W.TakeBytes());
		}
	} // namespace
	auto FCacheRecordFixtureCodec::Encode(const FCacheRecord& Record, uint64 MaximumBytes) -> std::expected<FSharedByteBuffer, FCacheError>
	try
	{
		if (!Record.IsValid()) return std::unexpected(FCacheError{ECacheError::InvalidRequest, "Cache record is empty."});
		if (std::ranges::any_of(Record.GetValues(), [](const auto& Item) { return !Item.Value.HasData(); }))
			return std::unexpected(FCacheError{ECacheError::InvalidRequest, "Schema-2 encoding requires resident value data."});
		auto Decoded = Private::ReadBuildOutput(Record.GetKey(), Record);
		if (!Decoded) return std::unexpected(std::move(Decoded.error()));
		const auto& Output = *Decoded;
		std::vector<FSharedByteBuffer> Metadata;
		for (const auto& Item : Output.GetMetadata())
			Metadata.push_back(CanonicalObjectEncoding(Item.Object.GetBytes()));
		FBinaryWriter W({.MaximumTotalBytes = MaximumBytes});
		W.WriteHeader({CacheRecordMagic, CacheRecordSchema, 0});
		W.WriteString(Record.GetKey().GetBucket().ToString());
		W.WriteHash128(Record.GetKey().GetHash());
		W.WriteString(Output.GetSchema());
		W.WriteU32(Output.GetSchemaVersion());
		W.WriteU32(uint32(Output.GetValues().size()));
		W.WriteU32(uint32(Output.GetMetadata().size()));
		W.WriteU32(uint32(Output.GetMessages().size()));
		for (const auto& Item : Output.GetValues())
		{
			W.WriteBytes(std::as_bytes(std::span(Item.Id.GetBytes())));
			W.WriteU64(Item.Value.GetRawSize());
			W.WriteHash128(Item.Value.GetRawHash());
		}
		for (size_t Index = 0; Index < Output.GetMetadata().size(); ++Index)
		{
			W.WriteBytes(std::as_bytes(std::span(Output.GetMetadata()[Index].Id.GetBytes())));
			W.WriteU64(Metadata[Index].GetSize());
			W.WriteHash128(FXxHash128::HashBuffer(Metadata[Index].GetBytes()));
		}
		for (const auto& M : Output.GetMessages())
		{
			W.WriteU8(uint8(M.Severity));
			W.WriteString(M.Text);
		}
		for (const auto& Item : Output.GetValues())
			W.WriteBytes(Item.Value.GetData().GetBytes());
		for (const auto& Object : Metadata)
			W.WriteBytes(Object.GetBytes());
		if (W.HasError()) return std::unexpected(FCacheError{ECacheError::ValueTooLarge, "Cache record encoding exceeds its byte budget."});
		const auto Hash = FXxHash128::HashBuffer(W.GetBytes());
		W.WriteHash128(Hash);
		if (W.HasError()) return std::unexpected(FCacheError{ECacheError::ValueTooLarge, "Cache record trailer exceeds its byte budget."});
		return FSharedByteBuffer::Take(W.TakeBytes());
	}
	catch (const std::bad_alloc&)
	{
		return std::unexpected(FCacheError{ECacheError::StorageFailure, "Allocation"});
	}

	auto FCacheRecordFixtureCodec::CompressEncoded(const FSharedByteBuffer& Raw, uint64 MaximumBytes) -> std::expected<FSharedByteBuffer, FCacheError>
	try
	{
		FBinaryReader R(Raw.GetBytes());
		if (!R.ReadAndValidateHeader(CacheRecordMagic, CacheRecordSchema, 0)) return std::unexpected(FCacheError{ECacheError::InvalidRequest, "Compression requires a raw cache record."});
		constexpr uint64 FrameBytes = 40;
		if (Raw.GetSize() > MaximumBytes || MaximumBytes <= FrameBytes) return std::unexpected(FCacheError{ECacheError::ValueTooLarge, "Cache record compression exceeds its byte budget."});
		FByteBuffer Compressed(std::min<uint64>(ZSTD_compressBound(Raw.size()), MaximumBytes - FrameBytes));
		const size_t Size = ZSTD_compress(Compressed.data(), Compressed.size(), Raw.data(), Raw.size(), 3);
		if (ZSTD_isError(Size)) return std::unexpected(FCacheError{ECacheError::StorageFailure, "Cache record compression failed."});
		Compressed.resize(Size);
		FBinaryWriter W({.MaximumTotalBytes = MaximumBytes});
		W.WriteHeader({CacheRecordMagic, CacheRecordSchema, 1});
		W.WriteU64(Raw.GetSize());
		W.WriteBytes(Compressed);
		W.WriteHash128(FXxHash128::HashBuffer(W.GetBytes()));
		if (W.HasError()) return std::unexpected(FCacheError{ECacheError::ValueTooLarge, "Compressed cache record exceeds its byte budget."});
		return FSharedByteBuffer::Take(W.TakeBytes());
	}
	catch (const std::bad_alloc&)
	{
		return std::unexpected(FCacheError{ECacheError::StorageFailure, "Allocation"});
	}

	auto FCacheRecordFixtureCodec::Decode(const FCacheKey& ExpectedKey, FSharedByteBuffer Bytes, FBuildOutputLimits Limits, uint64 MaximumBytes) -> std::expected<FCacheRecord, FCacheError>
	try
	{
		auto Corrupt = [] { return std::unexpected(FCacheError{ECacheError::Corrupt, "Cache record envelope is malformed or unsupported."}); };
		if (!ExpectedKey.IsValid()) return std::unexpected(FCacheError{ECacheError::InvalidRequest, "Cache record requires an action key."});
		if (Bytes.GetSize() > MaximumBytes || Bytes.GetSize() < 32) return Corrupt();
		const uint64 BodySize = Bytes.GetSize() - 16;
		FBinaryReader Trailer(Bytes.MakeView(BodySize, 16).GetBytes());
		FXxHash128 Hash;
		if (!Trailer.ReadHash128(Hash) || Hash != FXxHash128::HashBuffer(Bytes.MakeView(0, BodySize).GetBytes())) return Corrupt();
		uint32 Format = 0;
		if (!ReadLittleEndianAt(Bytes.GetBytes(), 8, Format)) return Corrupt();
		if (Format == 1)
		{
			FBinaryReader R(Bytes.MakeView(0, BodySize).GetBytes(), {.MaximumTotalBytes = MaximumBytes});
			uint64 Size = 0;
			FByteView Frame;
			if (!R.ReadAndValidateHeader(CacheRecordMagic, CacheRecordSchema, 1) || !R.ReadU64(Size) || Size > MaximumBytes || !R.ReadRegion(Frame, R.GetRemainingBytes(), MaximumBytes) || ZSTD_getFrameContentSize(Frame.data(), Frame.size()) != Size || ZSTD_findFrameCompressedSize(Frame.data(), Frame.size()) != Frame.size()) return Corrupt();
			FByteBuffer Raw(Size);
			if (ZSTD_decompress(Raw.data(), Raw.size(), Frame.data(), Frame.size()) != Size) return Corrupt();
			return Decode(ExpectedKey, FSharedByteBuffer::Take(std::move(Raw)), Limits, MaximumBytes);
		}
		FBinaryReader R(Bytes.MakeView(0, BodySize).GetBytes(), {.MaximumTotalBytes = MaximumBytes});
		std::string Bucket, Schema;
		FXxHash128 KeyHash;
		uint32 Version = 0, ValueCount = 0, MetaCount = 0, MessageCount = 0;
		if (!R.ReadAndValidateHeader(CacheRecordMagic, CacheRecordSchema, 0) || !R.ReadString(Bucket, FCacheBucket::MaximumNameLength) || !R.ReadHash128(KeyHash) || Bucket != ExpectedKey.GetBucket().ToString() || KeyHash != ExpectedKey.GetHash() || !R.ReadString(Schema, 96) || !R.ReadU32(Version) || !R.ReadU32(ValueCount) || !R.ReadU32(MetaCount) || !R.ReadU32(MessageCount) || ValueCount > Limits.MaximumValues || MetaCount > Limits.MaximumMetadata || MessageCount > Limits.MaximumMessages) return Corrupt();
		struct FDescriptor
		{
			FValueId Id;
			uint64 Size;
			FXxHash128 Hash;
		};
		std::vector<FDescriptor> Values(ValueCount), Meta(MetaCount);
		auto ReadDescriptors = [&](auto& List) { for (auto& D : List) { FByteView Id; if (!R.ReadRegion(Id, 12, 12) || !R.ReadU64(D.Size) || !R.ReadHash128(D.Hash)) return false; std::array<uint8, 12> Raw{}; for (size_t I = 0; I < 12; ++I) Raw[I] = uint8(Id[I]); D.Id = FValueId::FromBytes(Raw); } return true; };
		if (!ReadDescriptors(Values) || !ReadDescriptors(Meta)) return Corrupt();
		std::vector<FBuildOutputMessage> Messages(MessageCount);
		for (auto& M : Messages)
		{
			uint8 S;
			if (!R.ReadU8(S) || !R.ReadString(M.Text, 4096)) return Corrupt();
			M.Severity = EBuildMessageSeverity(S);
		}
		FBuildOutputBuilder Builder(std::move(Schema), Version, Limits);
		uint64 Offset = R.Tell();
		for (const auto& D : Values)
		{
			if (D.Size > BodySize - Offset) return Corrupt();
			auto Data = Bytes.MakeView(Offset, D.Size);
			if (!Builder.AddValue(D.Id, std::move(Data))) return Corrupt();
			Offset += D.Size;
		}
		for (const auto& D : Meta)
		{
			if (D.Size > BodySize - Offset) return Corrupt();
			auto Data = Bytes.MakeView(Offset, D.Size);
			if (FXxHash128::HashBuffer(Data.GetBytes()) != D.Hash) return Corrupt();
			auto Object = FCbObject::TryLoad(Data);
			if (!Object || !Builder.AddMeta(D.Id, std::move(*Object))) return Corrupt();
			Offset += D.Size;
		}
		if (Offset != BodySize) return Corrupt();
		for (auto& M : Messages)
			if (!Builder.AddMessage(M.Severity, std::move(M.Text))) return Corrupt();
		auto Output = std::move(Builder).Build();
		if (!Output) return Corrupt();
		// Inspect the validated public values instead of accessing builder internals
		// or hashing their immutable payloads a second time in this fixture codec.
		for (const auto& D : Values)
		{
			const auto* Value = Output->FindValue(D.Id);
			if (!Value || Value->GetRawHash() != D.Hash) return Corrupt();
		}
		return Private::MakeBuildCacheRecord(ExpectedKey, *Output, Limits);
	}
	catch (const std::bad_alloc&)
	{
		return std::unexpected(FCacheError{ECacheError::StorageFailure, "Allocation"});
	}

} // namespace Durin::DerivedData
