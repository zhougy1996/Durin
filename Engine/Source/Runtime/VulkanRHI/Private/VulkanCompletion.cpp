#include "VulkanCompletion.h"

#include "VulkanCommandBuffer.h"
#include "VulkanDevice.h"
#include "VulkanDiagnostics.h"
#include "VulkanMemory.h"
#include "VulkanRHIPrivate.h"
#include "VulkanSubmission.h"

namespace Durin::VulkanRHI
{
	auto FVulkanCompletionWatermark::AllocateToken() -> FVulkanCompletionToken
	{
		requiref(NextToken != 0, "Vulkan completion token space is exhausted.");
		return NextToken++;
	}

	auto FVulkanCompletionWatermark::ObserveCompleted(
		FVulkanCompletionToken Token) -> void
	{
		if (Token == 0 || Token <= CompletedToken)
		{
			return;
		}
		requiref(Token < NextToken,
			"Cannot complete an unallocated Vulkan token: token={}, next={}",
			Token, NextToken);
		ObservedCompletedTokens.insert(Token);
		while (ObservedCompletedTokens.erase(CompletedToken + 1) != 0)
		{
			++CompletedToken;
		}
	}

	auto FVulkanCompletionWatermark::GetCompletedToken() const
		-> FVulkanCompletionToken
	{
		return CompletedToken;
	}

	auto FVulkanCompletionWatermark::IsRetirementEligible(
		FVulkanCompletionToken Token) const -> bool
	{
		return Token == 0 || Token <= CompletedToken;
	}

	FVulkanCompletionTracker::FVulkanCompletionTracker(FVulkanDevice& InDevice)
		: Device(InDevice)
	{
	}

	auto FVulkanCompletionTracker::ReserveToken() -> FVulkanCompletionToken
	{
		CheckVulkanRHIThread();
		LastReservedToken = Watermark.AllocateToken();
		return LastReservedToken;
	}

	auto FVulkanCompletionTracker::TrackSubmitted(
		FVulkanCompletionToken Token, FVulkanFence* Fence,
		std::span<FVulkanPayload* const> Payloads) -> FVulkanCompletionToken
	{
		CheckVulkanRHIThread();
		check(Fence && !Payloads.empty());
		check(Token > LastSubmittedToken.load()
			&& Token <= LastReservedToken.load());
		LastSubmittedToken = Token;
		FSubmission& Submission = Submissions.emplace_back();
		Submission.Token = Token;
		Submission.Fence = Fence;
		Submission.Payloads.assign(Payloads.begin(), Payloads.end());
		return Token;
	}

	auto FVulkanCompletionTracker::Poll() -> void
	{
		CheckVulkanRHIThread();
		for (FSubmission& Submission : Submissions)
		{
			if (!Device.GetFenceManager().IsFenceSignaled(Submission.Fence))
			{
				break;
			}
			Watermark.ObserveCompleted(Submission.Token);
		}
		ReleaseCompleted();
	}

	auto FVulkanCompletionTracker::WaitForToken(
		FVulkanCompletionToken Token) -> void
	{
		CheckVulkanRHIThread();
		if (Token == 0 || Token <= Watermark.GetCompletedToken())
		{
			return;
		}
		const auto It = std::ranges::find(Submissions, Token,
			&FSubmission::Token);
		requiref(It != Submissions.end(),
			"Unknown Vulkan completion token: token={}, completed={}, submitted={}",
			Token, Watermark.GetCompletedToken(), LastSubmittedToken.load());
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

	auto FVulkanCompletionTracker::WaitForAll() -> void
	{
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
		return Watermark.GetCompletedToken();
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
			Watermark.ObserveCompleted(Submission.Token);
		}
	}

	auto FVulkanCompletionTracker::ReleaseCompleted() -> void
	{
		while (!Submissions.empty()
			&& Submissions.front().Token <= Watermark.GetCompletedToken())
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
