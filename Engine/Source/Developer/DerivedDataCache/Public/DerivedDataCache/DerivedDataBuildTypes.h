#pragma once

#include "DerivedDataCacheAPI.h"
#include "Hash/XxHash.h"

namespace Durin::DerivedData
{
	// Immutable named bytes exchanged with the derived-data cache.
	class FBuildValue
	{
	public:
		DERIVEDDATACACHE_API static auto FromOwned(
			std::string Name, std::vector<std::byte> Bytes) -> FBuildValue;

		auto GetName() const -> std::string_view { return Name; }
		// Reserved for content-addressed storage, request deduplication, and remote
		// execution protocols. Local execution does not currently consume the digest.
		auto GetContentIdentity() const -> const FXxHash128& { return ContentIdentity; }
		auto GetBytes() const -> std::span<const std::byte>
		{
			return Bytes ? std::span<const std::byte>(*Bytes) : std::span<const std::byte>();
		}
		auto GetSize() const -> uint64 { return Bytes ? Bytes->size() : 0; }
		auto IsValid() const -> bool { return !Name.empty() && Bytes != nullptr; }

	private:
		std::string Name;
		FXxHash128 ContentIdentity;
		std::shared_ptr<const std::vector<std::byte>> Bytes;
	};

	class FBuildKey
	{
	public:
		DERIVEDDATACACHE_API static auto FromString(
			std::string_view Value, std::string* OutError = nullptr) -> FBuildKey;
		auto IsValid() const -> bool { return Value.size() == 32; }
		auto ToString() const -> std::string_view { return Value; }
		auto operator==(const FBuildKey&) const -> bool = default;
	private:
		std::string Value;
	};

	// Identifies one registered Build capability by a stable, case-sensitive name.
	class FBuildFunctionName
	{
	public:
		DERIVEDDATACACHE_API static auto FromString(
			std::string_view Value, std::string* OutError = nullptr) -> FBuildFunctionName;
		auto IsValid() const -> bool { return !Value.empty(); }
		auto ToString() const -> std::string_view { return Value; }
		auto operator==(const FBuildFunctionName&) const -> bool = default;

	private:
		std::string Value;
	};

	struct FBuildPolicy
	{
		bool bQueryCache = true;
		bool bAllowLocalBuild = true;
		bool bStoreBuildResult = true;
		bool bRequireStoreSuccess = false;
		// Store-only cache warming may suppress returned bytes without changing
		// query, build, or persistence behavior.
		bool bReturnData = true;
	};

	enum class EBuildStatus : uint8 { CacheHit, Built, CacheMiss, Canceled, Failed };
	enum class EBuildFailurePhase : uint8
	{
		None, Request, FunctionLookup, CacheQuery, CachedValueValidation,
		LocalBuild, BuiltValueValidation, CacheStore
	};
	// Kept separate from status so future remote and shared-cache origins do not
	// require multiplying success states.
	enum class EBuildValueOrigin : uint8 { None, Cache, Local };

	struct FBuildPhaseDurations
	{
		uint64 CacheQueryNanoseconds = 0;
		uint64 CachedValueValidationNanoseconds = 0;
		uint64 LocalBuildNanoseconds = 0;
		uint64 BuiltValueValidationNanoseconds = 0;
		uint64 CacheStoreNanoseconds = 0;
	};

	struct FBuildOutput
	{
		EBuildStatus Status = EBuildStatus::Failed;
		EBuildFailurePhase FailurePhase = EBuildFailurePhase::None;
		EBuildValueOrigin Origin = EBuildValueOrigin::None;
		FBuildValue Value;
		std::string Diagnostic;
		std::string StoreDiagnostic;
		FBuildPhaseDurations PhaseDurations;
		// Execution markers support telemetry for failed and canceled requests,
		// where Status and FailurePhase alone do not describe work already done.
		bool bCacheQueried = false;
		bool bLocalBuildExecuted = false;
		auto Succeeded() const -> bool
		{
			return Status == EBuildStatus::CacheHit || Status == EBuildStatus::Built;
		}
	};

	class FBuildCancellationToken
	{
	public:
		FBuildCancellationToken() = default;
		explicit FBuildCancellationToken(std::function<bool()> InShouldCancel)
			: ShouldCancel(std::move(InShouldCancel)) {}
		auto IsCanceled() const -> bool { return ShouldCancel && ShouldCancel(); }
	private:
		std::function<bool()> ShouldCancel;
	};
}
