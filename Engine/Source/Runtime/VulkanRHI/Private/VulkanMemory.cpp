#include "VulkanMemory.h"

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

	bool FVulkanMemoryManager::CreateBuffer(FVulkanAllocation& OutAllocation, vk::Buffer& OutBuffer, const vk::BufferCreateInfo& BufferCreateInfo, const char* DebugName) const
	{
		VmaAllocationCreateInfo AllocCreateInfo{};
		AllocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO;

		VkBuffer RawBuffer;
		VkResult Result = vmaCreateBuffer(
			Allocator,
			reinterpret_cast<const VkBufferCreateInfo*>(&BufferCreateInfo),
			&AllocCreateInfo,
			&RawBuffer,
			&OutAllocation.Handle,
			&OutAllocation.Info
		);

		if (Result != VK_SUCCESS) return false;

		OutBuffer = RawBuffer;

		if (DebugName) {
			vmaSetAllocationName(Allocator, OutAllocation.Handle, DebugName);
		}
		return true;
	}

	void FVulkanMemoryManager::DestroyImage(FVulkanAllocation& InAllocation, vk::Image InImage) const
	{
		check(InImage && InAllocation.Handle != VK_NULL_HANDLE);
		vmaDestroyImage(Allocator, InImage, InAllocation.Handle);

		InAllocation.Handle = nullptr;
		InAllocation.Info = {};
	}

	void FVulkanMemoryManager::DestroyBuffer(FVulkanAllocation& InAllocation, vk::Buffer InBuffer) const
	{
		check(InImage && InAllocation.Handle != VK_NULL_HANDLE);
		vmaDestroyBuffer(Allocator, InBuffer, InAllocation.Handle);

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

	void FVulkanFenceManager::Deinit()
	{
		std::lock_guard Lock(FenceMutex);
		check(UsedFences.empty());
		for (FVulkanFence* Fence : FreeFences)
		{
			DestroyFence(Fence);
		}
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