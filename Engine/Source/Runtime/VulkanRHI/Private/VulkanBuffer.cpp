#include "VulkanBuffer.h"

#include "RHICommandList.h"
#include "VulkanDevice.h"
#include "VulkanRHIPrivate.h"

namespace Doge::VulkanRHI
{
	FVulkanBuffer::FVulkanBuffer(FVulkanDevice* InDevice, const FRHIBufferCreateDesc& InCreateDesc)
		: FRHIBuffer(InCreateDesc)
		, Device(InDevice)
	{
		vk::BufferCreateInfo BufferInfo;
		BufferInfo.setSize(InCreateDesc.Size);
		BufferInfo.setUsage(ConvertToVulkanBufferUsageFlags(InCreateDesc.Usage));
		BufferInfo.setSharingMode(vk::SharingMode::eExclusive);

		const FVulkanMemoryManager& MemoryManager = InDevice->GetMemoryManager();
		MemoryManager.CreateBuffer(Allocation, Buffer, BufferInfo);
	}

	FVulkanBuffer::~FVulkanBuffer()
	{
		Device->GetDeferredDeletionQueue().EnqueueResource(FDeferredDeletionQueue::EType::Buffer, Buffer, Allocation);
	}

	auto FVulkanDynamicRHI::RHICreateBuffer(FRHICommandList& RHICmdList, const FRHIBufferCreateDesc& CreateDesc) -> TRefCountPtr<FRHIBuffer>
	{
		return new FVulkanBuffer(Device, CreateDesc);
	}

} // namespace Doge::VulkanRHI