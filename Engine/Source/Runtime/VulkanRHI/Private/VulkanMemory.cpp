#include "VulkanMemory.h"

#define VMA_IMPLEMENTATION
#include "vma/vk_mem_alloc.h"

#include "VulkanDevice.h"
#include "VulkanDynamicRHI.h"


namespace Doge::VulkanRHI
{
	auto FVulkanAllocation::FlushMappedMemory(FVulkanDevice* Device) const -> void
	{
		auto& MemoryManager = Device->GetMemoryManager();
		MemoryManager.Flush(*this);
	}

	FVulkanMemoryManager::FVulkanMemoryManager()
		: Device(nullptr)
		, Allocator(nullptr)
	{
	}

	auto FVulkanMemoryManager::Init(FVulkanDevice* InDevice) -> void
	{
		Device = InDevice;
		vk::PhysicalDevice Gpu = Device->GetGpu();
		MemoryProperties = Gpu.getMemoryProperties();

		FVulkanDynamicRHI& DynamicRHI = FVulkanDynamicRHI::Get();
		const vk::DynamicLoader& DynamicLoader = DynamicRHI.RHIGetDynamicLoader();
		VmaVulkanFunctions vmaFuncs = {};
		vmaFuncs.vkGetInstanceProcAddr = DynamicLoader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
		vmaFuncs.vkGetDeviceProcAddr = DynamicLoader.getProcAddress<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");

		VmaAllocatorCreateInfo allocatorInfo = {};
		allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
		allocatorInfo.physicalDevice = Gpu;
		allocatorInfo.device = Device->GetHandle();
		allocatorInfo.pVulkanFunctions = &vmaFuncs;
		allocatorInfo.instance = DynamicRHI.RHIGetVkInstance();

		vmaCreateAllocator(&allocatorInfo, &Allocator);
	}

	auto FVulkanMemoryManager::Deinit() -> void
	{
		vmaDestroyAllocator(Allocator);
		Allocator = nullptr;
	}

	auto FVulkanMemoryManager::CreateImage(FVulkanAllocation& OutAllocation, vk::Image& OutImage, const vk::ImageCreateInfo& ImageCreateInfo, const char* DebugName /* = nullptr */) const -> bool
	{
		VmaAllocationCreateInfo AllocCreateInfo{};
		AllocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO;

		VkImage RawImage;
		VkResult Result = vmaCreateImage(
			Allocator,
			reinterpret_cast<const VkImageCreateInfo*>(&ImageCreateInfo),
			&AllocCreateInfo,
			&RawImage,
			&OutAllocation.Handle,
			&OutAllocation.Info
		);

		if (Result != VK_SUCCESS) return false;

		OutImage = RawImage;

		if (DebugName)
		{
			vmaSetAllocationName(Allocator, OutAllocation.Handle, DebugName);
		}
		return true;
	}

	auto FVulkanMemoryManager::CreateBuffer(FVulkanAllocation& OutAllocation, vk::Buffer& OutBuffer, EVulkanAllocationFlags AllocFlags, const vk::BufferCreateInfo& BufferCreateInfo, const char* DebugName) const -> bool
	{
		VmaAllocationCreateFlags VmaFlags = 0;
		VmaFlags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

		VmaAllocationCreateInfo AllocCreateInfo{};
		AllocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO;
		AllocCreateInfo.flags = VmaFlags;

		if (EnumHasAnyFlags(AllocFlags, EVulkanAllocationFlags::HostVisible))
		{
			AllocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
		}

		VkBuffer RawBuffer;
		VkResult Result = vmaCreateBuffer(
			Allocator,
			reinterpret_cast<const VkBufferCreateInfo*>(&BufferCreateInfo),
			&AllocCreateInfo,
			&RawBuffer,
			&OutAllocation.Handle,
			&OutAllocation.Info
		);

		if (Result != VK_SUCCESS)
		{
			DOGE_ERROR("Failed to create buffer: {}", vk::to_string(static_cast<vk::Result>(Result)));
			return false;
		}

		OutBuffer = RawBuffer;

		if (DebugName)
		{
			vmaSetAllocationName(Allocator, OutAllocation.Handle, DebugName);
		}
		return true;
	}

	auto FVulkanMemoryManager::DestroyImage(FVulkanAllocation& InAllocation, vk::Image InImage) const -> void
	{
		check(InImage && InAllocation.Handle != VK_NULL_HANDLE);
		vmaDestroyImage(Allocator, InImage, InAllocation.Handle);

		InAllocation.Handle = nullptr;
		InAllocation.Info = {};
	}

	auto FVulkanMemoryManager::DestroyBuffer(FVulkanAllocation& InAllocation, vk::Buffer InBuffer) const -> void
	{
		check(InBuffer && InAllocation.Handle != VK_NULL_HANDLE);
		vmaDestroyBuffer(Allocator, InBuffer, InAllocation.Handle);

		InAllocation.Handle = nullptr;
		InAllocation.Info = {};
	}

	auto FVulkanMemoryManager::MapMemory(FVulkanAllocation& Allocation) const -> void*
	{
		if (!Allocation.IsValid())
		{
			DOGE_ERROR("Attempted to map an invalid allocation.");
			return nullptr;
		}

		void* Data = nullptr;
		VkResult Result = vmaMapMemory(Allocator, Allocation.Handle, &Data);
		if (Result != VK_SUCCESS)
		{
			DOGE_ERROR("Failed to map memory: {}", vk::to_string(static_cast<vk::Result>(Result)));
			const vk::MemoryType MemoryType = GetMemoryType(Allocation);
			const vk::MemoryHeap MemoryHeap = GetMemoryHeap(MemoryType.heapIndex);
			DOGE_ERROR("Allocation size: {}", Allocation.GetSize());
			DOGE_ERROR("Memory Heap: {}", vk::to_string(MemoryHeap.flags));
			DOGE_ERROR("Memory property flags: {}", vk::to_string(MemoryType.propertyFlags));
			return nullptr;
		}

		vmaGetAllocationInfo(Allocator, Allocation.Handle, &Allocation.Info);
		return Data;
	}

	auto FVulkanMemoryManager::UnmapMemory(FVulkanAllocation& Allocation) const -> void
	{
		if (!Allocation.IsValid())
		{
			return;
		}
		vmaUnmapMemory(Allocator, Allocation.Handle);
		vmaGetAllocationInfo(Allocator, Allocation.Handle, &Allocation.Info);
	}

	auto FVulkanMemoryManager::Flush(const FVulkanAllocation& Allocation, vk::DeviceSize Offset, vk::DeviceSize Size) const -> void
	{
		vmaFlushAllocation(Allocator, Allocation.Handle, Offset, Size);
	}

	auto FVulkanMemoryManager::GetMemoryType(uint32 MemoryTypeIndex) const -> vk::MemoryType
	{
		check(MemoryTypeIndex < MemoryProperties.memoryTypeCount);
		return MemoryProperties.memoryTypes[MemoryTypeIndex];
	}

	auto FVulkanMemoryManager::GetMemoryType(const FVulkanAllocation& Allocation) const -> vk::MemoryType
	{
		return GetMemoryType(Allocation.GetMemoryTypeIndex());
	}

	auto FVulkanMemoryManager::GetMemoryHeap(uint32 MemoryHeapIndex) const -> vk::MemoryHeap
	{
		check(MemoryHeapIndex < MemoryProperties.memoryHeapCount);
		return MemoryProperties.memoryHeaps[MemoryHeapIndex];
	}

	FVulkanFence::FVulkanFence(FVulkanDevice& InDevice, FVulkanFenceManager& Owner, bool bInCreateSignaled)
		: Device(InDevice)
		, Owner(Owner)
		, State(EState::NotReady)
	{
		vk::FenceCreateInfo fenceInfo;
		if (bInCreateSignaled)
		{
			State = EState::Signaled;
			fenceInfo.setFlags(vk::FenceCreateFlagBits::eSignaled);
		}

		Handle = Device.GetHandle().createFence(fenceInfo);
	}

	FVulkanFence::~FVulkanFence()
	{
		Device.GetHandle().destroyFence(Handle);
	}

	auto FVulkanFence::Wait(uint64 TimeoutInNanoseconds) -> bool
	{
		check(State == EState::NotReady);
		vk::Result result = Device.GetHandle().waitForFences(Handle, true, TimeoutInNanoseconds);
		if (result == vk::Result::eSuccess)
		{
			State = EState::Signaled;
			return true;
		}
		if (result == vk::Result::eTimeout)
		{
			return false;
		}
		DOGE_ERROR("Failed to wait for fence: {}", vk::to_string(result));
		return false;
	}

	auto FVulkanFence::Reset() -> void
	{
		Device.GetHandle().resetFences(Handle);
		State = EState::NotReady;
	}

	FVulkanFenceManager::FVulkanFenceManager(FVulkanDevice& InDevice)
		: Device(InDevice)
	{
	}
	FVulkanFenceManager::~FVulkanFenceManager()
	{
		check(UsedFences.empty());
		check(FreeFences.empty());
	}

	auto FVulkanFenceManager::Deinit() -> void
	{
		std::lock_guard Lock(FenceMutex);
		check(UsedFences.empty());
		for (FVulkanFence* Fence : FreeFences)
		{
			DestroyFence(Fence);
		}
		FreeFences.clear();
	}

	auto FVulkanFenceManager::IsFenceSignaled(FVulkanFence* Fence) -> bool
	{
		if (Fence->IsSignaled())
		{
			return true;
		}

		return CheckFenceSignaled(Fence);
	}

	auto FVulkanFenceManager::AllocateFence(bool bInCreateSignaled) -> FVulkanFence*
	{
		std::lock_guard Lock(FenceMutex);

		if (!FreeFences.empty())
		{
			FVulkanFence* Fence = FreeFences.back();
			if (bInCreateSignaled)
			{
				Fence->State = FVulkanFence::EState::Signaled;
			}
			FreeFences.pop_back();
			UsedFences.push_back(Fence);
			return Fence;
		}

		FVulkanFence* NewFence = new FVulkanFence(Device, *this, bInCreateSignaled);
		UsedFences.push_back(NewFence);
		return NewFence;
	}

	auto FVulkanFenceManager::ReleaseFence(FVulkanFence*& Fence) -> void
	{
		std::lock_guard Lock(FenceMutex);
		const auto It = std::ranges::find(UsedFences, Fence);
		check(It != UsedFences.end());
		UsedFences.erase(It);
		ResetFence(Fence);
		FreeFences.push_back(Fence);
		Fence = nullptr;
	}

	auto FVulkanFenceManager::WaitForFence(FVulkanFence* InFence, uint64 TimeoutInNanoseconds) -> bool
	{
		return InFence->Wait(TimeoutInNanoseconds);
	}

	auto FVulkanFenceManager::ResetFence(FVulkanFence* InFence) -> void
	{
		InFence->Reset();
	}

	auto FVulkanFenceManager::CheckFenceSignaled(FVulkanFence* InFence) const -> bool
	{
		check(InFence->State == FVulkanFence::EState::NotReady);

		vk::Result Result = Device.GetHandle().getFenceStatus(InFence->GetHandle());

		if (Result == vk::Result::eSuccess)
		{
			InFence->State = FVulkanFence::EState::Signaled;
			return true;
		}
		if (Result == vk::Result::eNotReady)
		{
			return false;
		}
		DOGE_ERROR("Failed to check fence status: {}", vk::to_string(Result));
		return false;
	}

	auto FVulkanFenceManager::DestroyFence(FVulkanFence* InFence) const -> void
	{
		Device.GetHandle().destroyFence(InFence->GetHandle());
		InFence->Handle = nullptr;
		delete InFence;
	}

	FVulkanSemaphore::FVulkanSemaphore(FVulkanDevice& InDevice)
		: Device(InDevice)
	{
		Semaphore = Device.GetHandle().createSemaphore(vk::SemaphoreCreateInfo());
	}

	FVulkanSemaphore::~FVulkanSemaphore()
	{
		Device.GetDeferredDeletionQueue().EnqueueResource(FDeferredDeletionQueue::EType::Semaphore, Semaphore);
	}
} // namespace Doge::VulkanRHI