#include "PSOPrecacheComponent.h"

#include "CoreGlobals.h"
#include "RenderingThread.h"

namespace Durin
{
	namespace
	{
		std::mutex GOwnerNotificationsMutex;
		std::vector<std::function<void()>> GOwnerNotifications;
	}
	auto FPSOPrecacheComponentData::DispatchOwnerNotifications() -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		std::vector<std::function<void()>> Pending;
		{ std::lock_guard Lock(GOwnerNotificationsMutex); Pending.swap(GOwnerNotifications); }
		for (auto& Notify : Pending) Notify();
	}
	static auto CheckPrecacheOwnerThread() -> void { if (GIsGameThreadIdInitialized) CheckGameThread(); }
	FPSOPrecacheComponentData::~FPSOPrecacheComponentData() { if (CompletionEpoch) ++*CompletionEpoch; }
	auto FPSOPrecacheComponentData::Reset() -> void
	{
		CheckPrecacheOwnerThread();
		require(LatestPSOPrecacheJobSet != std::numeric_limits<uint64>::max());
		++LatestPSOPrecacheJobSet;
		if (CompletionEpoch) *CompletionEpoch = LatestPSOPrecacheJobSet;
		Called = false; Completion = {}; Subscriptions.clear(); MaterialPSOPrecacheRequestIDs.clear();
	}
	auto FPSOPrecacheComponentData::SetMaterialPSOPrecacheData(std::vector<FMaterialPSOPrecacheRequestID> Requests,
		std::function<void()> Finished, std::function<void()> Changed) -> bool
	{
		CheckPrecacheOwnerThread();
		if (Requests.size() > MaximumRequests) return false;
		if (!CompletionEpoch) CompletionEpoch = std::make_shared<std::atomic<uint64>>(0);
		Reset(); Called = true;
		MaterialPSOPrecacheRequestIDs = std::move(Requests); Completion = std::move(Finished);
		if (Changed)
		{
			const std::weak_ptr<std::atomic<uint64>> Epoch = CompletionEpoch;
			const auto JobSet = LatestPSOPrecacheJobSet;
			auto Queued = std::make_shared<std::atomic_bool>(false);
			for (const auto& Request : MaterialPSOPrecacheRequestIDs)
				Subscriptions.push_back(Request.Subscribe([Epoch, JobSet, Queued, Changed] {
					if (GIsGameThreadIdInitialized && IsInGameThread())
					{
						if (const auto Current = Epoch.lock(); Current && Current->load() == JobSet) Changed();
						return;
					}
					if (Queued->exchange(true)) return;
					std::lock_guard Lock(GOwnerNotificationsMutex);
					GOwnerNotifications.emplace_back([Epoch, JobSet, Queued, Changed] {
						Queued->store(false);
						if (const auto Current = Epoch.lock(); Current && Current->load() == JobSet) Changed();
					});
				}));
		}
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
