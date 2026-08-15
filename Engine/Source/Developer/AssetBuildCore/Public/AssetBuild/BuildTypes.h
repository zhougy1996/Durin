#pragma once

#include "AssetBuildCoreAPI.h"
#include "Hash/XxHash.h"

namespace Durin::Asset::Build
{
	// Immutable named bytes exchanged with the derived-data cache.
	class FBuildValue
	{
	public:
		ASSETBUILDCORE_API static auto FromOwned(
			std::string Name, std::vector<uint8> Bytes) -> FBuildValue;

		auto GetName() const -> std::string_view { return Name; }
		auto GetContentIdentity() const -> const FXxHash128& { return ContentIdentity; }
		auto GetBytes() const -> std::span<const uint8>
		{
			return Bytes ? std::span<const uint8>(*Bytes) : std::span<const uint8>();
		}
		auto GetSize() const -> uint64 { return Bytes ? Bytes->size() : 0; }
		auto IsValid() const -> bool { return !Name.empty() && Bytes != nullptr; }

	private:
		std::string Name;
		FXxHash128 ContentIdentity;
		std::shared_ptr<const std::vector<uint8>> Bytes;
	};

	// Explicit cache query/store policy for one operation.
	struct FBuildCachePolicy
	{
		bool bQueryCache = true;
		bool bStoreBuildResult = true;
		bool bRequireStoreSuccess = false;
	};

	class FBuildKey
	{
	public:
		ASSETBUILDCORE_API static auto FromString(
			std::string_view Value, std::string* OutError = nullptr) -> FBuildKey;
		auto IsValid() const -> bool { return Value.size() == 32; }
		auto ToString() const -> std::string_view { return Value; }
		auto operator==(const FBuildKey&) const -> bool = default;
	private:
		std::string Value;
	};

	struct FBuildFunctionIdentity
	{
		std::string Name;
		uint32 Version = 0;
		auto IsValid() const -> bool { return !Name.empty() && Version != 0; }
		auto operator==(const FBuildFunctionIdentity&) const -> bool = default;
	};

	struct FBuildPolicy
	{
		bool bQueryCache = true;
		bool bAllowLocalBuild = true;
		bool bStoreBuildResult = true;
		bool bRequireStoreSuccess = false;
		bool bReturnData = true;
	};

	enum class EBuildStatus : uint8 { CacheHit, Built, CacheMiss, Canceled, Failed };
	enum class EBuildFailurePhase : uint8
	{
		None, Request, FunctionLookup, CacheQuery, CachedValueValidation,
		LocalBuild, BuiltValueValidation, CacheStore
	};
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
