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
			EBuildExecutionFlags ExecutionFlags, std::string Diagnostic = {}, std::optional<FBuildOutput> DiagnosticOutput = {}) -> FBuildCompleteParams;
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

	// Supported UE policy subset: local builds and a single cache store.
	enum class EBuildPolicy : uint8
	{
		None = 0,
		BuildLocal = 1 << 0,
		Build = BuildLocal,
		CacheQuery = 1 << 2,
		CacheStoreOnBuild = 1 << 4,
		CacheStore = CacheStoreOnBuild,
		Cache = (1 << 2) | (1 << 4),
		Default = (1 << 0) | (1 << 2) | (1 << 4),
	};
	constexpr auto operator|(EBuildPolicy A, EBuildPolicy B) -> EBuildPolicy
	{ return EBuildPolicy(uint8(A) | uint8(B)); }
	constexpr auto operator&(EBuildPolicy A, EBuildPolicy B) -> EBuildPolicy
	{ return EBuildPolicy(uint8(A) & uint8(B)); }
	constexpr auto operator~(EBuildPolicy Value) -> EBuildPolicy
	{ return EBuildPolicy(~uint8(Value)); }
	// Uniform policy; unsupported remote or per-value behavior is not advertised.
	class FBuildPolicy
	{
	public:
		constexpr FBuildPolicy(EBuildPolicy Policy = EBuildPolicy::Default) : Policy(Policy) {}
		constexpr auto GetDefaultPolicy() const -> EBuildPolicy { return Policy; }
		constexpr auto GetCombinedPolicy() const -> EBuildPolicy { return Policy; }
		constexpr auto Has(EBuildPolicy Flag) const -> bool { return (Policy & Flag) == Flag; }
	private:
		EBuildPolicy Policy;
	};
	// Product validation is independent of execution and cache policy.
	struct FBuildInputLimits
	{
		uint64 MaximumTotalBytes = 2ull * 1024 * 1024 * 1024;
		uint64 MaximumMetadataBytes = 4ull * 1024 * 1024;
		uint32 MaximumValues = 4096;
	};
	struct FBuildRequestLimits
	{
		FBuildInputLimits Input;
		FBuildOutputLimits Output;
	};
	class IBuildScheduler;
	struct FBuildServiceOptions
	{
		std::shared_ptr<ICache> Cache;
		// Applies to service-owned cache dispatchers.
		FCacheDispatchLimits CacheDispatchLimits;
		FCacheResourceLimits CacheLimits;
	};
}
