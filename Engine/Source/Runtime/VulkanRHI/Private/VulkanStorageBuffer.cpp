#include "VulkanStorageBuffer.h"
#include "VulkanShaderBufferBindings.h"
#include "VulkanDevice.h"
#include "VulkanDiagnostics.h"
#include "VulkanDynamicRHI.h"
#include "VulkanRHIPrivate.h"
#include "Profiling/Profiling.h"

namespace Durin::VulkanRHI
{
	auto FVulkanDynamicRHI::RHICreateStorageBuffer(const FRHIBufferDesc& Desc,
		ERHIBufferLifetimeUsage Usage, FByteView Data) -> TRefCountPtr<FRHIBuffer>
	{
		return new FVulkanStorageBuffer(*Device, Desc, Usage, Data);
	}

	FVulkanStorageAllocation::~FVulkanStorageAllocation()
	{
		if (Size) GVulkanMemoryBaselineTracker.RecordArenaRangeReclaimed(
			EVulkanAllocationClassCandidate::DynamicUpload, Size);
	}

	auto FVulkanStorageBuffer::AllocateContents() -> void
	{
		CheckVulkanRHIThread();
		auto Next = std::make_shared<FVulkanStorageAllocation>();
		Next->Lease = Device.GetBindingPool(false).Reserve(GetSize());
		const auto [Buffer, Offset] = Device.GetBindingPool(false).Resolve(
			Next->Lease);
		Next->Buffer = Buffer;
		Next->Offset = Offset;
		Buffer->InitializeDeferredReadOnly(GetContents(), static_cast<uint32>(Offset));
		const auto& Limits = Device.GetGpuProperties().limits;
		const uint64 Alignment = std::max<uint64>({16, Limits.nonCoherentAtomSize,
			Limits.minStorageBufferOffsetAlignment});
		Next->Size = (GetSize() + Alignment - 1) / Alignment * Alignment;
		GVulkanMemoryBaselineTracker.RecordArenaRangeAllocated(
			EVulkanAllocationClassCandidate::DynamicUpload, Next->Size, false, false);
		Allocation = std::move(Next);
	}

	auto FVulkanStorageBuffer::UpdateContents(uint32 Offset, FByteView Data) -> void
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Vulkan.Storage.Update");
		CheckVulkanRHIThread();
		FRHIStorageBuffer::UpdateContents(Offset, Data);
		AllocateContents();
	}

	auto FVulkanStorageBuffer::GetAllocation() -> const std::shared_ptr<FVulkanStorageAllocation>&
	{
		CheckVulkanRHIThread();
		if (!Allocation) AllocateContents();
		return Allocation;
	}
}
