#include "VulkanUniformBuffer.h"
#include "VulkanShaderBufferBindings.h"
#include "VulkanDevice.h"
#include "VulkanDiagnostics.h"
#include "VulkanDynamicRHI.h"
#include "VulkanRHIPrivate.h"
#include "RHIBufferUploadData.h"
#include "Profiling/Profiling.h"

namespace Durin::VulkanRHI
{
	auto FVulkanDynamicRHI::RHICreateUniformBuffer(const FRHIUniformBufferLayout& Layout,
		ERHIBufferLifetimeUsage Usage, FByteView Data, std::span<FRHIResource* const> References)
		-> TRefCountPtr<FRHIUniformBuffer>
	{
		return new FVulkanUniformBuffer(*Device, Layout, Usage, Data, References);
	}

	FVulkanUniformAllocation::~FVulkanUniformAllocation()
	{
		if (Size) GVulkanMemoryBaselineTracker.RecordArenaRangeReclaimed(
			EVulkanAllocationClassCandidate::DynamicUpload, Size);
	}

	auto FVulkanUniformBuffer::UpdateContents(FByteView Data,
		std::span<FRHIResource* const> References) -> void
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Vulkan.Uniform.Update");
		CheckVulkanRHIThread();
		require(Data.size() == GetSize());
		auto Next = std::make_shared<FVulkanUniformAllocation>();
		Next->Lease = Device.GetBindingPool(true).Reserve(GetSize());
		const auto [Buffer, Offset] = Device.GetBindingPool(true).Resolve(
			Next->Lease);
		Next->Buffer = Buffer;
		Next->Offset = Offset;
		for (auto* Reference : References) Next->References.emplace_back(Reference);
		Buffer->InitializeDeferredReadOnly(Data, static_cast<uint32>(Offset));
		const auto& Limits = Device.GetGpuProperties().limits;
		const uint64 Alignment = std::max<uint64>({16, Limits.nonCoherentAtomSize,
			Limits.minUniformBufferOffsetAlignment});
		Next->Size = (GetSize() + Alignment - 1) / Alignment * Alignment;
		GVulkanMemoryBaselineTracker.RecordArenaRangeAllocated(
			EVulkanAllocationClassCandidate::DynamicUpload, Next->Size, false, false);
		Allocation = std::move(Next);
		ReleasePendingData();
	}

	auto FVulkanUniformBuffer::GetAllocation() -> const std::shared_ptr<FVulkanUniformAllocation>&
	{
		CheckVulkanRHIThread();
		if (!Allocation)
		{
			require(GetPendingData());
			std::vector<FRHIResource*> References;
			for (const auto& Reference : GetPendingReferences()) References.push_back(Reference.GetReference());
			UpdateContents(GetPendingData()->GetData(), References);
		}
		return Allocation;
	}
}
