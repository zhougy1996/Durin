#pragma once

#include "VulkanQueue.h"
#include "VMA/VulkanMemoryAllocator.h"

namespace Doge::VulkanRHI
{
	class FVulkanDevice;
	class FVulkanFenceManager;

	struct FVulkanAllocation
	{
		VmaAllocation Handle = nullptr;
		VmaAllocationInfo Info;

		template<typename T = void>
		auto GetMappedData() const -> T* { return static_cast<T*>(Info.pMappedData); }

		auto GetSize() const -> vk::DeviceSize { return Info.size; }

		auto GetMemory() const -> vk::DeviceMemory { return Info.deviceMemory; }

		auto GetOffset() const -> vk::DeviceSize { return Info.offset; }

		auto GetMemoryTypeIndex() const -> uint32 { return Info.memoryType; }

		auto IsValid() const -> bool { return Handle != nullptr; }

		auto FlushMappedMemory(FVulkanDevice* Device) const -> void;
	};

	class FVulkanMemoryManager
	{
	public:
		FVulkanMemoryManager();

		auto Init(FVulkanDevice* InDevice) -> void;

		auto Deinit() -> void;

		auto CreateImage(FVulkanAllocation& OutAllocation, vk::Image& OutImage, const vk::ImageCreateInfo& ImageCreateInfo, const char* DebugName = nullptr) const -> bool;

		auto CreateBuffer(FVulkanAllocation& OutAllocation, vk::Buffer& OutBuffer, const vk::BufferCreateInfo& BufferCreateInfo, const char* DebugName = nullptr) const -> bool;

		auto DestroyImage(FVulkanAllocation& InAllocation, vk::Image InImage) const -> void;

		auto DestroyBuffer(FVulkanAllocation& InAllocation, vk::Buffer InBuffer) const -> void;

		auto MapMemory(const FVulkanAllocation& Allocation) const -> void*;

		auto UnmapMemory(const FVulkanAllocation& Allocation) const -> void;

		auto Flush(const FVulkanAllocation& Allocation, vk::DeviceSize Offset = 0, vk::DeviceSize Size = VK_WHOLE_SIZE) const -> void;

		auto GetMemoryType(uint32 MemoryTypeIndex) const -> vk::MemoryType;

		auto GetMemoryType(const FVulkanAllocation& Allocation) const -> vk::MemoryType;

		auto GetMemoryHeap(uint32 MemoryHeapIndex) const -> vk::MemoryHeap;

		DOGE_NONCOPYABLE(FVulkanMemoryManager)
	private:
		FVulkanDevice* Device;

		vk::PhysicalDeviceMemoryProperties MemoryProperties;

		VmaAllocator Allocator;
	};

	class FVulkanFence
	{
	public:
		FVulkanFence(FVulkanDevice& InDevice, FVulkanFenceManager& InOwner, bool bInCreateSignaled);

		~FVulkanFence();

		auto GetHandle() const -> vk::Fence { return Handle; }

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

		vk::Fence Handle;

		EState State;

		friend class FVulkanFenceManager;
	};

	class FVulkanFenceManager
	{
	public:
		FVulkanFenceManager(FVulkanDevice& InDevice);

		~FVulkanFenceManager();

		auto Deinit() -> void;

		// Check if the fence is signaled, will check refresh the state of the fence
		auto IsFenceSignaled(FVulkanFence* InFence) -> bool;

		auto AllocateFence(bool bInCreateSignaled) -> FVulkanFence*;

		auto ReleaseFence(FVulkanFence*& InFence) -> void;

		auto WaitForFence(FVulkanFence* InFence, uint64 InTimeoutInNanoseconds) -> bool;

		auto ResetFence(FVulkanFence* InFence) -> void;

	private:
		auto CheckFenceSignaled(FVulkanFence* InFence) const -> bool;

		auto DestroyFence(FVulkanFence* InFence) const -> void;

		FVulkanDevice& Device;

		std::vector<FVulkanFence*> FreeFences;

		std::vector<FVulkanFence*> UsedFences;

		std::mutex FenceMutex;

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
} // namespace Doge::VulkanRHI