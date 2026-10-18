#pragma once

#include "CoreMinimal.h"
#include "DerivedDataStatus.h"
#include "DerivedDataBuildFunction.h"
#include "DerivedDataCache/DerivedDataCache.h"

namespace Durin::DerivedData
{
	namespace Private { struct FBuildCompletionAccess; }

	// Execution facts are independent of the request completion status (EStatus).
	enum class EBuildExecutionFlags : uint32
	{
		None = 0, CacheQueryHit = 1 << 0, BuildLocal = 1 << 1,
	};
	constexpr auto operator|(EBuildExecutionFlags A, EBuildExecutionFlags B) -> EBuildExecutionFlags
	{ return EBuildExecutionFlags(uint32(A) | uint32(B)); }
	constexpr auto operator|=(EBuildExecutionFlags& A, EBuildExecutionFlags B) -> EBuildExecutionFlags&
	{ return A = A | B; }
	constexpr auto HasBuildExecutionFlag(EBuildExecutionFlags Value, EBuildExecutionFlags Flag) -> bool
	{ return (uint32(Value) & uint32(Flag)) != 0; }

	class FBuildCompleteParams
	{
	public:
		DERIVEDDATACACHE_API static auto Error(std::optional<FCacheKey> Key,
			EBuildExecutionFlags ExecutionFlags, std::string Diagnostic = {}) -> FBuildCompleteParams;
		DERIVEDDATACACHE_API static auto Canceled(std::optional<FCacheKey> Key,
			EBuildExecutionFlags ExecutionFlags) -> FBuildCompleteParams;
		auto GetStatus() const -> EStatus { return Status; }
		auto GetExecutionFlags() const -> EBuildExecutionFlags { return ExecutionFlags; }
		auto GetCacheKey() const -> const FCacheKey* { return CacheKey ? &*CacheKey : nullptr; }
		auto GetOutput() const -> const FBuildOutput* { return Output ? &*Output : nullptr; }
		// Execution diagnostics are completion-only and never enter a cache record.
		auto GetDiagnostic() const -> std::string_view { return Diagnostic; }
	private:
		friend struct Private::FBuildCompletionAccess;
		EStatus Status = EStatus::Canceled;
		EBuildExecutionFlags ExecutionFlags = EBuildExecutionFlags::None;
		std::optional<FCacheKey> CacheKey;
		std::optional<FBuildOutput> Output;
		std::string Diagnostic;
	};

	enum class EBuildResultDelivery : uint8 { AfterPersistence, BeforePersistence };

	struct FBuildPolicy
	{
		bool QueryCache = true, BuildLocal = true, StoreOnBuild = true;
		bool ForceBuild = false;
		// Early notification leaves persistence tracked by request/owner/session waits.
		EBuildResultDelivery ResultDelivery = EBuildResultDelivery::AfterPersistence;
		FBuildOutputLimits InputLimits, OutputLimits, PersistenceLimits;
		uint64 MaximumEncodedBytes = FCacheRecord::DefaultMaximumEncodedBytes;
		// Cache payload/codec workspace only; producer reservations belong to the asset scheduler.
		uint64 MaximumWorkingSetBytes = std::numeric_limits<uint64>::max();
	};
	class IAsyncCache;
	class IBuildScheduler;
	struct FBuildServiceOptions
	{
		std::shared_ptr<ICache> Cache;
		std::shared_ptr<IAsyncCache> AsyncCache;
		// Applies to service-owned dispatchers; injected AsyncCache owns its policy.
		FCacheDispatchLimits CacheDispatchLimits;
	};
}
