#include "VulkanMemory.h"

#include "VulkanDevice.h"

namespace Doge::VulkanRHI
{
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
		else if (result == vk::Result::eTimeout)
		{
			return false;
		}
		else
		{
			DOGE_ERROR("Failed to wait for fence: {}", vk::to_string(result));
			return false;
		}
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
		else if (Result == vk::Result::eNotReady)
		{
			return false;
		}
		else
		{
			DOGE_ERROR("Failed to check fence status: {}", vk::to_string(Result));
			return false;
		}
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
}