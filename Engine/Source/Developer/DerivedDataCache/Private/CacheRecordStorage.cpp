#include "CacheRecordStorage.h"
#include "Serialization/BinaryFormat.h"
namespace Durin::DerivedData::Private
{
	namespace
	{
		constexpr uint64 MaximumManifestBytes = 8ull * 1024 * 1024;
		auto ManifestLimits(uint64 Bytes) -> FCbLimits { return {.MaximumEncodedBytes = std::min(Bytes, MaximumManifestBytes), .MaximumDepth = 40, .MaximumFields = 700000}; }
		auto HashBytes(FXxHash128 Hash) -> FSharedByteBuffer
		{
			FBinaryWriter W;
			W.WriteHash128(Hash);
			return FSharedByteBuffer::Take(W.TakeBytes());
		}
		auto ReadHash(FCbFieldView Field) -> std::optional<FXxHash128>
		{
			const auto Bytes = Field.AsBinary();
			if (!Bytes || Bytes->GetSize() != 16) return {};
			FBinaryReader R(Bytes->GetBytes());
			FXxHash128 Hash;
			if (!R.ReadHash128(Hash)) return {};
			return Hash;
		}
	} // namespace
	auto EncodeCacheManifest(const FCacheRecord& Record, uint64 MaximumBytes, uint64 MaximumWorkingBytes) -> std::expected<FSharedByteBuffer, FCacheError>
	try
	{
		auto TooLarge = [] { return std::unexpected(FCacheError{ECacheError::ValueTooLarge, "Manifest exceeds encoded or CB workspace budget."}); };
		if (auto Valid = Record.CheckLimits({.MaximumTotalBytes = UINT64_MAX}); !Valid) return std::unexpected(std::move(Valid.error()));
		const uint64 Estimate = 1024 + Record.GetValues().size() * 512 + 100 + Record.GetMeta().GetBytes().GetSize();
		if (MaximumBytes < 32 || Estimate > MaximumWorkingBytes / 8) return TooLarge();
		const auto Limits = ManifestLimits(MaximumBytes - 32);
		FCbWriter Root(Limits), Values(Limits);
		Root.AddString(std::string(Record.GetKey().GetBucket().ToString()), "Bucket");
		Root.AddBinary(HashBytes(Record.GetKey().GetHash()), "Key");
		Root.AddObject(Record.GetMeta(), "Meta");
		for (const auto& Item : Record.GetValues())
		{
			const auto Identity = BlobIdentity(Item.Value);
			FCbWriter Attachment(Limits), Value(Limits);
			Attachment.AddUInt64(Identity.Version, "Version");
			Attachment.AddString(std::string(Identity.Algorithm), "Algorithm");
			Attachment.AddString(std::string(Identity.Namespace), "Namespace");
			Attachment.AddBinary(HashBytes(Identity.Hash), "Hash");
			auto Ref = Attachment.SaveObject();
			if (!Ref) return TooLarge();
			Value.AddObjectId({Item.Id.GetBytes()}, "Id");
			Value.AddUInt64(Item.Value.GetRawSize(), "RawSize");
			Value.AddObject(std::move(*Ref), "Attachment");
			auto Object = Value.SaveObject();
			if (!Object || !Values.AddObject(std::move(*Object))) return TooLarge();
		}
		auto ValueArray = Values.SaveArray();
		if (!ValueArray || !Root.AddArray(std::move(*ValueArray), "Values")) return TooLarge();
		auto Object = Root.SaveObject();
		if (!Object) return TooLarge();
		FBinaryWriter W({.MaximumTotalBytes = MaximumBytes});
		W.Reserve(Object->GetBytes().GetSize() + 32);
		W.WriteHeader({0x4d424444, 3, 0});
		W.WriteBytes(Object->GetBytes().GetBytes());
		W.WriteHash128(FXxHash128::HashBuffer(W.GetBytes()));
		if (W.HasError()) return TooLarge();
		return FSharedByteBuffer::Take(W.TakeBytes());
	}
	catch (const std::bad_alloc&)
	{
		return std::unexpected(FCacheError{ECacheError::StorageFailure, "Manifest allocation failed."});
	}
	auto DecodeCacheManifest(const FCacheKey& ExpectedKey, FSharedByteBuffer Bytes, FCacheRecordLimits Limits, uint64 MaximumBytes) -> std::expected<FCacheRecord, FCacheError>
	try
	{
		auto Corrupt = [] { return std::unexpected(FCacheError{ECacheError::Corrupt, "Cache manifest is malformed or unsupported."}); };
		if (!ExpectedKey.IsValid() || Bytes.GetSize() < 49 || Bytes.GetSize() > MaximumBytes || Bytes.GetSize() > MaximumManifestBytes + 32) return Corrupt();
		const uint64 BodySize = Bytes.GetSize() - 16;
		FBinaryReader Trailer(Bytes.MakeView(BodySize, 16).GetBytes());
		FXxHash128 Hash;
		if (!Trailer.ReadHash128(Hash) || Hash != FXxHash128::HashBuffer(Bytes.MakeView(0, BodySize).GetBytes())) return Corrupt();
		FBinaryReader Header(Bytes.MakeView(0, 16).GetBytes());
		if (!Header.ReadAndValidateHeader(0x4d424444, 3, 0)) return Corrupt();
		auto Object = FCbObject::TryLoad(Bytes.MakeView(16, BodySize - 16), ManifestLimits(MaximumBytes));
		if (!Object) return Corrupt();
		const auto Root = Object->GetView();
		const auto Bucket = Root.Find("Bucket").AsString();
		const auto Key = ReadHash(Root.Find("Key"));
		const auto Values = Root.Find("Values").AsArray();
		const auto Meta = Root.Find("Meta");
		if (Root.Num() != 4 || !Bucket || *Bucket != ExpectedKey.GetBucket().ToString() || !Key || *Key != ExpectedKey.GetHash()
			|| !Meta.AsObject().IsValid() || !Values.IsValid() || Values.Num() > std::min<uint32>(Limits.MaximumValues, 4096)) return Corrupt();
		auto LoadedMeta = FCbObject::TryLoad(Meta.GetBytes(), {.MaximumEncodedBytes = Meta.GetBytes().GetSize()});
		if (!LoadedMeta) return Corrupt();
		FCacheRecordBuilder Builder(ExpectedKey, Limits);
		if (!Builder.SetMeta(std::move(*LoadedMeta))) return Corrupt();
		FValueId Previous;
		for (const auto& Field : Values.GetFields())
		{
			const auto Value = Field.AsObject();
			const auto Id = Value.Find("Id").AsObjectId();
			const auto Size = Value.Find("RawSize").AsUInt64();
			const auto Attachment = Value.Find("Attachment").AsObject();
			const auto RawHash = ReadHash(Attachment.Find("Hash"));
			if (Value.Num() != 3 || !Id || !Size || Attachment.Num() != 4 || !RawHash
				|| Attachment.Find("Version").AsUInt64() != FCacheBlobIdentity::Version || Attachment.Find("Algorithm").AsString() != FCacheBlobIdentity::Algorithm
				|| Attachment.Find("Namespace").AsString() != FCacheBlobIdentity::Namespace) return Corrupt();
			const auto ValueId = FValueId::FromBytes(Id->Bytes);
			if (ValueId.IsNull() || (!Previous.IsNull() && Previous >= ValueId)) return Corrupt();
			if (!Builder.AddValue(ValueId, FValue::FromReference(*RawHash, *Size))) return Corrupt();
			Previous = ValueId;
		}
		auto Record = std::move(Builder).Build();
		if (!Record) return Corrupt();
		return std::move(*Record);
	}
	catch (const std::bad_alloc&)
	{
		return std::unexpected(FCacheError{ECacheError::StorageFailure, "Manifest allocation failed."});
	}
} // namespace Durin::DerivedData::Private
