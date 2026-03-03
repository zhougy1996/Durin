#pragma once

namespace Doge::VulkanRHI
{
	class FVulkanDevice;
	class FVulkanFenceManager;

	class FVulkanFence
	{
	public:
		FVulkanFence(FVulkanDevice& InDevice, FVulkanFenceManager& InOwner, bool bInCreateSignaled);

		~FVulkanFence();

		auto GetHandle() const -> vk::Fence { return Fence; }

		// Return the cached state of the fence, the state will not be checked or refreshed
		// If you want to refresh the state of the fence, use FVulkanFenceManager::IsFenceSignaled
		auto IsSignaled() const -> bool { return State == EState::Signaled; }

	private:
		enum class EState
		{
			// Initial state
			NotReady,

			// After GPU processed it
			Signaled,
		};


		auto Wait(uint64 TimeoutInNanoseconds) -> bool;

		auto Reset() -> void;

		FVulkanDevice& Device;

		FVulkanFenceManager& Owner;

		vk::Fence Fence;

		EState State;

		friend class FVulkanFenceManager;
	};

	class FVulkanFenceManager
	{
	public:
		FVulkanFenceManager(FVulkanDevice& InDevice);

		// Check if the fence is signaled, will check refresh the state of the fence
		auto IsFenceSignaled(FVulkanFence* InFence) -> bool;

		auto AllocateFence(bool bInCreateSignaled) -> FVulkanFence*;

		auto ReleaseFence(FVulkanFence*& InFence) -> void;

		auto WaitForFence(FVulkanFence* InFence, uint64 InTimeoutInNanoseconds) -> bool;

		auto ResetFence(FVulkanFence* InFence) -> void;

	private:
		auto CheckFenceSignaled(FVulkanFence* InFence) const -> bool;

		FVulkanDevice& Device;
	};

	class FVulkanSemaphore
	{
	public:
		FVulkanSemaphore(FVulkanDevice& InDevice);

		virtual ~FVulkanSemaphore();

		auto GetHandle() const -> vk::Semaphore { return Semaphore; }

	private:
		FVulkanDevice& Device;

		vk::Semaphore Semaphore;
	};
}