#include "AssetTools/MutationTesting.h"
#include "AssetMutationStagingInternal.h"

namespace Durin
{
	namespace
	{
		struct FRelocationFailureInjection
		{
			std::map<EAssetRelocationFailurePoint, uint32> RemainingOccurrences;
		};

		auto GetRelocationFailureInjection() -> FRelocationFailureInjection&
		{
			static FRelocationFailureInjection Injection;
			return Injection;
		}
	}

	auto SetAssetRelocationFailurePointForTesting(
		EAssetRelocationFailurePoint Point,
		uint32 Occurrence) -> void
	{
		auto& Injection = GetRelocationFailureInjection();
		if (Point == EAssetRelocationFailurePoint::None)
		{
			Injection.RemainingOccurrences.clear();
			return;
		}
		Injection.RemainingOccurrences.insert_or_assign(
			Point, std::max(Occurrence, 1u));
	}

	namespace AssetToolsPrivate
	{
		auto ConsumeAssetRelocationFailure(
			EAssetRelocationFailurePoint Point) -> bool
		{
			FRelocationFailureInjection& Injection =
				GetRelocationFailureInjection();
			auto Injected = Injection.RemainingOccurrences.find(Point);
			if (Injected == Injection.RemainingOccurrences.end()
				|| Injected->second == 0)
				return false;
			if (--Injected->second != 0) return false;
			Injection.RemainingOccurrences.erase(Injected);
			return true;
		}
	}
}
