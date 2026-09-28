#pragma once
#include "DerivedDataBuildFunction.h"

namespace Durin::DerivedData
{
	enum class EStatus : uint8 { Ok, Error, Canceled };
	enum class EBuildStatus : uint32
	{
		None = 0, CacheKey = 1 << 0, CacheQuery = 1 << 1,
		CacheQueryHit = 1 << 2, BuildLocal = 1 << 3,
		CacheStore = 1 << 4, CacheStoreHit = 1 << 5,
	};
	constexpr auto operator|(EBuildStatus A, EBuildStatus B) -> EBuildStatus
	{ return EBuildStatus(uint32(A) | uint32(B)); }
	constexpr auto operator|=(EBuildStatus& A, EBuildStatus B) -> EBuildStatus&
	{ return A = A | B; }
	constexpr auto HasBuildStatus(EBuildStatus Value, EBuildStatus Flag) -> bool
	{ return (uint32(Value) & uint32(Flag)) != 0; }

	struct FBuildMetric { std::string Name; uint64 Value = 0; };
	struct FBuildDiagnostic
	{
		EBuildOperation Operation = EBuildOperation::Admission;
		FCacheError Error;
	};
	struct FBuildExecutionReport
	{
		std::vector<FBuildMetric> Metrics;
		std::vector<FBuildDiagnostic> Diagnostics;
		uint64 PersistenceNanoseconds = 0;
	};

	class FBuildCompleteParams
	{
	public:
		DERIVEDDATACACHE_API static auto Ok(FBuildOutput Output, FCacheKey Key,
			EBuildStatus BuildStatus, FBuildExecutionReport Report) -> FBuildCompleteParams;
		DERIVEDDATACACHE_API static auto Error(FBuildFailure Failure,
			std::optional<FCacheKey> Key, EBuildStatus BuildStatus,
			FBuildExecutionReport Report) -> FBuildCompleteParams;
		DERIVEDDATACACHE_API static auto Canceled(std::optional<FCacheKey> Key,
			EBuildStatus BuildStatus, FBuildExecutionReport Report) -> FBuildCompleteParams;
		auto GetStatus() const -> EStatus { return Status; }
		auto GetBuildStatus() const -> EBuildStatus { return BuildStatus; }
		auto GetCacheKey() const -> const FCacheKey* { return CacheKey ? &*CacheKey : nullptr; }
		auto GetOutput() const -> const FBuildOutput* { return Output ? &*Output : nullptr; }
		auto GetFailure() const -> const FBuildFailure* { return Failure ? &*Failure : nullptr; }
		auto GetReport() const -> const FBuildExecutionReport& { return Report; }
	private:
		EStatus Status = EStatus::Canceled;
		EBuildStatus BuildStatus = EBuildStatus::None;
		std::optional<FCacheKey> CacheKey;
		std::optional<FBuildOutput> Output;
		std::optional<FBuildFailure> Failure;
		FBuildExecutionReport Report;
	};

	struct FBuildPolicy
	{
		bool QueryCache = true, BuildLocal = true, StoreOnBuild = true;
		bool ForceBuild = false, ReturnData = true;
		FBuildOutputLimits InputLimits, OutputLimits, PersistenceLimits;
		uint64 MaximumEncodedBytes = FCacheRecord::DefaultMaximumEncodedBytes;
		uint64 MaximumWorkingSetBytes = std::numeric_limits<uint64>::max();
	};
	struct FBuildCacheOperations
	{
		std::function<FCacheGetResult(const FCacheGetRequest&)> Get;
		std::function<FCachePutResult(const FCachePutRequest&)> Put;
		std::function<std::expected<FCacheRecord, FCacheError>(const FCacheKey&, const FBuildOutput&, FBuildOutputLimits)> MakeRecord;
		std::function<std::expected<FSharedByteBuffer, FCacheError>(const FCacheRecord&, uint64)> Encode;
		std::function<std::expected<FSharedByteBuffer, FCacheError>(const FSharedByteBuffer&, uint64)> Compress;
	};
	using FBuildDiagnosticSink = std::function<void(const FBuildAction&, const FBuildDiagnostic&)>;
	struct FBuildServiceOptions
	{
		FBuildCacheOperations Cache;
		FBuildDiagnosticSink Diagnostics;
		FBuildMetricSink Metrics;
		bool CompressRecords = false;
	};
}
