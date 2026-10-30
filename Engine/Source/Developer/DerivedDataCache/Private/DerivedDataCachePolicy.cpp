#include "DerivedDataCachePolicy.h"

namespace Durin::DerivedData
{
	namespace
	{
		constexpr uint8 PolicyMask = uint8(ECachePolicy::Query | ECachePolicy::SkipData);
		auto CheckPolicy(ECachePolicy Policy) -> void
		{
			requiref((uint8(Policy) & ~PolicyMask) == 0, "Unsupported cache policy flags.");
		}
	} // namespace

	FCacheRecordPolicy::FCacheRecordPolicy(ECachePolicy BasePolicy)
		: BasePolicy(BasePolicy)
		, RecordPolicy(BasePolicy & ECachePolicy::Query)
	{
		CheckPolicy(BasePolicy);
	}

	auto FCacheRecordPolicy::GetValuePolicy(const FValueId& Id) const -> ECachePolicy
	{
		const auto Policies = GetValuePolicies();
		const auto It = std::ranges::lower_bound(Policies, Id, {}, &FCacheValuePolicy::Id);
		return It != Policies.end() && It->Id == Id ? It->Policy : BasePolicy;
	}

	FCacheRecordPolicyBuilder::FCacheRecordPolicyBuilder(ECachePolicy BasePolicy)
		: BasePolicy(BasePolicy)
	{
		CheckPolicy(BasePolicy);
	}

	auto FCacheRecordPolicyBuilder::AddValuePolicy(const FCacheValuePolicy& Value) -> void
	{
		requiref(!Built, "Cache policy builder is no longer usable.");
		requiref(!Value.Id.IsNull(), "Cache value policy requires a valid Value ID.");
		CheckPolicy(Value.Policy);
		requiref(Values.size() < 4096, "Too many cache value policies.");
		Values.push_back(Value);
	}

	auto FCacheRecordPolicyBuilder::Build() -> FCacheRecordPolicy
	{
		requiref(!std::exchange(Built, true), "Cache policy builder is no longer usable.");
		std::ranges::sort(Values, {}, &FCacheValuePolicy::Id);
		requiref(std::adjacent_find(Values.begin(), Values.end(), [](const auto& A, const auto& B) {
					 return A.Id == B.Id;
				 }) == Values.end(),
				 "Duplicate cache value policy.");
		std::erase_if(Values, [&](const auto& Value) { return Value.Policy == BasePolicy; });
		FCacheRecordPolicy Result(BasePolicy);
		for (const auto& Value : Values)
			Result.RecordPolicy = Result.RecordPolicy | (Value.Policy & ECachePolicy::Query);
		if (!Values.empty()) Result.Values = std::make_shared<const std::vector<FCacheValuePolicy>>(std::move(Values));
		return Result;
	}
} // namespace Durin::DerivedData
