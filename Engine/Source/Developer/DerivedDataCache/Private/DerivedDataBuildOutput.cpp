#include "DerivedDataBuildOutput.h"
#include "DerivedDataBuildValidation.h"
#include "Serialization/BinaryFormat.h"
#include <zstd.h>

namespace Durin::DerivedData
{
	namespace
	{
		constexpr uint32 CacheRecordMagic = 0x52424444; // DDBR
		constexpr uint32 CacheRecordSchema = 2;
		auto IsIdentifier(std::string_view Id) -> bool { return Private::IsBuildValueIdentifier(Id); }
		auto IsValidMessage(const FBuildOutputMessage& Message) -> bool
		{
			return Message.Severity <= EBuildMessageSeverity::Error && Message.Text.size() <= 4096
				&& Message.Text.find('\0') == std::string::npos;
		}
		auto IsValidLog(const FBuildOutputLog& Log) -> bool
		{
			return Log.Severity <= EBuildLogSeverity::Error && !Log.Category.empty()
				&& Log.Category.size() <= 128 && Log.Text.size() <= 4096
				&& Log.Category.find('\0') == std::string::npos
				&& Log.Text.find('\0') == std::string::npos;
		}
	}

	struct FBuildOutput::FState
	{
		std::string Schema;
		uint32 SchemaVersion = 0;
		std::vector<FValueWithId> Values;
		std::vector<FBuildOutputMeta> Metadata;
		std::vector<FBuildOutputMessage> Messages;
		std::vector<FBuildOutputLog> Logs;
	};
	struct FBuildOutputBuilder::FState
	{
		FBuildOutput::FState Data;
		FBuildOutputLimits Limits;
		bool Frozen = false;
		std::string Error;
	};
	auto MakeBuildMetadata(FSharedByteBuffer Payload) -> std::expected<FCbObject, std::string>
	{ FCbWriter Writer; Writer.AddBinary(std::move(Payload), "Payload"); return Writer.SaveObject(); }
	auto GetBuildMetadataPayload(const FBuildOutput& Output, FValueId Id) -> FSharedByteBuffer
	{ const auto Field = Output.FindMeta(Id).Find("Payload"); const auto Payload = Field.AsBinary(); return Payload ? *Payload : FSharedByteBuffer{}; }

	static auto Validate(const FBuildOutput::FState& Data, FBuildOutputLimits Limits)
		-> std::expected<void, std::string>
	{
		if (!IsIdentifier(Data.Schema) || !Data.SchemaVersion) return std::unexpected("Build output schema is invalid.");
		if (Data.Values.size() > std::min<uint32>(Limits.MaximumValues, 4096)
			|| Data.Metadata.size() > std::min<uint32>(Limits.MaximumMetadata, 4096)
			|| Data.Messages.size() > std::min<uint32>(Limits.MaximumMessages, 128)
			|| Data.Logs.size() > std::min<uint32>(Limits.MaximumLogs, 128))
			return std::unexpected("Build output table limit exceeded.");
		uint64 Total = 0, MetadataBytes = 0;
		auto Add = [&](uint64 Size) { if (Size > Limits.MaximumTotalBytes - Total) return false; Total += Size; return true; };
		// FValue computes its hash when constructed over immutable data. Limits and
		// output/record conversions must not rehash that same allocation.
		FValueId Previous;
		for (const auto& Item : Data.Values)
		{
			if (Item.Id.IsNull() || (!Previous.IsNull() && Previous >= Item.Id)
				|| Item.Value.GetRawHash().IsZero() || Item.Value.GetRawSize() != Item.Value.GetData().GetSize())
				return std::unexpected("Build output value is invalid or duplicated.");
			Previous = Item.Id;
			if (!Add(Item.Value.GetRawSize())) return std::unexpected("Build output byte limit exceeded.");
		}
		Previous = {};
		for (const auto& Item : Data.Metadata)
		{
			const uint64 Size = Item.Object.GetBytes().GetSize();
			if (Item.Id.IsNull() || (!Previous.IsNull() && Previous >= Item.Id) || !Item.Object.IsValid())
				return std::unexpected("Build output metadata is invalid or duplicated.");
			Previous = Item.Id;
			if (Size > std::min<uint64>(Limits.MaximumMetadataBytes, 4ull * 1024 * 1024) - MetadataBytes || !Add(Size))
				return std::unexpected("Build output metadata limit exceeded.");
			MetadataBytes += Size;
		}
		bool Error = false;
		for (const auto& Message : Data.Messages)
		{
			if (!IsValidMessage(Message) || !Add(Message.Text.size())) return std::unexpected("Build output message is invalid.");
			Error |= Message.Severity == EBuildMessageSeverity::Error;
		}
		for (const auto& Log : Data.Logs)
		{
			if (!IsValidLog(Log) || !Add(Log.Category.size()) || !Add(Log.Text.size()))
				return std::unexpected("Build output log is invalid.");
			Error |= Log.Severity == EBuildLogSeverity::Error;
		}
		if (Error && !Data.Values.empty()) return std::unexpected("Build output with an error cannot contain values.");
		return {};
	}

	FBuildOutputBuilder::FBuildOutputBuilder(std::string Schema, uint32 SchemaVersion, FBuildOutputLimits Limits)
		: State(std::make_unique<FState>())
	{ State->Data.Schema = std::move(Schema); State->Data.SchemaVersion = SchemaVersion; State->Limits = Limits; }
	FBuildOutputBuilder::~FBuildOutputBuilder() = default;
	auto FBuildOutputBuilder::AddValue(FValueId Id, FSharedByteBuffer Data) -> bool
	{
		if (!State || State->Frozen) return false;
		if (Id.IsNull()) { State->Error = "Build output value ID is null."; return false; }
		if (State->Data.Values.size() >= std::min<uint32>(State->Limits.MaximumValues, 4096)) { State->Error = "Build output value limit exceeded."; return false; }
		if (std::ranges::any_of(State->Data.Values, [&](const auto& Item) { return Item.Id == Id; })) { State->Error = "Duplicate build output value ID."; return false; }
		State->Data.Values.push_back({Id, FValue(std::move(Data))}); return true;
	}
	auto FBuildOutputBuilder::AddMeta(FValueId Id, FCbObject Object) -> bool
	{
		if (!State || State->Frozen) return false;
		if (Id.IsNull() || !Object.IsValid()) { State->Error = "Build output metadata is invalid."; return false; }
		if (State->Data.Metadata.size() >= std::min<uint32>(State->Limits.MaximumMetadata, 4096)) { State->Error = "Build output metadata limit exceeded."; return false; }
		if (std::ranges::any_of(State->Data.Metadata, [&](const auto& Item) { return Item.Id == Id; })) { State->Error = "Duplicate build output metadata ID."; return false; }
		State->Data.Metadata.push_back({Id, std::move(Object)}); return true;
	}
	auto FBuildOutputBuilder::AddMessage(EBuildMessageSeverity Severity, std::string Text) -> bool
	{
		if (!State || State->Frozen) return false;
		FBuildOutputMessage Message{Severity, std::move(Text)};
		if (State->Data.Messages.size() >= std::min<uint32>(State->Limits.MaximumMessages, 128) || !IsValidMessage(Message))
		{ State->Error = "Build output message limit exceeded."; return false; }
		State->Data.Messages.push_back(std::move(Message)); return true;
	}
	auto FBuildOutputBuilder::AddLog(std::string Category, EBuildLogSeverity Severity,
		std::string Text) -> bool
	{
		if (!State || State->Frozen) return false;
		FBuildOutputLog Log{std::move(Category), Severity, std::move(Text)};
		if (State->Data.Logs.size() >= std::min<uint32>(State->Limits.MaximumLogs, 128)
			|| !IsValidLog(Log))
		{ State->Error = "Build output log limit exceeded."; return false; }
		State->Data.Logs.push_back(std::move(Log)); return true;
	}
	auto FBuildOutputBuilder::Build() && -> std::expected<FBuildOutput, std::string>
	{
		if (!State || State->Frozen) return std::unexpected("Build output builder is no longer mutable.");
		State->Frozen = true;
		if (!State->Error.empty())
		{
			State->Data.Values.clear();
			FBuildOutputMessage Message{EBuildMessageSeverity::Error, State->Error.substr(0, 4096)};
			const uint32 MaximumMessages = std::min<uint32>(State->Limits.MaximumMessages, 128);
			if (MaximumMessages == 0) return std::unexpected(std::move(State->Error));
			if (State->Data.Messages.size() >= MaximumMessages) State->Data.Messages.back() = std::move(Message);
			else State->Data.Messages.push_back(std::move(Message));
		}
		if (std::ranges::any_of(State->Data.Messages, [](const auto& M) { return M.Severity == EBuildMessageSeverity::Error; }))
			State->Data.Values.clear();
		if (std::ranges::any_of(State->Data.Logs, [](const auto& L) { return L.Severity == EBuildLogSeverity::Error; }))
			State->Data.Values.clear();
		std::ranges::sort(State->Data.Values, {}, &FValueWithId::Id);
		std::ranges::sort(State->Data.Metadata, {}, &FBuildOutputMeta::Id);
		if (auto Valid = Validate(State->Data, State->Limits); !Valid) return std::unexpected(std::move(Valid.error()));
		FBuildOutput Result; Result.State = std::make_shared<const FBuildOutput::FState>(std::move(State->Data)); return Result;
	}
	auto FBuildOutput::HasError() const -> bool
	{
		return State && (std::ranges::any_of(State->Messages,
			[](const auto& M) { return M.Severity == EBuildMessageSeverity::Error; })
			|| std::ranges::any_of(State->Logs,
				[](const auto& L) { return L.Severity == EBuildLogSeverity::Error; }));
	}
	auto FBuildOutput::HasLogs() const -> bool { return State && !State->Logs.empty(); }
	auto FBuildOutput::CheckLimits(FBuildOutputLimits Limits) const -> std::expected<void, std::string> { return State ? Validate(*State, Limits) : std::unexpected("Build output is empty."); }
	auto FBuildOutput::GetSchema() const -> std::string_view { return State ? State->Schema : std::string_view{}; }
	auto FBuildOutput::GetSchemaVersion() const -> uint32 { return State ? State->SchemaVersion : 0; }
	auto FBuildOutput::GetValues() const -> std::span<const FValueWithId> { return State ? std::span(State->Values) : std::span<const FValueWithId>{}; }
	auto FBuildOutput::GetMetadata() const -> std::span<const FBuildOutputMeta> { return State ? std::span(State->Metadata) : std::span<const FBuildOutputMeta>{}; }
	auto FBuildOutput::GetMessages() const -> std::span<const FBuildOutputMessage> { return State ? std::span(State->Messages) : std::span<const FBuildOutputMessage>{}; }
	auto FBuildOutput::GetLogs() const -> std::span<const FBuildOutputLog> { return State ? std::span(State->Logs) : std::span<const FBuildOutputLog>{}; }
	auto FBuildOutput::FindValue(FValueId Id) const -> const FValue* { const auto Values = GetValues(); const auto It = std::ranges::lower_bound(Values, Id, {}, &FValueWithId::Id); return It != Values.end() && It->Id == Id ? &It->Value : nullptr; }
	auto FBuildOutput::FindMeta(FValueId Id) const -> FCbObjectView { const auto Meta = GetMetadata(); const auto It = std::ranges::lower_bound(Meta, Id, {}, &FBuildOutputMeta::Id); return It != Meta.end() && It->Id == Id ? It->Object.GetView() : FCbObjectView{}; }

	struct FCacheRecord::FState { FCacheKey Key; FBuildOutput Output; };
	auto FCacheRecord::FromOutput(const FCacheKey& Key, const FBuildOutput& Output, FBuildOutputLimits Limits) -> std::expected<FCacheRecord, FCacheError>
	{
		if (!Key.IsValid() || !Output.IsValid()) return std::unexpected(FCacheError{ECacheError::InvalidRequest, "Cache record requires a key and output."});
		if (Output.HasLogs()) return std::unexpected(FCacheError{ECacheError::InvalidRequest,
			"Build outputs with transient logs cannot be cached."});
		if (auto Valid = Output.CheckLimits(Limits); !Valid) return std::unexpected(FCacheError{ECacheError::ValueTooLarge, std::move(Valid.error())});
		FCacheRecord Result; Result.State = std::make_shared<FState>(Key, Output); return Result;
	}
	auto FCacheRecord::ToOutput(const FCacheKey& ExpectedKey, FBuildOutputLimits Limits) const -> std::expected<FBuildOutput, FCacheError>
	{
		if (!State || State->Key != ExpectedKey) return std::unexpected(FCacheError{ECacheError::Corrupt, "Cache record key mismatch."});
		if (auto Valid = State->Output.CheckLimits(Limits); !Valid) return std::unexpected(FCacheError{ECacheError::Corrupt, std::move(Valid.error())});
		return State->Output;
	}
	auto FCacheRecord::Encode(uint64 MaximumBytes) const -> std::expected<FSharedByteBuffer, FCacheError>
	try
	{
		if (!State) return std::unexpected(FCacheError{ECacheError::InvalidRequest, "Cache record is empty."});
		const auto& Output = *State->Output.State;
		FBinaryWriter W({.MaximumTotalBytes = MaximumBytes}); W.WriteHeader({CacheRecordMagic, CacheRecordSchema, 0});
		W.WriteString(State->Key.GetBucket().ToString()); W.WriteHash128(State->Key.GetHash()); W.WriteString(Output.Schema); W.WriteU32(Output.SchemaVersion);
		W.WriteU32(uint32(Output.Values.size())); W.WriteU32(uint32(Output.Metadata.size())); W.WriteU32(uint32(Output.Messages.size()));
		for (const auto& Item : Output.Values) { W.WriteBytes(std::as_bytes(std::span(Item.Id.GetBytes()))); W.WriteU64(Item.Value.GetRawSize()); W.WriteHash128(Item.Value.GetRawHash()); }
		for (const auto& Item : Output.Metadata) { W.WriteBytes(std::as_bytes(std::span(Item.Id.GetBytes()))); W.WriteU64(Item.Object.GetBytes().GetSize()); W.WriteHash128(FXxHash128::HashBuffer(Item.Object.GetBytes().GetBytes())); }
		for (const auto& M : Output.Messages) { W.WriteU8(uint8(M.Severity)); W.WriteString(M.Text); }
		for (const auto& Item : Output.Values) W.WriteBytes(Item.Value.GetData().GetBytes());
		for (const auto& Item : Output.Metadata) W.WriteBytes(Item.Object.GetBytes().GetBytes());
		if (W.HasError()) return std::unexpected(FCacheError{ECacheError::ValueTooLarge, "Cache record encoding exceeds its byte budget."});
		const auto Hash = FXxHash128::HashBuffer(W.GetBytes()); W.WriteHash128(Hash);
		if (W.HasError()) return std::unexpected(FCacheError{ECacheError::ValueTooLarge, "Cache record trailer exceeds its byte budget."});
		return FSharedByteBuffer::Take(W.TakeBytes());
	}
	catch (const std::bad_alloc&) { return std::unexpected(FCacheError{ECacheError::StorageFailure, "Allocation"}); }

	auto FCacheRecord::CompressEncoded(const FSharedByteBuffer& Raw, uint64 MaximumBytes) -> std::expected<FSharedByteBuffer, FCacheError>
	try
	{
		FBinaryReader R(Raw.GetBytes()); if (!R.ReadAndValidateHeader(CacheRecordMagic, CacheRecordSchema, 0)) return std::unexpected(FCacheError{ECacheError::InvalidRequest, "Compression requires a raw cache record."});
		constexpr uint64 FrameBytes = 40; if (Raw.GetSize() > MaximumBytes || MaximumBytes <= FrameBytes) return std::unexpected(FCacheError{ECacheError::ValueTooLarge, "Cache record compression exceeds its byte budget."});
		FByteBuffer Compressed(std::min<uint64>(ZSTD_compressBound(Raw.size()), MaximumBytes - FrameBytes));
		const size_t Size = ZSTD_compress(Compressed.data(), Compressed.size(), Raw.data(), Raw.size(), 3);
		if (ZSTD_isError(Size)) return std::unexpected(FCacheError{ECacheError::StorageFailure, "Cache record compression failed."});
		Compressed.resize(Size); FBinaryWriter W({.MaximumTotalBytes = MaximumBytes}); W.WriteHeader({CacheRecordMagic, CacheRecordSchema, 1}); W.WriteU64(Raw.GetSize()); W.WriteBytes(Compressed); W.WriteHash128(FXxHash128::HashBuffer(W.GetBytes()));
		if (W.HasError()) return std::unexpected(FCacheError{ECacheError::ValueTooLarge, "Compressed cache record exceeds its byte budget."}); return FSharedByteBuffer::Take(W.TakeBytes());
	}
	catch (const std::bad_alloc&) { return std::unexpected(FCacheError{ECacheError::StorageFailure, "Allocation"}); }

	auto FCacheRecord::Decode(const FCacheKey& ExpectedKey, FSharedByteBuffer Bytes, FBuildOutputLimits Limits, uint64 MaximumBytes) -> std::expected<FCacheRecord, FCacheError>
	try
	{
		auto Corrupt = [] { return std::unexpected(FCacheError{ECacheError::Corrupt, "Cache record envelope is malformed or unsupported."}); };
		if (!ExpectedKey.IsValid()) return std::unexpected(FCacheError{ECacheError::InvalidRequest, "Cache record requires an action key."});
		if (Bytes.GetSize() > MaximumBytes || Bytes.GetSize() < 32) return Corrupt();
		const uint64 BodySize = Bytes.GetSize() - 16; FBinaryReader Trailer(Bytes.MakeView(BodySize, 16).GetBytes()); FXxHash128 Hash;
		if (!Trailer.ReadHash128(Hash) || Hash != FXxHash128::HashBuffer(Bytes.MakeView(0, BodySize).GetBytes())) return Corrupt();
		uint32 Format = 0; if (!ReadLittleEndianAt(Bytes.GetBytes(), 8, Format)) return Corrupt();
		if (Format == 1)
		{
			FBinaryReader R(Bytes.MakeView(0, BodySize).GetBytes(), {.MaximumTotalBytes = MaximumBytes}); uint64 Size = 0; FByteView Frame;
			if (!R.ReadAndValidateHeader(CacheRecordMagic, CacheRecordSchema, 1) || !R.ReadU64(Size) || Size > MaximumBytes || !R.ReadRegion(Frame, R.GetRemainingBytes(), MaximumBytes) || ZSTD_getFrameContentSize(Frame.data(), Frame.size()) != Size || ZSTD_findFrameCompressedSize(Frame.data(), Frame.size()) != Frame.size()) return Corrupt();
			FByteBuffer Raw(Size); if (ZSTD_decompress(Raw.data(), Raw.size(), Frame.data(), Frame.size()) != Size) return Corrupt(); return Decode(ExpectedKey, FSharedByteBuffer::Take(std::move(Raw)), Limits, MaximumBytes);
		}
		FBinaryReader R(Bytes.MakeView(0, BodySize).GetBytes(), {.MaximumTotalBytes = MaximumBytes}); std::string Bucket, Schema; FXxHash128 KeyHash; uint32 Version = 0, ValueCount = 0, MetaCount = 0, MessageCount = 0;
		if (!R.ReadAndValidateHeader(CacheRecordMagic, CacheRecordSchema, 0) || !R.ReadString(Bucket, FCacheBucket::MaximumNameLength) || !R.ReadHash128(KeyHash) || Bucket != ExpectedKey.GetBucket().ToString() || KeyHash != ExpectedKey.GetHash() || !R.ReadString(Schema, 96) || !R.ReadU32(Version) || !R.ReadU32(ValueCount) || !R.ReadU32(MetaCount) || !R.ReadU32(MessageCount) || ValueCount > Limits.MaximumValues || MetaCount > Limits.MaximumMetadata || MessageCount > Limits.MaximumMessages) return Corrupt();
		struct FDescriptor { FValueId Id; uint64 Size; FXxHash128 Hash; }; std::vector<FDescriptor> Values(ValueCount), Meta(MetaCount);
		auto ReadDescriptors = [&](auto& List) { for (auto& D : List) { FByteView Id; if (!R.ReadRegion(Id, 12, 12) || !R.ReadU64(D.Size) || !R.ReadHash128(D.Hash)) return false; std::array<uint8, 12> Raw{}; for (size_t I = 0; I < 12; ++I) Raw[I] = uint8(Id[I]); D.Id = FValueId::FromBytes(Raw); } return true; };
		if (!ReadDescriptors(Values) || !ReadDescriptors(Meta)) return Corrupt();
		std::vector<FBuildOutputMessage> Messages(MessageCount); for (auto& M : Messages) { uint8 S; if (!R.ReadU8(S) || !R.ReadString(M.Text, 4096)) return Corrupt(); M.Severity = EBuildMessageSeverity(S); }
		FBuildOutputBuilder Builder(std::move(Schema), Version, Limits); uint64 Offset = R.Tell();
		for (const auto& D : Values)
		{
			if (D.Size > BodySize - Offset) return Corrupt();
			auto Data = Bytes.MakeView(Offset, D.Size);
			if (!Builder.AddValue(D.Id, std::move(Data))) return Corrupt();
			// AddValue constructs FValue and hashes the payload once. Compare that
			// identity with the persisted descriptor instead of scanning it again.
			if (Builder.State->Data.Values.back().Value.GetRawHash() != D.Hash) return Corrupt();
			Offset += D.Size;
		}
		for (const auto& D : Meta) { if (D.Size > BodySize - Offset) return Corrupt(); auto Data = Bytes.MakeView(Offset, D.Size); if (FXxHash128::HashBuffer(Data.GetBytes()) != D.Hash) return Corrupt(); auto Object = FCbObject::TryLoad(Data); if (!Object || !Builder.AddMeta(D.Id, std::move(*Object))) return Corrupt(); Offset += D.Size; }
		if (Offset != BodySize) return Corrupt(); for (auto& M : Messages) if (!Builder.AddMessage(M.Severity, std::move(M.Text))) return Corrupt(); auto Output = std::move(Builder).Build(); if (!Output) return Corrupt(); return FromOutput(ExpectedKey, *Output, Limits);
	}
	catch (const std::bad_alloc&) { return std::unexpected(FCacheError{ECacheError::StorageFailure, "Allocation"}); }

	auto FCacheRecord::GetKey() const -> FCacheKey { return State ? State->Key : FCacheKey{}; }
	auto FCacheRecord::GetSchema() const -> std::string_view { return State ? State->Output.GetSchema() : std::string_view{}; }
	auto FCacheRecord::GetSchemaVersion() const -> uint32 { return State ? State->Output.GetSchemaVersion() : 0; }
	auto FCacheRecord::GetMetadata() const -> std::span<const FBuildOutputMeta> { return State ? State->Output.GetMetadata() : std::span<const FBuildOutputMeta>{}; }
	auto FCacheRecord::GetMessages() const -> std::span<const FBuildOutputMessage> { return State ? State->Output.GetMessages() : std::span<const FBuildOutputMessage>{}; }
	auto FCacheRecord::GetValues() const -> std::span<const FValueWithId> { return State ? State->Output.GetValues() : std::span<const FValueWithId>{}; }
}
