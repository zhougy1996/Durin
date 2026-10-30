#pragma once

#include "CoreMinimal.h"
#include "DerivedDataValue.h"

namespace Durin::DerivedData
{
	// Supported read-policy subset for the local record cache.
	enum class ECachePolicy : uint8
	{
		None = 0,
		QueryLocal = 1 << 0,
		Query = QueryLocal,
		Default = Query,
		SkipData = 1 << 4,
	};
	constexpr auto operator|(ECachePolicy A, ECachePolicy B) -> ECachePolicy
	{
		return ECachePolicy(uint8(A) | uint8(B));
	}
	constexpr auto operator&(ECachePolicy A, ECachePolicy B) -> ECachePolicy
	{
		return ECachePolicy(uint8(A) & uint8(B));
	}
	constexpr auto HasCachePolicy(ECachePolicy Policy, ECachePolicy Flag) -> bool
	{
		return (Policy & Flag) == Flag;
	}

	struct FCacheValuePolicy
	{
		FValueId Id;
		ECachePolicy Policy = ECachePolicy::Default;
		auto operator==(const FCacheValuePolicy&) const -> bool = default;
	};

	// Immutable default policy and sorted, owned per-value overrides.
	class FCacheRecordPolicy
	{
	public:
		DERIVEDDATACACHE_API FCacheRecordPolicy(ECachePolicy BasePolicy = ECachePolicy::Default);
		auto GetBasePolicy() const -> ECachePolicy { return BasePolicy; }
		auto GetRecordPolicy() const -> ECachePolicy { return RecordPolicy; }
		DERIVEDDATACACHE_API auto GetValuePolicy(const FValueId& Id) const -> ECachePolicy;
		auto GetValuePolicies() const -> std::span<const FCacheValuePolicy>
		{
			return Values ? std::span<const FCacheValuePolicy>(*Values) : std::span<const FCacheValuePolicy>{};
		}
		auto IsUniform() const -> bool { return !Values; }
		auto IsDefault() const -> bool { return IsUniform() && BasePolicy == ECachePolicy::Default; }

	private:
		friend class FCacheRecordPolicyBuilder;
		ECachePolicy BasePolicy, RecordPolicy;
		std::shared_ptr<const std::vector<FCacheValuePolicy>> Values;
	};

	// Invalid flags, null/duplicate IDs, or reuse after Build are programming errors.
	class FCacheRecordPolicyBuilder
	{
	public:
		DERIVEDDATACACHE_API FCacheRecordPolicyBuilder(ECachePolicy BasePolicy = ECachePolicy::Default);
		FCacheRecordPolicyBuilder(const FCacheRecordPolicyBuilder&) = delete;
		FCacheRecordPolicyBuilder(FCacheRecordPolicyBuilder&&) noexcept = default;
		auto operator=(FCacheRecordPolicyBuilder&&) noexcept -> FCacheRecordPolicyBuilder& = default;
		DERIVEDDATACACHE_API auto AddValuePolicy(const FCacheValuePolicy& Value) -> void;
		auto AddValuePolicy(const FValueId& Id, ECachePolicy Policy) -> void { AddValuePolicy({Id, Policy}); }
		DERIVEDDATACACHE_API auto Build() -> FCacheRecordPolicy;

	private:
		ECachePolicy BasePolicy;
		std::vector<FCacheValuePolicy> Values;
		bool Built = false;
	};
} // namespace Durin::DerivedData
