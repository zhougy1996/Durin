#include "VulkanBuffer.h"

#include "RHICommandList.h"
#include "VulkanCommandBuffer.h"
#include "VulkanDevice.h"
#include "VulkanRHIPrivate.h"
#include "VulkanContext.h"

namespace Doge::VulkanRHI
{
	struct FVulkanPendingBufferLock
	{
		FStagingBuffer* StagingBuffer = nullptr;
		uint32 Offset = 0;
		uint32 Size = 0;
		EResourceLockMode LockMode = EResourceLockMode::WriteOnly;
		bool FirstLock = false;
	};

	namespace
	{
		std::unordered_map<const FRHIBuffer*, FVulkanPendingBufferLock> GPendingLocks;

		std::mutex GPendingLocksMutex;

		// Add a pending lock for the given buffer.
		auto AddPendingLock(const FRHIBuffer* Buffer, const FVulkanPendingBufferLock& BufferLock) -> void
		{
			std::lock_guard Lock(GPendingLocksMutex);
			check(!GPendingLocks.contains(Buffer));
			GPendingLocks.emplace(Buffer, BufferLock);
		}

		// Retrieve and remove the pending lock for the given buffer.
		auto RetrievePendingLock(const FRHIBuffer* Buffer) -> FVulkanPendingBufferLock
		{
			std::lock_guard Lock(GPendingLocksMutex);
			const auto It = GPendingLocks.find(Buffer);
			check(It != GPendingLocks.end() && "Buffer Lock/Unlock mismatch.");
			FVulkanPendingBufferLock Result = It->second;
			GPendingLocks.erase(It);
			return Result;
		}
	} // namespace

	FVulkanBuffer::FVulkanBuffer(FVulkanDevice* InDevice, const FRHIBufferCreateDesc& InCreateDesc)
		: FRHIBuffer(InCreateDesc)
		, Device(InDevice)
	{
		vk::BufferCreateInfo BufferInfo;
		BufferInfo.setSize(InCreateDesc.Size);
		BufferInfo.setUsage(ConvertToVulkanBufferUsageFlags(InCreateDesc.Usage));
		BufferInfo.setSharingMode(vk::SharingMode::eExclusive);

		EVulkanAllocationFlags AllocFlags{};

		const FVulkanMemoryManager& MemoryManager = Device->GetMemoryManager();
		MemoryManager.CreateBuffer(Allocation, Buffer, AllocFlags, BufferInfo);
	}

	FVulkanBuffer::~FVulkanBuffer()
	{
		Device->GetDeferredDeletionQueue().EnqueueResource(FDeferredDeletionQueue::EType::Buffer, Buffer, Allocation);
	}

	auto FVulkanBuffer::Lock(const FRHICommandList& RHICmdList, uint32 Offset, uint32 Size, EResourceLockMode LockMode) -> void*
	{
		check(LockStatus == ELockStatus::Unlocked);
		check(Size != 0 && Offset + Size <= Desc.Size);
		void* Data = nullptr;

		if (LockMode == EResourceLockMode::ReadOnly)
		{
			DOGE_ERROR("Read-only buffer locking is not supported yet.");
			return nullptr;
		}

		if (LockMode == EResourceLockMode::WriteOnly)
		{
			if (IsStatic())
			{
				auto* StagingBuffer = new FStagingBuffer(*Device, Size);
				Data = StagingBuffer->Map();
				AddPendingLock(this, FVulkanPendingBufferLock{StagingBuffer, Offset, Size, LockMode, true});
			}
		}

		if (Data == nullptr)
		{
			DOGE_ERROR("Failed to lock buffer.");
			return nullptr;
		}
		LockStatus = ELockStatus::Locked;
		return static_cast<uint8*>(Data) + Offset;
	}

	auto FVulkanBuffer::Unlock(const FRHICommandList& RHICmdList) -> void
	{
		check(LockStatus != ELockStatus::Unlocked);
		FVulkanPendingBufferLock BufferLock = RetrievePendingLock(this);
		FStagingBuffer* StagingBuffer = BufferLock.StagingBuffer;
		check(StagingBuffer);
		EResourceLockMode LockMode = BufferLock.LockMode;

		if (LockMode == EResourceLockMode::WriteOnly)
		{
			StagingBuffer->FlushMappedMemory();
			auto& Context = static_cast<FVulkanCommandListContext&>(RHICmdList.GetContext());
			vk::CommandBuffer CmdBufferHandle = Context.GetCommandBuffer()->GetHandle();
			vk::BufferCopy CopyRegion;
			CopyRegion.setSrcOffset(BufferLock.Offset);
			CopyRegion.setDstOffset(BufferLock.Offset);
			CopyRegion.setSize(BufferLock.Size);
			CmdBufferHandle.copyBuffer(StagingBuffer->GetHandle(), Buffer, CopyRegion);

			// Insert a memory barrier to ensure the copy is visible to subsequent buffer reads.
			vk::BufferMemoryBarrier BufferBarrier;
			BufferBarrier.setBuffer(Buffer);
			BufferBarrier.setOffset(BufferLock.Offset);
			BufferBarrier.setSize(BufferLock.Size);
			BufferBarrier.setSrcAccessMask(vk::AccessFlagBits::eTransferWrite);
			BufferBarrier.setDstAccessMask(vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite);

			CmdBufferHandle.pipelineBarrier(
				vk::PipelineStageFlagBits::eTransfer,
				vk::PipelineStageFlagBits::eAllCommands,
				{},
				{},
				BufferBarrier,
				{});
		}
		StagingBuffer->Unmap();
		delete StagingBuffer;
	}

	auto FVulkanBuffer::IsDynamic() const -> bool
	{
		return EnumHasAnyFlags(Desc.Usage, EBufferUsageFlags::Dynamic);
	}

	auto FVulkanBuffer::IsStatic() const -> bool
	{
		return EnumHasAnyFlags(Desc.Usage, EBufferUsageFlags::Static);
	}

	FStagingBuffer::FStagingBuffer(FVulkanDevice& InDevice, uint32 InBufferSize)
		: Device(InDevice)
		, BufferSize(InBufferSize)
	{
		vk::BufferCreateInfo BufferInfo;
		BufferInfo.setSize(InBufferSize);
		BufferInfo.setUsage(vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferSrc);
		BufferInfo.setSharingMode(vk::SharingMode::eExclusive);

		EVulkanAllocationFlags AllocFlags = EVulkanAllocationFlags::HostVisible | EVulkanAllocationFlags::Mapped;

		const FVulkanMemoryManager& MemoryManager = Device.GetMemoryManager();
		MemoryManager.CreateBuffer(Allocation, Buffer, AllocFlags, BufferInfo);
	}

	FStagingBuffer::~FStagingBuffer()
	{
		Device.GetDeferredDeletionQueue().EnqueueResource(FDeferredDeletionQueue::EType::Buffer, Buffer, Allocation);
	}

	auto FStagingBuffer::Map() -> void*
	{
		const FVulkanMemoryManager& MemoryManager = Device.GetMemoryManager();
		MemoryManager.MapMemory(Allocation);
		return Allocation.GetMappedData();
	}

	auto FStagingBuffer::Unmap() -> void
	{
		const FVulkanMemoryManager& MemoryManager = Device.GetMemoryManager();
		MemoryManager.UnmapMemory(Allocation);
	}

	auto FStagingBuffer::GetMappedData() const -> void*
	{
		return Allocation.GetMappedData();
	}

	auto FStagingBuffer::FlushMappedMemory() -> void
	{
		Allocation.FlushMappedMemory(&Device);
	}

	auto FVulkanDynamicRHI::RHICreateBuffer(FRHICommandList& RHICmdList, const FRHIBufferCreateDesc& CreateDesc) -> TRefCountPtr<FRHIBuffer>
	{
		FVulkanBuffer* CreatedBuffer = new FVulkanBuffer(Device, CreateDesc);
		auto& InitialData = CreateDesc.InitialData;
		if (InitialData.Data)
		{
			RHICmdList.WriteBuffer(CreatedBuffer, InitialData.Data, InitialData.Size, 0);
		}
		return CreatedBuffer;
	}

	auto FVulkanDynamicRHI::RHILockBuffer(FRHICommandList& RHICmdList, FRHIBuffer* Buffer, uint32 Offset, uint32 Size, EResourceLockMode LockMode) -> void*
	{
		return static_cast<FVulkanBuffer*>(Buffer)->Lock(RHICmdList, Offset, Size, LockMode);
	}

	auto FVulkanDynamicRHI::RHIUnlockBuffer(FRHICommandList& RHICmdList, FRHIBuffer* Buffer) -> void
	{
		return static_cast<FVulkanBuffer*>(Buffer)->Unlock(RHICmdList);
	}

} // namespace Doge::VulkanRHI