#pragma once
#include "CoreMinimal.h"
#include "DerivedDataValue.h"
#include "Serialization/CompactBinary.h"
namespace Durin::DerivedData
{
	// Logical limits for an opaque metadata object and value descriptors/resident data.
	struct FCacheRecordLimits
	{
		uint64 MaximumTotalBytes = 2ull * 1024 * 1024 * 1024;
		uint64 MaximumMetadataBytes = 8ull * 1024 * 1024;
		uint32 MaximumValues = 4096;
	};
	// Immutable cache data. The cache never interprets producer schemas or diagnostics.
	class FCacheRecord
	{
	public:
		FCacheRecord() = default;
		auto IsValid() const -> bool { return State != nullptr; }
		DERIVEDDATACACHE_API auto GetKey() const -> FCacheKey;
		DERIVEDDATACACHE_API auto GetMeta() const -> FCbObject;
		DERIVEDDATACACHE_API auto GetValues() const -> std::span<const FValueWithId>;
		DERIVEDDATACACHE_API auto FindValue(FValueId Id) const -> const FValue*;
		DERIVEDDATACACHE_API auto CheckLimits(FCacheRecordLimits Limits = {}, bool bRequireData = false) const -> std::expected<void, FCacheError>;
		DERIVEDDATACACHE_API auto RemoveValueData() const -> FCacheRecord;
		DERIVEDDATACACHE_API auto WithValues(std::span<const FValueWithId> Values) const -> std::expected<FCacheRecord, FCacheError>;
		DERIVEDDATACACHE_API auto WithSelectedValues(std::span<const FValueWithId> Values) const -> std::expected<FCacheRecord, FCacheError>;

	private:
		friend class FCacheRecordBuilder;
		struct FState;
		std::shared_ptr<const FState> State;
	};
	// Freezes a record after validating its key, metadata and unique value IDs.
	class FCacheRecordBuilder
	{
	public:
		DERIVEDDATACACHE_API explicit FCacheRecordBuilder(FCacheKey Key, FCacheRecordLimits Limits = {});
		DERIVEDDATACACHE_API ~FCacheRecordBuilder();
		FCacheRecordBuilder(FCacheRecordBuilder&&) noexcept = default;
		FCacheRecordBuilder(const FCacheRecordBuilder&) = delete;
		auto operator=(const FCacheRecordBuilder&) -> FCacheRecordBuilder& = delete;
		DERIVEDDATACACHE_API auto SetMeta(FCbObject Meta) -> bool;
		DERIVEDDATACACHE_API auto AddValue(FValueId Id, FValue Value) -> bool;
		DERIVEDDATACACHE_API auto Build() && -> std::expected<FCacheRecord, FCacheError>;

	private:
		struct FState;
		std::unique_ptr<FState> State;
	};
} // namespace Durin::DerivedData
