#include "PSOPrecacheComponent.h"

#include "CoreGlobals.h"
#include "RenderingThread.h"

namespace Durin
{
	static auto CheckPrecacheOwnerThread() -> void { if (GIsGameThreadIdInitialized) CheckGameThread(); }
	FPSOPrecacheComponentData::~FPSOPrecacheComponentData() { if (CompletionEpoch) ++*CompletionEpoch; }
	auto FPSOPrecacheComponentData::Reset() -> void
	{
		CheckPrecacheOwnerThread();
		require(LatestPSOPrecacheJobSet != std::numeric_limits<uint64>::max());
		++LatestPSOPrecacheJobSet;
		if (CompletionEpoch) *CompletionEpoch = LatestPSOPrecacheJobSet;
		Called = false; Completion = {}; MaterialPSOPrecacheRequestIDs.clear();
	}
	auto FPSOPrecacheComponentData::SetMaterialPSOPrecacheData(std::vector<FMaterialPSOPrecacheRequestID> Requests,
		std::function<void()> Finished) -> bool
	{
		CheckPrecacheOwnerThread();
		if (Requests.size() > MaximumRequests) return false;
		if (!CompletionEpoch) CompletionEpoch = std::make_shared<std::atomic<uint64>>(0);
		Reset(); Called = true;
		MaterialPSOPrecacheRequestIDs = std::move(Requests); Completion = std::move(Finished);
		return true;
	}
	auto FPSOPrecacheComponentData::GetState() const -> EMaterialPSOPrecacheState
	{
		if (!Called) return EMaterialPSOPrecacheState::Canceled;
		bool Pending = false;
		for (const auto& Request : MaterialPSOPrecacheRequestIDs)
		{
			const auto State = Request.GetProgress().State;
			if (State == EMaterialPSOPrecacheState::Pending) Pending = true;
			else if (State != EMaterialPSOPrecacheState::Ready) return State;
		}
		return Pending ? EMaterialPSOPrecacheState::Pending : EMaterialPSOPrecacheState::Ready;
	}
	auto FPSOPrecacheComponentData::IsPSOPrecaching() const -> bool
	{
		CheckPrecacheOwnerThread();
		return Called && std::ranges::any_of(MaterialPSOPrecacheRequestIDs,
			[](const auto& Request) { return Request.GetProgress().State == EMaterialPSOPrecacheState::Pending; });
	}
	auto FPSOPrecacheComponentData::IsReadyForProxyCreation() const -> bool
	{
		CheckPrecacheOwnerThread();
		return Called && std::ranges::all_of(MaterialPSOPrecacheRequestIDs,
			[](const auto& Request) { return Request.GetProgress().bRequiredReady; });
	}
	auto FPSOPrecacheComponentData::CheckPSOPrecachingAndBoostPriority(EPSOPrecachePriority Priority) -> bool
	{
		CheckPrecacheOwnerThread();
		for (const auto& Request : MaterialPSOPrecacheRequestIDs) Request.BoostPriority(Priority);
		return IsPSOPrecaching();
	}
	auto FPSOPrecacheComponentData::SetLatestJobSetCompleted(uint64 JobSet) -> bool
	{
		CheckPrecacheOwnerThread();
		if (JobSet != LatestPSOPrecacheJobSet || GetState() != EMaterialPSOPrecacheState::Ready) return false;
		return Poll() == EMaterialPSOPrecacheState::Ready;
	}
	auto FPSOPrecacheComponentData::Poll() -> EMaterialPSOPrecacheState
	{
		CheckPrecacheOwnerThread();
		const auto State = GetState();
		if (State == EMaterialPSOPrecacheState::Ready && LatestPSOPrecacheJobSetCompleted != LatestPSOPrecacheJobSet)
		{
			LatestPSOPrecacheJobSetCompleted = LatestPSOPrecacheJobSet;
			const auto Epoch = CompletionEpoch;
			const auto JobSet = LatestPSOPrecacheJobSet;
			auto Finished = std::exchange(Completion, {});
			if (Finished) Finished(); // Reentrant Reset/replacement cannot publish the old set afterward.
			return Epoch->load() == JobSet ? State : EMaterialPSOPrecacheState::Canceled;
		}
		return State;
	}
}
