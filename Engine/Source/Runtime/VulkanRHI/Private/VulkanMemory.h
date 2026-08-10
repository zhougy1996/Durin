#pragma once

#include "CoreMinimal.h"
#include "VulkanRHIAPI.h"
#include "VulkanQueue.h"

namespace Durin::VulkanRHI
{
	class FVulkanDevice;
	class FVulkanFenceManager;
	enum class EVulkanAllocationClassCandidate : uint8;

	struct FVulkanMappedRange
	{
		vk::DeviceSize Offset = 0;
		vk::DeviceSize Size = 0;
	};

	VULKANRHI_API auto NormalizeVulkanMappedRange(vk::DeviceSize Offset,
		vk::DeviceSize Size, vk::DeviceSize AllocationSize,
		vk::DeviceSize NonCoherentAtomSize) -> FVulkanMappedRange;

	// Carries a VMA allocation handle and the memory properties visible to its owner.
	struct FVulkanAllocation
	{
		VmaAllocation Handle = nullptr;
		VmaAllocationInfo Info;
		EVulkanAllocationClassCandidate Class;
		bool bDedicated = false;

		template<typename T = void>
		auto GetMappedData() const -> T* { return static_cast<T*>(Info.pMappedData); }

		auto GetSize() const -> vk::DeviceSize { return Info.size; }

		auto GetMemory() const -> vk::DeviceMemory { return Info.deviceMemory; }

		auto GetOffset() const -> vk::DeviceSize { return Info.offset; }

		auto GetMemoryTypeIndex() const -> uint32 { return Info.memoryType; }

		auto IsValid() const -> bool { return Handle != nullptr; }

		auto FlushMappedMemory(FVulkanDevice* Device) const -> void;
	};

	// Owns the Vulkan memory allocator and centralizes resource allocation policy.
	class FVulkanMemoryManager
	{
	public:
		FVulkanMemoryManager();

		auto Init(FVulkanDevice* InDevice) -> void;

		auto Deinit() -> void;

		auto CreateImage(FVulkanAllocation& OutAllocation, vk::Image& OutImage,
			EVulkanAllocationClassCandidate Candidate,
			const vk::ImageCreateInfo& ImageCreateInfo,
			const char* DebugName = nullptr) const -> vk::Result;

		auto CreateBuffer(FVulkanAllocation& OutAllocation, vk::Buffer& OutBuffer,
			EVulkanAllocationClassCandidate Candidate,
			const vk::BufferCreateInfo& BufferCreateInfo,
			const char* DebugName = nullptr) const -> vk::Result;

		auto DestroyImage(FVulkanAllocation& InAllocation, vk::Image InImage) const -> void;

		auto DestroyBuffer(FVulkanAllocation& InAllocation, vk::Buffer InBuffer) const -> void;

		auto MapMemory(FVulkanAllocation& Allocation) const -> void*;

		auto UnmapMemory(FVulkanAllocation& Allocation) const -> void;

		auto Flush(const FVulkanAllocation& Allocation, vk::DeviceSize Offset = 0, vk::DeviceSize Size = VK_WHOLE_SIZE) const -> void;

		auto Invalidate(const FVulkanAllocation& Allocation, vk::DeviceSize Offset = 0, vk::DeviceSize Size = VK_WHOLE_SIZE) const -> void;

		auto GetMemoryType(uint32 MemoryTypeIndex) const -> vk::MemoryType;

		auto GetMemoryType(const FVulkanAllocation& Allocation) const -> vk::MemoryType;

		auto GetMemoryHeap(uint32 MemoryHeapIndex) const -> vk::MemoryHeap;

		auto RefreshBaselineHeapBudgets() const -> void;

		DURIN_NONCOPYABLE(FVulkanMemoryManager)
	private:
		FVulkanDevice* Device;

		vk::PhysicalDeviceMemoryProperties MemoryProperties;

		VmaAllocator Allocator;
	};

	// Wraps a reusable Vulkan fence and tracks whether it is idle, submitted, or signaled.
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
		// Tracks whether a pooled fence is idle, submitted, or observed as signaled.
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

	// Pools Vulkan fences and returns them only after their submissions complete.
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

	// Owns a Vulkan semaphore used to order queue and presentation operations.
	class FVulkanSemaphore
	{
	public:
		FVulkanSemaphore(FVulkanDevice& InDevice);

		virtual ~FVulkanSemaphore();

		auto GetHandle() const -> vk::Semaphore { return Semaphore; }
		// Destroys a semaphore whose owning queue/present scope has already been
		// retired, without sweeping unrelated device deferred deletions.
		auto DestroyImmediately() -> void;

	private:
		FVulkanDevice& Device;

		vk::Semaphore Semaphore;
	};
} // namespace Durin::VulkanRHI
