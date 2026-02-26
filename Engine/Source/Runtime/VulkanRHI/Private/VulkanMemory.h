#pragma once

namespace Doge::VulkanRHI
{
	class FVulkanDevice;
	class FVulkanFenceManager;

	class FVulkanFence
	{
	public:
		FVulkanFence(FVulkanDevice& Device, FVulkanFenceManager& Owner, bool bCreateSignaled);

		~FVulkanFence();

		auto GetHandle() const -> vk::Fence { return Fence_; }

		// Return the cached state of the fence, the state will not be checked or refreshed
		// If you want to refresh the state of the fence, use FVulkanFenceManager::IsFenceSignaled
		auto IsSignaled() const -> bool { return State_ == EState::eSignaled; }

	private:
		enum class EState
		{
			// Initial state
			eNotReady,

			// After GPU processed it
			eSignaled,
		};


		auto Wait(uint64 TimeoutInNanoseconds) -> bool;

		auto Reset() -> void;

		FVulkanDevice& Device_;

		FVulkanFenceManager& Owner_;

		vk::Fence Fence_;

		EState State_;

		friend class FVulkanFenceManager;
	};

	class FVulkanFenceManager
	{
	public:
		FVulkanFenceManager(FVulkanDevice& Device);

		// Check if the fence is signaled, will check refresh the state of the fence
		auto IsFenceSignaled(FVulkanFence* Fence) -> bool;

		auto AllocateFence(bool bCreateSignaled) -> FVulkanFence*;

		auto ReleaseFence(FVulkanFence*& Fence) -> void;

		auto WaitForFence(FVulkanFence* Fence, uint64 TimeoutInNanoseconds) -> bool;

		auto ResetFence(FVulkanFence* Fence) -> void;

	private:
		auto CheckFenceSignaled(FVulkanFence* Fence) -> bool;

		FVulkanDevice& Device_;
	};

	class FVulkanSemaphore
	{
	public:
		FVulkanSemaphore(FVulkanDevice& Device);

		virtual ~FVulkanSemaphore();

		auto GetHandle() -> vk::Semaphore const { return Semaphore_; }

	private:
		FVulkanDevice& Device_;

		vk::Semaphore Semaphore_;
	};
}