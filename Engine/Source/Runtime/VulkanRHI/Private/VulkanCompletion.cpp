#include "VulkanCompletion.h"
#include "Backend/RHICompletionBackend.h"

#include "VulkanCommandBuffer.h"
#include "VulkanDevice.h"
#include "VulkanGPUTiming.h"
#include "VulkanDiagnostics.h"
#include "VulkanMemory.h"
#include "VulkanRHIPrivate.h"
#include "VulkanSubmission.h"

namespace Durin::VulkanRHI
{
	FVulkanCompletionTracker::FVulkanCompletionTracker(FVulkanDevice& InDevice,
		uint64 InDeviceGeneration, FRHIQueueId InQueue)
		: Device(InDevice)
		, DeviceGeneration(InDeviceGeneration)
		, Timeline(DeviceGeneration, InQueue)
	{
	}

	auto FVulkanCompletionTracker::ReserveSyncPoint() -> FRHIGPUSyncPointRef
	{
		CheckVulkanRHIThread();
		const auto SyncPoint = Timeline.Reserve();
		require(SyncPoint.GetState() == ERHIGPUSubmissionState::Pending);
		LastReservedToken = FRHIGPUSyncPointBackend::GetPoint(SyncPoint).Value;
		{
			std::lock_guard Lock(SyncPointMutex);
			LastReservedSyncPoint = SyncPoint;
		}
		return SyncPoint;
	}

	auto FVulkanCompletionTracker::CancelUnsubmitted(const FRHIGPUSyncPointRef& SyncPoint) -> void
	{
		CheckVulkanRHIThread();
		require(Timeline.Cancel(SyncPoint));
	}

	auto FVulkanCompletionTracker::GetLastReservedSyncPoint() const -> FRHIGPUSyncPointRef
	{
		std::lock_guard Lock(SyncPointMutex);
		return LastReservedSyncPoint;
	}

	auto FVulkanCompletionTracker::PrepareSubmission(
		FVulkanFence* Fence,
		std::span<FVulkanPayload* const> Payloads) -> void
	{
		CheckVulkanRHIThread();
		require(!bFailed && Fence && !Payloads.empty());
		require(Submissions.empty() || Submissions.back().bSubmitted);
		FSubmission Submission;
		Submission.Fence = Fence;
		Submission.Payloads.assign(Payloads.begin(), Payloads.end());
		require(Payloads.size() == 1);
		Submission.SyncPoint = Payloads.front()->GetSyncPoint();
		Submission.Token = FRHIGPUSyncPointBackend::GetPoint(Submission.SyncPoint).Value;
		require(Timeline.CanSubmit(Submission.SyncPoint));
		Submissions.push_back(std::move(Submission));
	}

	auto FVulkanCompletionTracker::CommitSubmission() -> FVulkanCompletionToken
	{
		CheckVulkanRHIThread();
		require(!bFailed && !Submissions.empty() && !Submissions.back().bSubmitted);
		auto& Submission = Submissions.back();
		const bool bAccepted = Timeline.MarkSubmitted(Submission.SyncPoint);
		require(bAccepted);
		Submission.bSubmitted = true;
		LastSubmittedToken = Submission.Token;
		return Submission.Token;
	}

	auto FVulkanCompletionTracker::FailSubmission(bool bDeviceLost) -> void
	{
		CheckVulkanRHIThread();
		bFailed = true;
		Timeline.Fail(bDeviceLost);
	}

	auto FVulkanCompletionTracker::ReleaseAfterDeviceStopped() -> void
	{
		CheckVulkanRHIThread();
		// Teardown is not successful completion. Command buffers remain in their
		// owning pool; quarantined fences are destroyed without reuse or reset.
		Timeline.Fail();
		for (auto& Submission : Submissions)
		{
			for (auto* Payload : Submission.Payloads) delete Payload;
			Device.GetFenceManager().DestroyFenceAfterDeviceStopped(Submission.Fence);
		}
		Submissions.clear();
	}

	auto FVulkanCompletionTracker::Poll() -> void
	{
		CheckVulkanRHIThread();
		if (bFailed) return;
		for (FSubmission& Submission : Submissions)
		{
			if (!Submission.bSubmitted) break;
			if (!Device.GetFenceManager().IsFenceSignaled(Submission.Fence))
			{
				break;
			}
			// Result processing is part of completion publication, not just recycling.
			if (!ResolveResults(Submission)) break;
			const bool bObserved = Timeline.ObserveCompleted(Submission.SyncPoint);
			require(bObserved);
			CompletedToken.store(Submission.Token, std::memory_order_release);
		}
		ReleaseCompleted();
	}

	auto FVulkanCompletionTracker::WaitForToken(
		FVulkanCompletionToken Token) -> void
	{
		CheckVulkanRHIThread();
		requiref(!bFailed, "Cannot wait for failed Vulkan submission as normal GPU completion.");
		if (Token == 0 || Token <= CompletedToken.load(std::memory_order_acquire))
		{
			return;
		}
		const auto It = std::ranges::find(Submissions, Token,
			&FSubmission::Token);
		requiref(It != Submissions.end(),
			"Unknown Vulkan completion token: token={}, completed={}, submitted={}",
			Token, CompletedToken.load(std::memory_order_acquire), LastSubmittedToken.load());
		const auto WaitStart = std::chrono::steady_clock::now();
		const bool bCompleted = Device.GetFenceManager().WaitForFence(
			It->Fence, UINT64_MAX);
		requiref(bCompleted, "Failed to wait for Vulkan completion token {}.", Token);
		const auto WaitDuration = std::chrono::steady_clock::now() - WaitStart;
		GVulkanMemoryBaselineTracker.RecordFrameFenceWait(
			std::chrono::duration_cast<std::chrono::nanoseconds>(WaitDuration).count());
		ObserveThrough(Token);
		ReleaseCompleted();
	}

	auto FVulkanCompletionTracker::WaitForSyncPoint(const FRHIGPUSyncPointRef& SyncPoint,
		uint64 TimeoutNanoseconds) -> ERHIGPUWaitResult
	{
		CheckVulkanRHIThread();
		if (!Timeline.Owns(SyncPoint)) return ERHIGPUWaitResult::Invalid;
		try
		{
			Poll();
			switch (SyncPoint.GetState())
			{
			case ERHIGPUSubmissionState::Complete: return ERHIGPUWaitResult::Complete;
			case ERHIGPUSubmissionState::Pending: return ERHIGPUWaitResult::Pending;
			case ERHIGPUSubmissionState::Canceled: return ERHIGPUWaitResult::Canceled;
			case ERHIGPUSubmissionState::Failed: return ERHIGPUWaitResult::Failed;
			case ERHIGPUSubmissionState::DeviceLost: return ERHIGPUWaitResult::DeviceLost;
			case ERHIGPUSubmissionState::Invalid: return ERHIGPUWaitResult::Invalid;
			case ERHIGPUSubmissionState::Submitted: break;
			}
			const auto Token = FRHIGPUSyncPointBackend::GetPoint(SyncPoint).Value;
			const auto It = std::ranges::find(Submissions, Token, &FSubmission::Token);
			require(It != Submissions.end() && It->bSubmitted);
			const auto Start = std::chrono::steady_clock::now();
			const bool bComplete = Device.GetFenceManager().WaitForFence(It->Fence, TimeoutNanoseconds);
			GVulkanMemoryBaselineTracker.RecordFrameFenceWait(std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - Start).count());
			if (!bComplete) return ERHIGPUWaitResult::Timeout;
			ObserveThrough(Token);
			ReleaseCompleted();
			return SyncPoint.IsComplete() ? ERHIGPUWaitResult::Complete : ERHIGPUWaitResult::Timeout;
		}
		catch (const vk::SystemError& Error)
		{
			const bool bDeviceLost = Error.code().value() == static_cast<int>(vk::Result::eErrorDeviceLost);
			FailSubmission(bDeviceLost);
			return bDeviceLost ? ERHIGPUWaitResult::DeviceLost : ERHIGPUWaitResult::Failed;
		}
	}

	auto FVulkanCompletionTracker::WaitForAll() -> void
	{
		if (bFailed) return;
		WaitForToken(LastSubmittedToken.load());
	}

	auto FVulkanCompletionTracker::GetLastSubmittedToken() const
		-> FVulkanCompletionToken
	{
		return LastSubmittedToken.load();
	}

	auto FVulkanCompletionTracker::GetLastReservedToken() const
		-> FVulkanCompletionToken
	{
		return LastReservedToken.load();
	}

	auto FVulkanCompletionTracker::GetCompletedToken() const
		-> FVulkanCompletionToken
	{
		return CompletedToken.load(std::memory_order_acquire);
	}

	auto FVulkanCompletionTracker::GetPendingSubmissionCount() const -> uint64
	{
		CheckVulkanRHIThread();
		return Submissions.size();
	}

	auto FVulkanCompletionTracker::ObserveThrough(
		FVulkanCompletionToken Token) -> void
	{
		for (FSubmission& Submission : Submissions)
		{
			if (Submission.Token > Token)
			{
				break;
			}
			requiref(Device.GetFenceManager().IsFenceSignaled(Submission.Fence),
				"Vulkan queue completion was not contiguous at token {}.",
				Submission.Token);
			if (!ResolveResults(Submission)) break;
			const bool bObserved = Timeline.ObserveCompleted(Submission.SyncPoint);
			require(bObserved);
			CompletedToken.store(Submission.Token, std::memory_order_release);
		}
	}

	auto FVulkanCompletionTracker::AppendAllocationUses(const std::shared_ptr<void>& Owner,
		FRHIRetirementPrerequisites& Uses) const -> void
	{
		CheckVulkanRHIThread();
		for (const auto& Submission : Submissions)
			for (const auto* Payload : Submission.Payloads)
				if (std::ranges::find(Payload->AllocationOwners, Owner) != Payload->AllocationOwners.end())
				{
					require(Uses.Add(Submission.SyncPoint));
					break;
				}
	}

	auto FVulkanCompletionTracker::ResolveResults(FSubmission& Submission) -> bool
	{
		for (auto* Payload : Submission.Payloads)
			if (!Payload->TimingQueries.empty()
				&& !Device.GetGPUTimingManager().ResolveCompleted(Payload->TimingQueries)) return false;
		return true;
	}

	auto FVulkanCompletionTracker::ReleaseCompleted() -> void
	{
		while (!Submissions.empty()
			&& Submissions.front().SyncPoint.IsRetirementEligible())
		{
			FSubmission& Submission = Submissions.front();
			for (FVulkanPayload* Payload : Submission.Payloads)
			{
				for (FVulkanCommandBuffer* CommandBuffer : Payload->CommandBuffers)
				{
					CommandBuffer->Reset();
				}
				delete Payload;
			}
			Device.GetFenceManager().ReleaseFence(Submission.Fence);
			Submissions.pop_front();
		}
	}
} // namespace Durin::VulkanRHI
