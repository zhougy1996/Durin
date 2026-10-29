#include "BuildCacheRecordPrivate.h"
#include "DerivedDataBuildValidation.h"
namespace Durin::DerivedData::Private
{
	auto MakeBuildCacheRecord(const FCacheKey& Key, const FBuildOutput& Output, FBuildOutputLimits Limits) -> std::expected<FCacheRecord, FCacheError>
	{
		if (!Key.IsValid() || !Output.IsValid() || Output.HasLogs()) return std::unexpected(FCacheError{ECacheError::InvalidRequest, "Cacheable build output requires a key and no transient logs."});
		if (auto Valid = Output.CheckLimits(Limits); !Valid) return std::unexpected(FCacheError{ECacheError::ValueTooLarge, std::move(Valid.error())});
		FCbLimits MetaLimits{.MaximumEncodedBytes = 8ull * 1024 * 1024, .MaximumDepth = 40, .MaximumFields = 700000};
		FCbWriter Root(MetaLimits), Metadata(MetaLimits), Messages(MetaLimits);
		Root.AddUInt64(1, "BuildOutput");
		Root.AddString(std::string(Output.GetSchema()), "Schema");
		Root.AddUInt64(Output.GetSchemaVersion(), "Version");
		for (const auto& Item : Output.GetMetadata())
		{
			FCbWriter Meta(MetaLimits);
			Meta.AddObjectId({Item.Id.GetBytes()}, "Id");
			Meta.AddObject(Item.Object, "Object");
			auto Object = Meta.SaveObject();
			if (!Object || !Metadata.AddObject(std::move(*Object))) return std::unexpected(FCacheError{ECacheError::ValueTooLarge, "Build cache metadata exceeds its bound."});
		}
		for (const auto& Item : Output.GetMessages())
		{
			FCbWriter Message(MetaLimits);
			Message.AddUInt64(uint8(Item.Severity), "Severity");
			Message.AddString(Item.Text, "Text");
			auto Object = Message.SaveObject();
			if (!Object || !Messages.AddObject(std::move(*Object))) return std::unexpected(FCacheError{ECacheError::ValueTooLarge, "Build cache messages exceed their bound."});
		}
		auto MetaArray = Metadata.SaveArray(), MessageArray = Messages.SaveArray();
		if (!MetaArray || !MessageArray) return std::unexpected(FCacheError{ECacheError::ValueTooLarge, "Build cache metadata encoding failed."});
		Root.AddArray(std::move(*MetaArray), "Metadata");
		Root.AddArray(std::move(*MessageArray), "Messages");
		auto Meta = Root.SaveObject();
		if (!Meta) return std::unexpected(FCacheError{ECacheError::ValueTooLarge, std::move(Meta.error())});
		FCacheRecordBuilder Record(Key, {.MaximumTotalBytes = UINT64_MAX});
		Record.SetMeta(std::move(*Meta));
		for (const auto& Value : Output.GetValues())
			Record.AddValue(Value.Id, Value.Value);
		return std::move(Record).Build();
	}
	auto ReadBuildCacheRecord(const FCacheRecord& Record) -> std::expected<FBuildCacheRecordView, FCacheError>
	{
		auto Corrupt = [] { return std::unexpected(FCacheError{ECacheError::Corrupt, "Cache record has invalid build metadata."}); };
		if (!Record.IsValid()) return Corrupt();
		const auto Meta = Record.GetMeta();
		const auto Root = Meta.GetView();
		const auto Schema = Root.Find("Schema").AsString();
		const auto Version = Root.Find("Version").AsUInt64();
		const auto Metadata = Root.Find("Metadata").AsArray(), Messages = Root.Find("Messages").AsArray();
		if (Root.Num() != 5 || Root.Find("BuildOutput").AsUInt64() != 1 || !Schema || !IsBuildValueIdentifier(*Schema) || !Version || !*Version || *Version > UINT32_MAX
			|| !Metadata.IsValid() || !Messages.IsValid() || Metadata.Num() > 4096 || Messages.Num() > 128) return Corrupt();
		FBuildCacheRecordView View{.Record = Record, .Schema = std::string(*Schema), .SchemaVersion = uint32(*Version)};
		FValueId Previous;
		for (const auto& Field : Metadata.GetFields())
		{
			const auto Entry = Field.AsObject();
			const auto Id = Entry.Find("Id").AsObjectId();
			const auto Object = Entry.Find("Object");
			if (Entry.Num() != 2 || !Id || !Object.AsObject().IsValid()) return Corrupt();
			const auto ValueId = FValueId::FromBytes(Id->Bytes);
			if (ValueId.IsNull() || (!Previous.IsNull() && Previous >= ValueId)) return Corrupt();
			auto Loaded = FCbObject::TryLoad(Object.GetBytes(), {.MaximumEncodedBytes = Object.GetBytes().GetSize()});
			if (!Loaded) return Corrupt();
			View.Metadata.push_back({ValueId, std::move(*Loaded)});
			Previous = ValueId;
		}
		for (const auto& Field : Messages.GetFields())
		{
			const auto Entry = Field.AsObject();
			const auto Severity = Entry.Find("Severity").AsUInt64();
			const auto Text = Entry.Find("Text").AsString();
			if (Entry.Num() != 2 || !Severity || *Severity > uint8(EBuildMessageSeverity::Error) || !Text || Text->size() > 4096 || Text->find('\0') != std::string_view::npos) return Corrupt();
			View.Messages.push_back({EBuildMessageSeverity(*Severity), std::string(*Text)});
		}
		return View;
	}
	auto ReadBuildOutput(const FCacheKey& Key, const FCacheRecord& Record, FBuildOutputLimits Limits) -> std::expected<FBuildOutput, FCacheError>
	{
		if (Record.GetKey() != Key) return std::unexpected(FCacheError{ECacheError::Corrupt, "Cache record key mismatch."});
		if (std::ranges::any_of(Record.GetValues(), [](const auto& Item) { return !Item.Value.HasData(); })) return std::unexpected(FCacheError{ECacheError::InvalidRequest, "Build output requires resident values."});
		auto View = ReadBuildCacheRecord(Record);
		if (!View) return std::unexpected(std::move(View.error()));
		return FBuildCacheRecordAccess::Restore(std::move(*View), Limits);
	}
} // namespace Durin::DerivedData::Private
namespace Durin::DerivedData
{
	auto GetBuildMetadataPayload(const FCacheRecord& Record, FValueId Id) -> FSharedByteBuffer
	{
		auto View = Private::ReadBuildCacheRecord(Record);
		return View ? View->FindMeta(Id).Find("Payload").AsBinary().value_or(FSharedByteBuffer{}) : FSharedByteBuffer{};
	}
} // namespace Durin::DerivedData
