#include "DerivedDataCacheRecord.h"
#include "Serialization/BinaryFormat.h"
namespace Durin::DerivedData
{
	struct FCacheRecord::FState
	{
		FCacheKey Key;
		FCbObject Meta;
		std::vector<FValueWithId> Values;
	};
	struct FCacheRecordBuilder::FState
	{
		std::shared_ptr<FCacheRecord::FState> Data = std::make_shared<FCacheRecord::FState>();
		FCacheRecordLimits Limits;
		bool Failed = false;
	};
	FCacheRecordBuilder::FCacheRecordBuilder(FCacheKey Key, FCacheRecordLimits Limits)
		: State(std::make_unique<FState>())
	{
		State->Data->Key = Key;
		State->Limits = Limits;
	}
	FCacheRecordBuilder::~FCacheRecordBuilder() = default;
	auto FCacheRecordBuilder::SetMeta(FCbObject Meta) -> bool
	{
		if (!State) return false;
		if (!Meta.IsValid())
		{
			State->Failed = true;
			return false;
		}
		State->Data->Meta = std::move(Meta);
		return true;
	}
	auto FCacheRecordBuilder::AddValue(FValueId Id, FValue Value) -> bool
	{
		if (!State) return false;
		if (Id.IsNull() || State->Data->Values.size() >= State->Limits.MaximumValues)
		{
			State->Failed = true;
			return false;
		}
		State->Data->Values.push_back({Id, std::move(Value)});
		return true;
	}
	auto FCacheRecordBuilder::Build() && -> std::expected<FCacheRecord, FCacheError>
	{
		auto Builder = std::move(State);
		if (!Builder || Builder->Failed) return std::unexpected(FCacheError{ECacheError::InvalidRequest, "Cache record builder is invalid or consumed."});
		if (!Builder->Data->Meta.IsValid())
		{
			FCbWriter Meta;
			auto Empty = Meta.SaveObject();
			if (!Empty) return std::unexpected(FCacheError{ECacheError::InvalidRequest, Empty.error()});
			Builder->Data->Meta = std::move(*Empty);
		}
		std::ranges::sort(Builder->Data->Values, {}, &FValueWithId::Id);
		FCacheRecord Record;
		Record.State = std::move(Builder->Data);
		if (auto Valid = Record.CheckLimits(Builder->Limits); !Valid) return std::unexpected(std::move(Valid.error()));
		return Record;
	}
	auto FCacheRecord::CheckLimits(FCacheRecordLimits Limits, bool bRequireData) const -> std::expected<void, FCacheError>
	{
		auto Invalid = [] { return std::unexpected(FCacheError{ECacheError::InvalidRequest, "Cache record key, metadata or value table is invalid."}); };
		auto TooLarge = [] { return std::unexpected(FCacheError{ECacheError::ValueTooLarge, "Cache record exceeds logical limits."}); };
		if (!State || !State->Key.IsValid() || !State->Meta.IsValid()) return Invalid();
		if (State->Values.size() > std::min<uint32>(Limits.MaximumValues, 4096)) return TooLarge();
		auto MetaBytes = State->Meta.GetBytes().GetSize();
		uint32 NameBytes = 0;
		if (ReadLittleEndianAt(State->Meta.GetBytes().GetBytes(), 1, NameBytes)) MetaBytes -= NameBytes;
		if (MetaBytes > Limits.MaximumMetadataBytes || MetaBytes > Limits.MaximumTotalBytes) return TooLarge();
		uint64 Total = MetaBytes;
		FValueId Previous;
		for (const auto& Item : State->Values)
		{
			if (Item.Id.IsNull() || (!Previous.IsNull() && Previous >= Item.Id) || Item.Value.GetRawHash().IsZero()
				|| (bRequireData && !Item.Value.HasData()) || (Item.Value.HasData() && Item.Value.GetRawSize() != Item.Value.GetData().GetSize())) return Invalid();
			if (Item.Value.GetRawSize() > Limits.MaximumTotalBytes - Total) return TooLarge();
			Total += Item.Value.GetRawSize();
			Previous = Item.Id;
		}
		return {};
	}
	auto FCacheRecord::GetKey() const -> FCacheKey { return State ? State->Key : FCacheKey{}; }
	auto FCacheRecord::GetMeta() const -> FCbObject { return State ? State->Meta : FCbObject{}; }
	auto FCacheRecord::GetValues() const -> std::span<const FValueWithId> { return State ? std::span(State->Values) : std::span<const FValueWithId>{}; }
	auto FCacheRecord::FindValue(FValueId Id) const -> const FValue*
	{
		const auto Values = GetValues();
		const auto It = std::ranges::lower_bound(Values, Id, {}, &FValueWithId::Id);
		return It != Values.end() && It->Id == Id ? &It->Value : nullptr;
	}
	auto FCacheRecord::RemoveValueData() const -> FCacheRecord
	{
		if (!State) return {};
		auto Data = std::make_shared<FState>(*State);
		for (auto& Item : Data->Values)
			Item.Value = Item.Value.RemoveData();
		FCacheRecord Result;
		Result.State = std::move(Data);
		return Result;
	}
	auto FCacheRecord::WithValues(std::span<const FValueWithId> Values) const -> std::expected<FCacheRecord, FCacheError>
	{
		if (!State || Values.size() != State->Values.size())
			return std::unexpected(FCacheError{ECacheError::InvalidRequest, "Record resolution requires all values."});
		return WithSelectedValues(Values);
	}
	auto FCacheRecord::WithSelectedValues(std::span<const FValueWithId> Values) const -> std::expected<FCacheRecord, FCacheError>
	{
		if (!State) return std::unexpected(FCacheError{ECacheError::InvalidRequest, "Record is empty."});
		auto Data = std::make_shared<FState>(*State);
		FValueId Previous;
		for (const auto& Actual : Values)
		{
			const auto It = std::ranges::lower_bound(Data->Values, Actual.Id, {}, &FValueWithId::Id);
			if (Actual.Id.IsNull() || (!Previous.IsNull() && Previous >= Actual.Id) || It == Data->Values.end()
				|| It->Id != Actual.Id || !Actual.Value.HasData() || It->Value.GetRawHash() != Actual.Value.GetRawHash()
				|| It->Value.GetRawSize() != Actual.Value.GetRawSize())
				return std::unexpected(FCacheError{ECacheError::Corrupt, "Resolved value identity mismatch."});
			Previous = Actual.Id;
			It->Value = Actual.Value;
		}
		FCacheRecord Record;
		Record.State = std::move(Data);
		return Record;
	}
} // namespace Durin::DerivedData
