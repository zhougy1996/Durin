#pragma once
#include "CoreMinimal.h"
#include "DerivedDataBuildOutput.h"
#include "DerivedDataStatus.h"

namespace Durin::DerivedData
{
	class IRequestOwner;
	namespace Private
	{
		struct FCacheAccess;
	}
	enum class ECacheGetMode : uint8
	{
		FullRecord,
		MetadataOnly,
		SelectedValues
	};
	struct FCacheGetRequest
	{
		FCacheKey Key;
		FBuildOutputLimits OutputLimits;
		uint64 MaximumEncodedBytes = FCacheRecord::DefaultMaximumEncodedBytes;
		uint64 MaximumWorkingSetBytes = 4ull * 1024 * 1024 * 1024;
		ECacheGetMode Mode = ECacheGetMode::FullRecord;
		std::span<const FValueId> RequiredValues;
		std::string Name;
		uint64 UserData = 0;
	};
	struct FCachePutRequest
	{
		FCacheRecord Record;
		uint64 MaximumEncodedBytes = FCacheRecord::DefaultMaximumEncodedBytes;
		uint64 MaximumRawBytes = 2ull * 1024 * 1024 * 1024;
		uint64 MaximumWorkingSetBytes = 4ull * 1024 * 1024 * 1024;
		std::string Name;
		uint64 UserData = 0;
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
		FCacheKey Key;
		uint64 UserData = 0;
		EStatus Status = EStatus::Error;
	};
	using FOnCacheGetComplete = std::function<void(FCacheGetResponse)>;
	using FOnCachePutComplete = std::function<void(FCachePutResponse)>;
	// Backend-neutral requests complete once per item, including rejection and cancellation.
	// Callbacks may run inline or on another thread. Owner tracks callback retirement.
	class ICache
	{
	public:
		DERIVEDDATACACHE_API auto Get(std::span<const FCacheGetRequest> Requests, IRequestOwner& Owner, FOnCacheGetComplete Completion) -> void;
		DERIVEDDATACACHE_API auto Put(std::span<const FCachePutRequest> Requests, IRequestOwner& Owner, FOnCachePutComplete Completion) -> void;

	private:
		friend struct Private::FCacheAccess;
		explicit ICache(std::shared_ptr<Private::FCacheAccess> State)
			: State(std::move(State))
		{
		}
		std::shared_ptr<Private::FCacheAccess> State;
	};
	DERIVEDDATACACHE_API auto GetCache() -> ICache&;
}
