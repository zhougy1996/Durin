#include "VulkanMemory.h"

#include "VulkanDevice.h"

FVulkanFence::FVulkanFence(FVulkanDevice& Device, FVulkanFenceManager& Owner, bool bCreateSignaled)
	: Device_(Device)
	, Owner_(Owner)
	, State_(EState::eNotReady)
{
	vk::FenceCreateInfo fenceInfo;
	if (bCreateSignaled)
	{
		State_ = EState::eSignaled;
		fenceInfo.setFlags(vk::FenceCreateFlagBits::eSignaled);
	}

	Fence_ = Device_.GetHandle().createFence(fenceInfo);
}

FVulkanFence::~FVulkanFence()
{
	Device_.GetHandle().destroyFence(Fence_);
}

auto FVulkanFence::Wait(uint64 TimeoutInNanoseconds) -> bool
{
	check(State_ == EState::eNotReady);
	vk::Result result = Device_.GetHandle().waitForFences(Fence_, true, TimeoutInNanoseconds);
	if (result == vk::Result::eSuccess)
	{
		State_ = EState::eSignaled;
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
	Device_.GetHandle().resetFences(Fence_);
	State_ = EState::eNotReady;
}

FVulkanFenceManager::FVulkanFenceManager(FVulkanDevice& Device)
	: Device_(Device)
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

auto FVulkanFenceManager::AllocateFence(bool bCreateSignaled) -> FVulkanFence*
{
	return new FVulkanFence(Device_, *this, bCreateSignaled);
}

auto FVulkanFenceManager::ReleaseFence(FVulkanFence*& Fence) -> void
{
	delete Fence;
	Fence = nullptr;
}

auto FVulkanFenceManager::WaitForFence(FVulkanFence* Fence, uint64 TimeoutInNanoseconds) -> bool
{
	return Fence->Wait(TimeoutInNanoseconds);
}

auto FVulkanFenceManager::ResetFence(FVulkanFence* Fence) -> void
{
	Fence->Reset();
}

auto FVulkanFenceManager::CheckFenceSignaled(FVulkanFence* Fence) -> bool
{
	check(Fence->State_ == FVulkanFence::EState::eNotReady);

	vk::Result Result = Device_.GetHandle().getFenceStatus(Fence->GetHandle());

	if (Result == vk::Result::eSuccess)
	{
		Fence->State_ = FVulkanFence::EState::eSignaled;
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

FVulkanSemaphore::FVulkanSemaphore(FVulkanDevice& Device)
	: Device_(Device)
{
	Semaphore_ = Device_.GetHandle().createSemaphore(vk::SemaphoreCreateInfo());
}

FVulkanSemaphore::~FVulkanSemaphore()
{
	Device_.GetHandle().destroySemaphore(Semaphore_);
}