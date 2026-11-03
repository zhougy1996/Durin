#pragma once
#include "CoreMinimal.h"
#include "DerivedDataCacheRecord.h"
#include "DerivedDataCachePolicy.h"
#include "DerivedDataStatus.h"

namespace Durin::DerivedData
{
	class IRequestOwner;
	struct FCacheGetResponse;
	struct FCachePutResponse;
	struct FCacheGetRequest
	{
		FCacheKey Key;
		FCacheRecordPolicy Policy;
		std::string Name;
		uint64 UserData = 0;
		DERIVEDDATACACHE_API auto MakeResponse(EStatus Status) const -> FCacheGetResponse;
	};
	struct FCachePutRequest
	{
		FCacheRecord Record;
		std::string Name;
		uint64 UserData = 0;
		DERIVEDDATACACHE_API auto MakeResponse(EStatus Status) const -> FCachePutResponse;
	};
	struct FCacheGetResponse
	{
		std::string Name;
		FCacheRecord Record;
		uint64 UserData = 0;
		EStatus Status = EStatus::Error;
	};
	struct FCachePutResponse
	{
		std::string Name;
		FCacheRecord Record;
		FCacheKey Key;
		uint64 UserData = 0;
		EStatus Status = EStatus::Error;
	};
	using FOnCacheGetComplete = TMoveOnlyFunction<void(FCacheGetResponse)>;
	using FOnCachePutComplete = TMoveOnlyFunction<void(FCachePutResponse)>;
	// Service/backend resource ceilings, never a per-request cache policy.
	struct FCacheResourceLimits
	{
		FCacheRecordLimits Record;
		uint64 MaximumEncodedBytes = 2ull * 1024 * 1024 * 1024 + 2ull * 1024 * 1024;
		uint64 MaximumWorkingSetBytes = 4ull * 1024 * 1024 * 1024;
	};
	// Backend-neutral requests complete once per item, including rejection and cancellation.
	// Callbacks may run inline or on another thread. Owner tracks callback retirement.
	class ICache
	{
	public:
		virtual ~ICache() = default;
		virtual auto Get(std::span<const FCacheGetRequest> Requests, IRequestOwner& Owner, FOnCacheGetComplete&& Completion) -> void = 0;
		virtual auto Put(std::span<const FCachePutRequest> Requests, IRequestOwner& Owner, FOnCachePutComplete&& Completion) -> void = 0;
	};
	DERIVEDDATACACHE_API auto GetCache() -> ICache&;
}
