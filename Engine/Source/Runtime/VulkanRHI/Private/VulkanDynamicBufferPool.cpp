#include "VulkanDynamicBufferPool.h"

#include "VulkanBuffer.h"
#include "VulkanDevice.h"
#include "VulkanDiagnostics.h"
#include "VulkanRHIPrivate.h"

namespace Durin::VulkanRHI
{
	auto FVulkanDynamicBufferPool::GetWriteAlignment() const -> uint32
	{
		const uint64 Alignment = std::max<uint64>(16, Device.GetGpuProperties().limits.nonCoherentAtomSize);
		require(Alignment <= UINT32_MAX);
		return static_cast<uint32>(Alignment);
	}
	auto FVulkanDynamicBufferPool::EnsureInitialPage() -> void
	{
		std::lock_guard Lock(GrowthMutex);
		if (!Pool.GetPageCount()) EnsurePage(PageGranularity);
	}
	auto FVulkanDynamicBufferPool::ProcessGrowthRequest() -> void
	{
		std::lock_guard Lock(GrowthMutex);
		if (const uint32 Requested = RequestedGrowth.exchange(0, std::memory_order_relaxed))
		{
			// Frame pacing or releasing the previous active slice can have made space
			// available since the recording-thread miss. Probe before allocating a page.
			if (!Pool.TryAcquire(Requested, GetWriteAlignment())) EnsurePage(Requested);
		}
	}
	auto FVulkanDynamicBufferPool::EnsurePage(uint32 Size) -> void
	{
		const uint64 MinimumCapacity = (uint64(Size) + PageGranularity - 1) / PageGranularity * PageGranularity;
		uint64 Capacity = std::max<uint64>(MinimumCapacity, NextPageSize);
		if (Capacity > UINT32_MAX || !Pool.CanAddPage(static_cast<uint32>(Capacity))) Capacity = MinimumCapacity;
		if (Capacity > UINT32_MAX || !Pool.CanAddPage(static_cast<uint32>(Capacity))) return;
		auto Desc = FRHIBufferCreateDesc::Create("VulkanDynamicBufferPage", static_cast<uint32>(Capacity), 0,
			EBufferUsageFlags::Dynamic | EBufferUsageFlags::VertexBuffer | EBufferUsageFlags::IndexBuffer);
		vk::Result Result;
		auto Owner = std::make_shared<TRefCountPtr<FVulkanBuffer>>(new FVulkanBuffer(Device, Desc, false, &Result));
		if (Result != vk::Result::eSuccess)
		{
			if (!IsRecoverableVulkanCreationError(Result)) ThrowVulkanError(Result, "Dynamic buffer page allocation");
			return;
		}
		void* Mapped = (*Owner)->GetMappedPointer();
		if (Mapped && Pool.AddPage(Owner, Mapped, Desc.Size))
		{
			(*Owner)->MarkMappedPage();
			NextPageSize = static_cast<uint32>(std::min<uint64>(Capacity * 2, MaxBytes));
		}
	}
	auto FVulkanDynamicBufferPool::TryAcquire(uint32 Size,
		FRHIDynamicBufferWritePoolSnapshot* Snapshot) -> std::shared_ptr<FRHIDynamicBufferWriteRange>
	{
		const uint32 Alignment = GetWriteAlignment();
		auto Range = Pool.TryAcquire(Size, Alignment, Snapshot);
		if (!Range)
		{
			const uint64 AlignedSize = (uint64(Size) + Alignment - 1) / Alignment * Alignment;
			if (AlignedSize <= UINT32_MAX)
			{
				uint32 Current = RequestedGrowth.load(std::memory_order_relaxed);
				while (Current < AlignedSize && !RequestedGrowth.compare_exchange_weak(Current,
					static_cast<uint32>(AlignedSize), std::memory_order_relaxed)) {}
			}
		}
		return Range;
	}
}
