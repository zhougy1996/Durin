#pragma once

#include "DerivedDataCache/DerivedDataCache.h"

namespace Durin::DerivedData
{
	struct FBuildValue
	{
		std::string Id;
		FSharedByteBuffer Data;
	};

	// Mutable construction data. Publishing moves descriptors and retains only
	// immutable blocks; callers must not retain writable aliases to shared bytes.
	struct FBuildOutputData
	{
		std::string Schema;
		uint32 SchemaVersion = 0;
		FSharedByteBuffer Metadata;
		std::vector<FBuildValue> Values;
	};

	struct FBuildOutputLimits
	{
		uint64 MaximumTotalBytes = 2ull * 1024 * 1024 * 1024;
		uint64 MaximumMetadataBytes = 4ull * 1024 * 1024;
		uint32 MaximumValues = 4096;
	};

	// Family-neutral immutable representation. Family validators interpret schema
	// metadata and required values without resolving source or constructing products.
	class FBuildOutput
	{
	public:
		FBuildOutput() = default;
		DERIVEDDATACACHE_API static auto TryCreate(FBuildOutputData Data,
			FBuildOutputLimits Limits = {}) -> std::expected<FBuildOutput, std::string>;
		auto IsValid() const -> bool { return State != nullptr; }
		DERIVEDDATACACHE_API auto CheckLimits(FBuildOutputLimits Limits = {}) const -> std::expected<void, std::string>;
		auto GetSchema() const -> std::string_view { return State ? State->Schema : std::string_view{}; }
		auto GetSchemaVersion() const -> uint32 { return State ? State->SchemaVersion : 0; }
		auto GetMetadata() const -> FSharedByteBuffer { return State ? State->Metadata : FSharedByteBuffer{}; }
		auto GetValues() const -> std::span<const FBuildValue> { return State ? std::span(State->Values) : std::span<const FBuildValue>{}; }
		DERIVEDDATACACHE_API auto FindValue(std::string_view Id) const -> const FBuildValue*;
	private:
		friend class FCacheRecord;
		std::shared_ptr<const FBuildOutputData> State;
	};

	// Keyed persistence representation. Construction is optional cache work and
	// retains blocks rather than flattening or serializing a family product.
	class FCacheRecord
	{
	public:
		// Payload budget plus a bounded descriptor table and framing.
		static constexpr uint64 DefaultMaximumEncodedBytes = 2ull * 1024 * 1024 * 1024 + 2ull * 1024 * 1024;
		FCacheRecord() = default;
		DERIVEDDATACACHE_API static auto FromOutput(const FCacheKey& Key,
			const FBuildOutput& Output, FBuildOutputLimits Limits = {})
			-> std::expected<FCacheRecord, FCacheError>;
		DERIVEDDATACACHE_API auto ToOutput(const FCacheKey& ExpectedKey,
			FBuildOutputLimits Limits = {}) const -> std::expected<FBuildOutput, FCacheError>;
		// Optional persistence. Encoding never replaces the original output blocks.
		DERIVEDDATACACHE_API auto Encode(uint64 MaximumBytes = DefaultMaximumEncodedBytes) const
			-> std::expected<FSharedByteBuffer, FCacheError>;
		// A separate optional step: one Zstd frame containing a raw record.
		DERIVEDDATACACHE_API static auto CompressEncoded(const FSharedByteBuffer& RawRecord,
			uint64 MaximumBytes = DefaultMaximumEncodedBytes) -> std::expected<FSharedByteBuffer, FCacheError>;
		// Raw records retain views into Bytes after validating the complete envelope.
		// MaximumEncodedBytes also bounds the inflated envelope before allocation.
		DERIVEDDATACACHE_API static auto Decode(const FCacheKey& ExpectedKey, FSharedByteBuffer Bytes,
			FBuildOutputLimits Limits = {}, uint64 MaximumEncodedBytes = DefaultMaximumEncodedBytes)
			-> std::expected<FCacheRecord, FCacheError>;
		auto IsValid() const -> bool { return State != nullptr; }
		DERIVEDDATACACHE_API auto GetKey() const -> FCacheKey;
		DERIVEDDATACACHE_API auto GetSchema() const -> std::string_view;
		DERIVEDDATACACHE_API auto GetSchemaVersion() const -> uint32;
		DERIVEDDATACACHE_API auto GetMetadata() const -> FSharedByteBuffer;
		DERIVEDDATACACHE_API auto GetMetadataHash() const -> FXxHash128;
		DERIVEDDATACACHE_API auto GetValues() const -> std::span<const FBuildValue>;
		DERIVEDDATACACHE_API auto GetValueHashes() const -> std::span<const FXxHash128>;
	private:
		struct FState;
		std::shared_ptr<const FState> State;
	};
}
