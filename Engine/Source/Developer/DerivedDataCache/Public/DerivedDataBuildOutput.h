#pragma once

#include "DerivedDataValue.h"
#include "Serialization/CompactBinary.h"

namespace Durin::DerivedData
{
	namespace Private { struct FBuildExecutionAccess; }
	class FBuildOutput;
	enum class EBuildMessageSeverity : uint8 { Note, Warning, Error };
	struct FBuildOutputMessage { EBuildMessageSeverity Severity = EBuildMessageSeverity::Note; std::string Text; };
	enum class EBuildLogSeverity : uint8 { Display, Warning, Error };
	struct FBuildOutputLog
	{
		std::string Category;
		EBuildLogSeverity Severity = EBuildLogSeverity::Display;
		std::string Text;
	};
	struct FBuildOutputMeta { FValueId Id; FCbObject Object; };
	DERIVEDDATACACHE_API auto MakeBuildMetadata(FSharedByteBuffer Payload) -> std::expected<FCbObject, std::string>;
	DERIVEDDATACACHE_API auto GetBuildMetadataPayload(const FBuildOutput& Output,
		FValueId Id = FValueId::FromName("Metadata")) -> FSharedByteBuffer;

	struct FBuildOutputLimits
	{
		uint64 MaximumTotalBytes = 2ull * 1024 * 1024 * 1024;
		uint64 MaximumMetadataBytes = 4ull * 1024 * 1024;
		uint32 MaximumValues = 4096;
		uint32 MaximumMetadata = 4096;
		uint32 MaximumMessages = 128;
		uint32 MaximumLogs = 128;
	};

	class FBuildOutput
	{
	public:
		struct FState;
		FBuildOutput() = default;
		auto IsValid() const -> bool { return State != nullptr; }
		DERIVEDDATACACHE_API auto HasError() const -> bool;
		DERIVEDDATACACHE_API auto HasLogs() const -> bool;
		auto SharesStateWith(const FBuildOutput& Other) const -> bool { return State && State == Other.State; }
		DERIVEDDATACACHE_API auto CheckLimits(FBuildOutputLimits Limits = {}) const -> std::expected<void, std::string>;
		DERIVEDDATACACHE_API auto GetSchema() const -> std::string_view;
		DERIVEDDATACACHE_API auto GetSchemaVersion() const -> uint32;
		DERIVEDDATACACHE_API auto GetValues() const -> std::span<const FValueWithId>;
		DERIVEDDATACACHE_API auto GetMetadata() const -> std::span<const FBuildOutputMeta>;
		DERIVEDDATACACHE_API auto GetMessages() const -> std::span<const FBuildOutputMessage>;
		DERIVEDDATACACHE_API auto GetLogs() const -> std::span<const FBuildOutputLog>;
		DERIVEDDATACACHE_API auto FindValue(FValueId Id) const -> const FValue*;
		DERIVEDDATACACHE_API auto FindMeta(FValueId Id) const -> FCbObjectView;
	private:
		friend class FBuildOutputBuilder;
		friend class FCacheRecord;
		std::shared_ptr<const FState> State;
	};

	class FBuildOutputBuilder
	{
	public:
		DERIVEDDATACACHE_API FBuildOutputBuilder(std::string Schema, uint32 SchemaVersion, FBuildOutputLimits Limits = {});
		DERIVEDDATACACHE_API ~FBuildOutputBuilder();
		FBuildOutputBuilder(FBuildOutputBuilder&&) noexcept = default;
		auto operator=(FBuildOutputBuilder&&) noexcept -> FBuildOutputBuilder& = default;
		FBuildOutputBuilder(const FBuildOutputBuilder&) = delete;
		auto operator=(const FBuildOutputBuilder&) -> FBuildOutputBuilder& = delete;
		DERIVEDDATACACHE_API auto AddValue(FValueId Id, FSharedByteBuffer Data) -> bool;
		DERIVEDDATACACHE_API auto AddMeta(FValueId Id, FCbObject Object) -> bool;
		DERIVEDDATACACHE_API auto AddMessage(EBuildMessageSeverity Severity, std::string Text) -> bool;
		DERIVEDDATACACHE_API auto AddLog(std::string Category, EBuildLogSeverity Severity,
			std::string Text) -> bool;
		DERIVEDDATACACHE_API auto Build() && -> std::expected<FBuildOutput, std::string>;
	private:
		friend class FBuildContext;
		friend class FCacheRecord;
		friend struct Private::FBuildExecutionAccess;
		struct FState;
		std::unique_ptr<FState> State;
	};

	class FCacheRecord
	{
	public:
		static constexpr uint64 DefaultMaximumEncodedBytes = 2ull * 1024 * 1024 * 1024 + 2ull * 1024 * 1024;
		FCacheRecord() = default;
		DERIVEDDATACACHE_API static auto FromOutput(const FCacheKey& Key, const FBuildOutput& Output,
			FBuildOutputLimits Limits = {}) -> std::expected<FCacheRecord, FCacheError>;
		DERIVEDDATACACHE_API auto ToOutput(const FCacheKey& ExpectedKey,
			FBuildOutputLimits Limits = {}) const -> std::expected<FBuildOutput, FCacheError>;
		DERIVEDDATACACHE_API auto Encode(uint64 MaximumBytes = DefaultMaximumEncodedBytes) const -> std::expected<FSharedByteBuffer, FCacheError>;
		DERIVEDDATACACHE_API static auto CompressEncoded(const FSharedByteBuffer& RawRecord,
			uint64 MaximumBytes = DefaultMaximumEncodedBytes) -> std::expected<FSharedByteBuffer, FCacheError>;
		DERIVEDDATACACHE_API static auto Decode(const FCacheKey& ExpectedKey, FSharedByteBuffer Bytes,
			FBuildOutputLimits Limits = {}, uint64 MaximumEncodedBytes = DefaultMaximumEncodedBytes) -> std::expected<FCacheRecord, FCacheError>;
		auto IsValid() const -> bool { return State != nullptr; }
		DERIVEDDATACACHE_API auto GetKey() const -> FCacheKey;
		DERIVEDDATACACHE_API auto GetSchema() const -> std::string_view;
		DERIVEDDATACACHE_API auto GetSchemaVersion() const -> uint32;
		DERIVEDDATACACHE_API auto GetMetadata() const -> std::span<const FBuildOutputMeta>;
		DERIVEDDATACACHE_API auto GetMessages() const -> std::span<const FBuildOutputMessage>;
		DERIVEDDATACACHE_API auto GetValues() const -> std::span<const FValueWithId>;
	private:
		struct FState;
		std::shared_ptr<const FState> State;
	};
}
