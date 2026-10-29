#include "DerivedDataBuildOutput.h"
#include "BuildCacheRecordPrivate.h"
#include "DerivedDataBuildValidation.h"
#include "Serialization/BinaryFormat.h"

namespace Durin::DerivedData
{
	namespace
	{
		auto IsIdentifier(std::string_view Id) -> bool { return Private::IsBuildValueIdentifier(Id); }
		auto CanonicalObjectBytes(const FSharedByteBuffer& Bytes) -> uint64
		{
			// An owned CB object may be a named field retained from a manifest.
			// Its logical metadata budget uses the canonical unnamed root size.
			uint32 NameBytes = 0;
			if (!ReadLittleEndianAt(Bytes.GetBytes(), 1, NameBytes)) return Bytes.GetSize();
			return Bytes.GetSize() - NameBytes;
		}
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
		std::vector<FBuildOutputMeta> Observations;
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

	template<typename T>
	static auto Validate(const T& Data, FBuildOutputLimits Limits, bool RequireData = true)
		-> std::expected<void, std::string>
	{
		if (!IsIdentifier(Data.Schema) || !Data.SchemaVersion) return std::unexpected("Build output schema is invalid.");
		if (Data.Values.size() > std::min<uint32>(Limits.MaximumValues, 4096)
			|| Data.Metadata.size() > std::min<uint32>(Limits.MaximumMetadata, 4096)
			|| Data.Messages.size() > std::min<uint32>(Limits.MaximumMessages, 128))
			return std::unexpected("Build output table limit exceeded.");
		uint64 Total = 0, MetadataBytes = 0;
		auto Add = [&](uint64 Size) { if (Size > Limits.MaximumTotalBytes - Total) return false; Total += Size; return true; };
		// FValue computes its hash when constructed over immutable data. Limits and
		// output/record conversions must not rehash that same allocation.
		FValueId Previous;
		for (const auto& Item : Data.Values)
		{
			if (Item.Id.IsNull() || (!Previous.IsNull() && Previous >= Item.Id)
				|| (RequireData && !Item.Value.HasData()) || Item.Value.GetRawHash().IsZero()
				|| (Item.Value.HasData() && Item.Value.GetRawSize() != Item.Value.GetData().GetSize()))
				return std::unexpected("Build output value is invalid or duplicated.");
			Previous = Item.Id;
			if (!Add(Item.Value.GetRawSize())) return std::unexpected("Build output byte limit exceeded.");
		}
		Previous = {};
		for (const auto& Item : Data.Metadata)
		{
			const uint64 Size = CanonicalObjectBytes(Item.Object.GetBytes());
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
		if constexpr (requires { Data.Logs; })
		{
			if (Data.Observations.size() > std::min<uint32>(Limits.MaximumObservations, 128))
				return std::unexpected("Build observation count limit exceeded.");
			uint64 ObservationBytes = 0; Previous = {};
			for (const auto& Item : Data.Observations)
			{
				const auto Size = CanonicalObjectBytes(Item.Object.GetBytes());
				if (Item.Id.IsNull() || (!Previous.IsNull() && Previous >= Item.Id) || !Item.Object.IsValid()
					|| Size > std::min<uint64>(Limits.MaximumObservationBytes, 1024 * 1024) - ObservationBytes || !Add(Size))
					return std::unexpected("Build observation is invalid or exceeds its byte limit.");
				Previous = Item.Id; ObservationBytes += Size;
			}
			if (Data.Logs.size() > std::min<uint32>(Limits.MaximumLogs, 128))
				return std::unexpected("Build output log limit exceeded.");
			for (const auto& Log : Data.Logs)
			{
				if (!IsValidLog(Log) || !Add(Log.Category.size()) || !Add(Log.Text.size()))
					return std::unexpected("Build output log is invalid.");
				Error |= Log.Severity == EBuildLogSeverity::Error;
			}
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
	auto FBuildOutputBuilder::AddObservation(FValueId Id, FCbObject Object) -> bool
	{
		if (!State || State->Frozen) return false;
		if (Id.IsNull() || !Object.IsValid() || State->Data.Observations.size() >= std::min<uint32>(State->Limits.MaximumObservations, 128)
			|| std::ranges::any_of(State->Data.Observations, [&](const auto& Item) { return Item.Id == Id; }))
		{ State->Error = "Build observation is invalid, duplicated, or exceeds its count limit."; return false; }
		State->Data.Observations.push_back({Id, std::move(Object)}); return true;
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
		std::ranges::sort(State->Data.Observations, {}, &FBuildOutputMeta::Id);
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
	auto FBuildOutput::GetObservations() const -> std::span<const FBuildOutputMeta> { return State ? std::span(State->Observations) : std::span<const FBuildOutputMeta>{}; }
	auto FBuildOutput::GetMessages() const -> std::span<const FBuildOutputMessage> { return State ? std::span(State->Messages) : std::span<const FBuildOutputMessage>{}; }
	auto FBuildOutput::GetLogs() const -> std::span<const FBuildOutputLog> { return State ? std::span(State->Logs) : std::span<const FBuildOutputLog>{}; }
	auto FBuildOutput::FindValue(FValueId Id) const -> const FValue* { const auto Values = GetValues(); const auto It = std::ranges::lower_bound(Values, Id, {}, &FValueWithId::Id); return It != Values.end() && It->Id == Id ? &It->Value : nullptr; }
	auto FBuildOutput::FindMeta(FValueId Id) const -> FCbObjectView { const auto Meta = GetMetadata(); const auto It = std::ranges::lower_bound(Meta, Id, {}, &FBuildOutputMeta::Id); return It != Meta.end() && It->Id == Id ? It->Object.GetView() : FCbObjectView{}; }


	auto Private::FBuildCacheRecordAccess::Restore(FBuildCacheRecordView View, FBuildOutputLimits Limits) -> std::expected<FBuildOutput, FCacheError>
	{
		auto Data = std::make_shared<FBuildOutput::FState>();
		Data->Schema = std::move(View.Schema);
		Data->SchemaVersion = View.SchemaVersion;
		Data->Values.assign(View.Record.GetValues().begin(), View.Record.GetValues().end());
		Data->Metadata = std::move(View.Metadata);
		Data->Messages = std::move(View.Messages);
		if (auto Valid = Validate(*Data, Limits); !Valid) return std::unexpected(FCacheError{ECacheError::Corrupt, std::move(Valid.error())});
		FBuildOutput Result;
		Result.State = std::move(Data);
		return Result;
	}
}
