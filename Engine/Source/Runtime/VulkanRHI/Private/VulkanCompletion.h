#pragma once

#include "VulkanRHIAPI.h"
#include "RHICompletion.h"

namespace Durin::VulkanRHI
{
	class FVulkanDevice;
	class FVulkanFence;
	class FVulkanPayload;

	using FVulkanCompletionToken = uint64;

	// Owns per-submit fences and releases payload storage in queue-token order.
	class FVulkanCompletionTracker
	{
	public:
		FVulkanCompletionTracker(FVulkanDevice& InDevice, uint64 InDeviceGeneration, FRHIQueueId InQueue);

		auto ReserveToken() -> FVulkanCompletionToken;
		// Allocate ownership storage before vkQueueSubmit; commit never allocates.
		auto PrepareSubmission(FVulkanCompletionToken Token, FVulkanFence* Fence,
			std::span<FVulkanPayload* const> Payloads) -> void;
		auto CommitSubmission() -> FVulkanCompletionToken;
		// Quarantine ambiguous native failure until device teardown, without recycling.
		auto FailSubmission(bool bDeviceLost = false) -> void;
		auto ReleaseAfterDeviceStopped() -> void;
		auto GetLastReservedTicket() const -> FRHIGPUSubmissionTicket;
		auto WaitForTicket(const FRHIGPUSubmissionTicket& Ticket, uint64 TimeoutNanoseconds)
			-> ERHIGPUWaitResult;
		auto GetDeviceGeneration() const -> uint64 { return DeviceGeneration; }
		auto Owns(const FRHIGPUSubmissionTicket& Ticket) const -> bool { return Timeline.Owns(Ticket); }
		auto Poll() -> void;
		auto WaitForToken(FVulkanCompletionToken Token) -> void;
		auto WaitForAll() -> void;

		auto GetLastSubmittedToken() const -> FVulkanCompletionToken;
		auto GetLastReservedToken() const -> FVulkanCompletionToken;
		auto GetCompletedToken() const -> FVulkanCompletionToken;
		auto GetPendingSubmissionCount() const -> uint64;

	private:
		struct FSubmission
		{
			FVulkanCompletionToken Token = 0;
			FVulkanFence* Fence = nullptr;
			std::vector<FVulkanPayload*> Payloads;
			FRHIGPUSubmissionTicket Ticket;
			bool bSubmitted = false;
		};

		auto ObserveThrough(FVulkanCompletionToken Token) -> void;
		auto ReleaseCompleted() -> void;

		FVulkanDevice& Device;
		// Compatibility observation for existing one-queue arenas and statistics.
		std::atomic<FVulkanCompletionToken> CompletedToken = 0;
		const uint64 DeviceGeneration;
		FRHIGPUQueueTimeline Timeline;
		mutable std::mutex TicketMutex;
		FRHIGPUSubmissionTicket LastReservedTicket;
		bool bFailed = false;
		std::deque<FSubmission> Submissions;
		std::atomic<FVulkanCompletionToken> LastSubmittedToken = 0;
		std::atomic<FVulkanCompletionToken> LastReservedToken = 0;
	};
} // namespace Durin::VulkanRHI
