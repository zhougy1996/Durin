#pragma once
#include "CoreMinimal.h"
#include "DerivedDataCacheRecord.h"
#include "DerivedDataBuildOutput.h"
namespace Durin::DerivedData::Private
{
	// Build metadata is one opaque cache object; only this adapter interprets it.
	struct FBuildCacheRecordView
	{
		FCacheRecord Record;
		std::string Schema;
		uint32 SchemaVersion = 0;
		std::vector<FBuildOutputMeta> Metadata;
		std::vector<FBuildOutputMessage> Messages;
		auto GetSchema() const -> std::string_view { return Schema; }
		auto GetSchemaVersion() const -> uint32 { return SchemaVersion; }
		auto GetMetadata() const -> std::span<const FBuildOutputMeta> { return Metadata; }
		auto GetMessages() const -> std::span<const FBuildOutputMessage> { return Messages; }
		auto GetValues() const -> std::span<const FValueWithId> { return Record.GetValues(); }
		auto FindValue(FValueId Id) const -> const FValue* { return Record.FindValue(Id); }
		auto FindMeta(FValueId Id) const -> FCbObjectView
		{
			const auto It = std::ranges::lower_bound(Metadata, Id, {}, &FBuildOutputMeta::Id);
			return It != Metadata.end() && It->Id == Id ? It->Object.GetView() : FCbObjectView{};
		}
	};
	DERIVEDDATACACHE_API auto ReadBuildCacheRecord(const FCacheRecord& Record) -> std::expected<FBuildCacheRecordView, FCacheError>;
	DERIVEDDATACACHE_API auto MakeBuildCacheRecord(const FCacheKey& Key, const FBuildOutput& Output, FBuildOutputLimits Limits = {}) -> std::expected<FCacheRecord, FCacheError>;
	DERIVEDDATACACHE_API auto ReadBuildOutput(const FCacheKey& Key, const FCacheRecord& Record, FBuildOutputLimits Limits = {}) -> std::expected<FBuildOutput, FCacheError>;
	struct FBuildCacheRecordAccess
	{
		static auto Restore(FBuildCacheRecordView View, FBuildOutputLimits Limits) -> std::expected<FBuildOutput, FCacheError>;
	};
} // namespace Durin::DerivedData::Private

namespace Durin::DerivedData
{
	DERIVEDDATACACHE_API auto GetBuildMetadataPayload(const FCacheRecord& Record, FValueId Id = FValueId::FromName("Metadata")) -> FSharedByteBuffer;
}
