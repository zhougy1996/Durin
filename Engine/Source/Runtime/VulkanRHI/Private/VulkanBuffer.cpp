#include "VulkanBuffer.h"

#include "RHICommandList.h"
#include "VulkanDynamicRHI.h"
#include "VulkanRHIPrivate.h"
#include "VulkanCommandBuffer.h"
#include "VulkanDevice.h"
#include "VulkanContext.h"

namespace Durin::VulkanRHI
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

	FVulkanBuffer::FVulkanBuffer(FVulkanDevice& InDevice, const FRHIBufferCreateDesc& InCreateDesc)
		: FRHIBuffer(InCreateDesc)
		, Device(InDevice)
	{
		vk::BufferCreateInfo BufferInfo;
		BufferInfo.setSize(InCreateDesc.Size);
		BufferInfo.setUsage(ToVulkan_BufferUsageFlags(InCreateDesc.Usage));
		BufferInfo.setSharingMode(vk::SharingMode::eExclusive);

		EVulkanAllocationFlags AllocFlags{};
		if (EnumHasAnyFlags(InCreateDesc.Usage, EBufferUsageFlags::Dynamic))
		{
			AllocFlags |= EVulkanAllocationFlags::HostVisible | EVulkanAllocationFlags::PersistentMapped;
		}

		const FVulkanMemoryManager& MemoryManager = Device.GetMemoryManager();
		MemoryManager.CreateBuffer(Allocation, Buffer, AllocFlags, BufferInfo);
	}

	FVulkanBuffer::~FVulkanBuffer()
	{
		check(LockStatus != ELockStatus::Locked);
		Device.GetDeferredDeletionQueue().EnqueueResource(FDeferredDeletionQueue::EType::Buffer, Buffer, Allocation);
	}

	auto FVulkanBuffer::Lock(const FRHICommandListImmediate& RHICmdList, uint32 Offset, uint32 Size, EResourceLockMode LockMode) -> void*
	{
		check(LockStatus == ELockStatus::Unlocked);
		check(Size != 0 && Offset + Size <= Desc.Size);

		LockStatus = ELockStatus::Locked;
		void* Data = nullptr;

		if (LockMode == EResourceLockMode::ReadOnly)
		{
			DURIN_ERROR("Read-only buffer locking is not supported yet.");
			LockStatus = ELockStatus::Unlocked;
			return nullptr;
		}

		if (LockMode == EResourceLockMode::WriteOnly)
		{
			if (IsStatic()) // Static buffers require a staging buffer for locking.
			{
				auto* StagingBuffer = new FStagingBuffer(Device, Size);
				Data = StagingBuffer->GetMappedPointer();
				AddPendingLock(this, FVulkanPendingBufferLock{StagingBuffer, Offset, Size, LockMode, true});
			}
			else
			{
				Data = Allocation.GetMappedData();
				if (Data != nullptr)
				{
					// For dynamic buffers, we can return the mapped pointer directly.
					LockStatus = ELockStatus::PersistentMapping;
				}
				else
				{
					checkf(false, "Failed to map buffer memory for dynamic buffer. This should not happen as the buffer was created with persistent mapping.");
				}
			}
		}

		check(Data);
		return static_cast<uint8*>(Data) + Offset;
	}

	auto FVulkanBuffer::Unlock(const FRHICommandListImmediate& RHICmdList) -> void
	{
		check(LockStatus != ELockStatus::Unlocked);

		if (LockStatus == ELockStatus::PersistentMapping)
		{
			FlushMappedMemory(0, Desc.Size);
		}
		else
		{
			FVulkanPendingBufferLock BufferLock = RetrievePendingLock(this);

			FStagingBuffer* StagingBuffer = BufferLock.StagingBuffer;

			check(StagingBuffer);
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

			delete StagingBuffer;
		}
		LockStatus = ELockStatus::Unlocked;
	}

	auto FVulkanBuffer::IsDynamic() const -> bool
	{
		return EnumHasAnyFlags(Desc.Usage, EBufferUsageFlags::Dynamic);
	}

	auto FVulkanBuffer::IsStatic() const -> bool
	{
		return EnumHasAnyFlags(Desc.Usage, EBufferUsageFlags::Static);
	}

	auto FVulkanBuffer::GetMappedPointer() const -> void*
	{
		return Allocation.GetMappedData();
	}

	auto FVulkanBuffer::FlushMappedMemory(uint32 Offset, uint32 Size) -> void
	{
		const vk::DeviceSize FlushSize = Size != 0 ? Size : VK_WHOLE_SIZE;
		Device.GetMemoryManager().Flush(Allocation, Offset, FlushSize);
	}

	FVulkanDynamicUniformBufferAllocator::FVulkanDynamicUniformBufferAllocator(FVulkanDevice& InDevice)
		: Device(InDevice)
	{
	}

	FVulkanDynamicUniformBufferAllocator::~FVulkanDynamicUniformBufferAllocator() = default;

	auto FVulkanDynamicUniformBufferAllocator::BeginFrame(uint32 FrameIndex) -> void
	{
		CurrentFrameIndex = FrameIndex % kFrameInFlight;
		FFrameState& FrameState = Frames[CurrentFrameIndex];
		FrameState.CurrentChunkIndex = 0;
		for (FChunk& Chunk : FrameState.Chunks)
		{
			Chunk.Offset = 0;
		}
	}

	auto FVulkanDynamicUniformBufferAllocator::Allocate(const void* Data, uint32 Size) -> FRHIUniformBufferRange
	{
		check(Data);
		check(Size > 0);

		const uint32 Alignment = GetAlignment();
		const uint32 AllocationSize = AlignUp(Size, Alignment);
		FFrameState& FrameState = Frames[CurrentFrameIndex];

		if (FrameState.Chunks.empty())
		{
			FrameState.Chunks.push_back(CreateChunk(AllocationSize));
		}

		FChunk* Chunk = &FrameState.Chunks[FrameState.CurrentChunkIndex];
		if (Chunk->Offset + AllocationSize > Chunk->Buffer->GetSize())
		{
			bool bFoundReusableChunk = false;
			for (uint32 ChunkIndex = FrameState.CurrentChunkIndex + 1; ChunkIndex < FrameState.Chunks.size(); ++ChunkIndex)
			{
				if (FrameState.Chunks[ChunkIndex].Buffer->GetSize() >= AllocationSize)
				{
					FrameState.CurrentChunkIndex = ChunkIndex;
					Chunk = &FrameState.Chunks[ChunkIndex];
					bFoundReusableChunk = true;
					break;
				}
			}

			if (!bFoundReusableChunk)
			{
				FrameState.Chunks.push_back(CreateChunk(AllocationSize));
				FrameState.CurrentChunkIndex = static_cast<uint32>(FrameState.Chunks.size() - 1);
				Chunk = &FrameState.Chunks.back();
			}
		}

		const uint32 AllocationOffset = AlignUp(Chunk->Offset, Alignment);
		check(AllocationOffset + Size <= Chunk->Buffer->GetSize());
		void* MappedPointer = Chunk->Buffer->GetMappedPointer();
		check(MappedPointer);
		std::memcpy(static_cast<uint8*>(MappedPointer) + AllocationOffset, Data, Size);
		Chunk->Buffer->FlushMappedMemory(AllocationOffset, Size);
		Chunk->Offset = AllocationOffset + AllocationSize;

		return FRHIUniformBufferRange{
			.Buffer = Chunk->Buffer.GetReference(),
			.Offset = AllocationOffset,
			.Size = Size
		};
	}

	auto FVulkanDynamicUniformBufferAllocator::CreateChunk(uint32 MinSize) -> FChunk
	{
		constexpr uint32 DefaultChunkSize = 4 * 1024 * 1024;
		const uint32 ChunkSize = std::max(DefaultChunkSize, AlignUp(MinSize, GetAlignment()));
		FRHIBufferCreateDesc CreateDesc = FRHIBufferCreateDesc::Create(
			"DynamicUniformBufferArena",
			ChunkSize,
			0,
			EBufferUsageFlags::UniformBuffer | EBufferUsageFlags::Dynamic | EBufferUsageFlags::Volatile
		);
		FChunk Chunk;
		Chunk.Buffer = new FVulkanBuffer(Device, CreateDesc);
		return Chunk;
	}

	auto FVulkanDynamicUniformBufferAllocator::GetAlignment() const -> uint32
	{
		const uint32 VulkanAlignment = static_cast<uint32>(Device.GetGpuProperties().limits.minUniformBufferOffsetAlignment);
		return std::max(16u, VulkanAlignment);
	}

	auto FVulkanDynamicUniformBufferAllocator::AlignUp(uint32 Value, uint32 Alignment) -> uint32
	{
		check(Alignment > 0);
		return (Value + Alignment - 1) / Alignment * Alignment;
	}

	FStagingBuffer::FStagingBuffer(FVulkanDevice& InDevice, uint32 InBufferSize)
		: Device(InDevice)
		, BufferSize(InBufferSize)
	{
		vk::BufferCreateInfo BufferInfo;
		BufferInfo.setSize(InBufferSize);
		BufferInfo.setUsage(vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferSrc);
		BufferInfo.setSharingMode(vk::SharingMode::eExclusive);

		EVulkanAllocationFlags AllocFlags = EVulkanAllocationFlags::HostVisible | EVulkanAllocationFlags::PersistentMapped;

		const FVulkanMemoryManager& MemoryManager = Device.GetMemoryManager();
		MemoryManager.CreateBuffer(Allocation, Buffer, AllocFlags, BufferInfo);
	}

	FStagingBuffer::~FStagingBuffer()
	{
		Device.GetDeferredDeletionQueue().EnqueueResource(FDeferredDeletionQueue::EType::Buffer, Buffer, Allocation);
	}

	auto FStagingBuffer::GetMappedPointer() const -> void*
	{
		return Allocation.GetMappedData();
	}

	auto FStagingBuffer::FlushMappedMemory() -> void
	{
		Allocation.FlushMappedMemory(&Device);
	}

	auto FVulkanDynamicRHI::RHICreateBuffer(FRHICommandListImmediate& RHICmdList, const FRHIBufferCreateDesc& CreateDesc) -> TRefCountPtr<FRHIBuffer>
	{
		FVulkanBuffer* CreatedBuffer = new FVulkanBuffer(*Device, CreateDesc);
		auto& InitialData = CreateDesc.InitialData;
		if (InitialData.Data)
		{
			RHICmdList.WriteBuffer(CreatedBuffer, InitialData.Data, InitialData.Size, 0);
		}
		return CreatedBuffer;
	}

	auto FVulkanDynamicRHI::RHIAllocateDynamicUniformBuffer(FRHICommandListImmediate& RHICmdList, const void* Data, uint32 Size) -> FRHIUniformBufferRange
	{
		return Device->GetDynamicUniformBufferAllocator().Allocate(Data, Size);
	}

	auto FVulkanDynamicRHI::RHILockBuffer(FRHICommandListImmediate& RHICmdList, FRHIBuffer* Buffer, uint32 Offset, uint32 Size, EResourceLockMode LockMode) -> void*
	{
		return static_cast<FVulkanBuffer*>(Buffer)->Lock(RHICmdList, Offset, Size, LockMode);
	}

	auto FVulkanDynamicRHI::RHIUnlockBuffer(FRHICommandListImmediate& RHICmdList, FRHIBuffer* Buffer) -> void
	{
		return static_cast<FVulkanBuffer*>(Buffer)->Unlock(RHICmdList);
	}

} // namespace Durin::VulkanRHI
