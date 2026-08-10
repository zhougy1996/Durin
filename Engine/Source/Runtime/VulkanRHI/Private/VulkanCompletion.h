#pragma once

#include "VulkanRHIAPI.h"

namespace Durin::VulkanRHI
{
	class FVulkanDevice;
	class FVulkanFence;
	class FVulkanPayload;

	using FVulkanCompletionToken = uint64;

	// Models ordered queue completion independently from Vulkan fence observation.
	class VULKANRHI_API FVulkanCompletionWatermark
	{
	public:
		auto AllocateToken() -> FVulkanCompletionToken;
		auto ObserveCompleted(FVulkanCompletionToken Token) -> void;
		auto GetCompletedToken() const -> FVulkanCompletionToken;
		auto IsRetirementEligible(FVulkanCompletionToken Token) const -> bool;

	private:
		FVulkanCompletionToken NextToken = 1;
		FVulkanCompletionToken CompletedToken = 0;
		std::set<FVulkanCompletionToken> ObservedCompletedTokens;
	};

	// Owns per-submit fences and releases payload storage in queue-token order.
	class FVulkanCompletionTracker
	{
	public:
		explicit FVulkanCompletionTracker(FVulkanDevice& InDevice);

		auto ReserveToken() -> FVulkanCompletionToken;
		auto TrackSubmitted(FVulkanCompletionToken Token, FVulkanFence* Fence,
			std::span<FVulkanPayload* const> Payloads) -> FVulkanCompletionToken;
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
		};

		auto ObserveThrough(FVulkanCompletionToken Token) -> void;
		auto ReleaseCompleted() -> void;

		FVulkanDevice& Device;
		FVulkanCompletionWatermark Watermark;
		std::deque<FSubmission> Submissions;
		std::atomic<FVulkanCompletionToken> LastSubmittedToken = 0;
		std::atomic<FVulkanCompletionToken> LastReservedToken = 0;
	};
} // namespace Durin::VulkanRHI
