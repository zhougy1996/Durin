#pragma once
#include "DerivedDataBuildFunction.h"

namespace Durin::DerivedData
{
	inline auto IsBuildCancelled(const FBuildResult& Result) -> bool
	{
		return !Result && Result.error().Category == EBuildErrorCategory::Cancelled;
	}
	struct FBuildRequestPolicy
	{
		bool ReadCache = true, WriteCache = true, ForceRebuild = false, Compress = false;
		FBuildOutputLimits InputLimits, OutputLimits, PersistenceLimits;
		uint64 MaximumEncodedBytes = FCacheRecord::DefaultMaximumEncodedBytes;
		uint64 MaximumWorkingSetBytes = std::numeric_limits<uint64>::max();
	};

	// Optional service overrides, including independently injectable persistence
	// failures. Empty operations use the production codec/backend. Borrowed inline.
	struct FBuildCacheOperations
	{
		std::function<FCacheGetResult(const FCacheGetRequest&)> Get;
		std::function<FCachePutResult(const FCachePutRequest&)> Put;
		std::function<std::expected<FCacheRecord, FCacheError>(const FCacheKey&, const FBuildOutput&, FBuildOutputLimits)> MakeRecord;
		std::function<std::expected<FSharedByteBuffer, FCacheError>(const FCacheRecord&, uint64)> Encode;
		std::function<std::expected<FSharedByteBuffer, FCacheError>(const FSharedByteBuffer&, uint64)> Compress;
	};
	struct FBuildRunObserver
	{
		std::function<void(EBuildSessionPhase)> OnPhase;
		std::function<void(const FBuildAction&, EBuildSessionPhase, const FCacheError&)> OnCacheIssue;
		std::function<void()> OnCacheHit;
		FBuildMetricObserver OnMetric;
		std::function<void(const FBuildAction&)> OnAction;
	};

	// Shared non-template inline execution. Never queues a task or publishes objects.
	// Observer callbacks run inline and must not throw. Session admission/dispatch
	// wraps this same path; this function itself owns no asynchronous lifecycle.
	DERIVEDDATACACHE_API auto ExecuteBuildRequest(const FBuildDefinition& Definition,
		const FBuildRegistrySnapshot& Registry, const IBuildInputResolver& Resolver,
		const FBuildRequestPolicy& Policy = {}, const FBuildCancellation& Cancel = {},
		const FBuildCacheOperations& Cache = {}, const FBuildRunObserver& Observer = {}) -> FBuildResult;
}
