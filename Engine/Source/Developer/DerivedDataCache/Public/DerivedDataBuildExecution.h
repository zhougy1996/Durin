#pragma once

#include "CoreMinimal.h"
#include "DerivedDataBuildFunction.h"
#include "DerivedDataCache/DerivedDataCache.h"

namespace Durin::DerivedData
{
	namespace Private { struct FBuildCompletionAccess; }

	enum class EStatus : uint8 { Ok, Error, Canceled };
	enum class EBuildStatus : uint32
	{
		None = 0, CacheQueryHit = 1 << 0, BuildLocal = 1 << 1,
	};
	constexpr auto operator|(EBuildStatus A, EBuildStatus B) -> EBuildStatus
	{ return EBuildStatus(uint32(A) | uint32(B)); }
	constexpr auto operator|=(EBuildStatus& A, EBuildStatus B) -> EBuildStatus&
	{ return A = A | B; }
	constexpr auto HasBuildStatus(EBuildStatus Value, EBuildStatus Flag) -> bool
	{ return (uint32(Value) & uint32(Flag)) != 0; }

	class FBuildCompleteParams
	{
	public:
		DERIVEDDATACACHE_API static auto Error(std::optional<FCacheKey> Key,
			EBuildStatus BuildStatus) -> FBuildCompleteParams;
		DERIVEDDATACACHE_API static auto Canceled(std::optional<FCacheKey> Key,
			EBuildStatus BuildStatus) -> FBuildCompleteParams;
		auto GetStatus() const -> EStatus { return Status; }
		auto GetBuildStatus() const -> EBuildStatus { return BuildStatus; }
		auto GetCacheKey() const -> const FCacheKey* { return CacheKey ? &*CacheKey : nullptr; }
		auto GetOutput() const -> const FBuildOutput* { return Output ? &*Output : nullptr; }
	private:
		friend struct Private::FBuildCompletionAccess;
		EStatus Status = EStatus::Canceled;
		EBuildStatus BuildStatus = EBuildStatus::None;
		std::optional<FCacheKey> CacheKey;
		std::optional<FBuildOutput> Output;
	};

	struct FBuildPolicy
	{
		bool QueryCache = true, BuildLocal = true, StoreOnBuild = true;
		bool ForceBuild = false;
		FBuildOutputLimits InputLimits, OutputLimits, PersistenceLimits;
		uint64 MaximumEncodedBytes = FCacheRecord::DefaultMaximumEncodedBytes;
		uint64 MaximumWorkingSetBytes = std::numeric_limits<uint64>::max();
	};
	struct FBuildServiceOptions
	{
		std::shared_ptr<ICache> Cache;
	};
}
