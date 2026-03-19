#include "VulkanMemory.h"

#define VMA_IMPLEMENTATION
#include "vma/vk_mem_alloc.h"


#include "VulkanDevice.h"
#include "VulkanDynamicRHI.h"

namespace Doge::VulkanRHI
{
	FVulkanMemoryManager::FVulkanMemoryManager()
		: Device(nullptr)
		, Allocator(nullptr)
	{
	}

	void FVulkanMemoryManager::Init(FVulkanDevice* InDevice)
	{
		Device = InDevice;

		vk::PhysicalDevice Gpu = Device->GetGpu();
		vk::PhysicalDeviceProperties GpuProps = Device->GetGpuProperties();

		VmaAllocatorCreateInfo allocatorInfo = {};
		allocatorInfo.vulkanApiVersion = GpuProps.apiVersion;
		allocatorInfo.physicalDevice = Gpu;
		allocatorInfo.device = Device->GetHandle();
		allocatorInfo.instance = FVulkanDynamicRHI::Get().RHIGetVkInstance();

		vmaCreateAllocator(&allocatorInfo, &Allocator);
	}

	void FVulkanMemoryManager::Deinit()
	{
		Allocator = nullptr;
	}

	bool FVulkanMemoryManager::CreateImage(FVulkanAllocation& OutAllocation, vk::Image& OutImage, const vk::ImageCreateInfo& ImageCreateInfo, const char* DebugName /* = nullptr */) const
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

		if (DebugName) {
			vmaSetAllocationName(Allocator, OutAllocation.Handle, DebugName);
		}
		return true;
	}

	void FVulkanMemoryManager::Destroy(FVulkanAllocation& InAllocation, vk::Image InImage) const
	{
		check(InImage && InAllocation.Handle != VK_NULL_HANDLE);
		vmaDestroyImage(Allocator, InImage, InAllocation.Handle);

		InAllocation.Handle = nullptr;
		InAllocation.Info = {};
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

		Fence = Device.GetHandle().createFence(fenceInfo);
	}

	FVulkanFence::~FVulkanFence()
	{
		Device.GetHandle().destroyFence(Fence);
	}

	auto FVulkanFence::Wait(uint64 TimeoutInNanoseconds) -> bool
	{
		check(State == EState::NotReady);
		vk::Result result = Device.GetHandle().waitForFences(Fence, true, TimeoutInNanoseconds);
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
		Device.GetHandle().resetFences(Fence);
		State = EState::NotReady;
	}

	FVulkanFenceManager::FVulkanFenceManager(FVulkanDevice& InDevice)
		: Device(InDevice)
	{
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
		return new FVulkanFence(Device, *this, bInCreateSignaled);
	}

	auto FVulkanFenceManager::ReleaseFence(FVulkanFence*& Fence) -> void
	{
		delete Fence;
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

	FVulkanSemaphore::FVulkanSemaphore(FVulkanDevice& InDevice)
		: Device(InDevice)
	{
		Semaphore = Device.GetHandle().createSemaphore(vk::SemaphoreCreateInfo());
	}

	FVulkanSemaphore::~FVulkanSemaphore()
	{
		Device.GetHandle().destroySemaphore(Semaphore);
	}
} // namespace Doge::VulkanRHI