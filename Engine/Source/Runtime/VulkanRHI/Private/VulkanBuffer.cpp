#include "VulkanBuffer.h"

#include "RHICommandList.h"
#include "VulkanDynamicRHI.h"
#include "VulkanRHIPrivate.h"
#include "VulkanCommandBuffer.h"
#include "VulkanDevice.h"
#include "VulkanContext.h"

namespace Durin::VulkanRHI
{
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
		Device.GetDeferredDeletionQueue().EnqueueResource(FDeferredDeletionQueue::EType::Buffer, Buffer, Allocation);
	}

	auto FVulkanBuffer::Write(
		FVulkanCommandListContext& Context,
		uint32 Offset,
		std::span<const uint8> Data) -> void
	{
		check(!Data.empty() && Offset <= Desc.Size && Data.size() <= Desc.Size - Offset);
		if (void* MappedData = Allocation.GetMappedData(); MappedData != nullptr)
		{
			std::memcpy(static_cast<uint8*>(MappedData) + Offset, Data.data(), Data.size());
			FlushMappedMemory(Offset, static_cast<uint32>(Data.size()));
			return;
		}

		FStagingBuffer StagingBuffer(Device, static_cast<uint32>(Data.size()));
		std::memcpy(StagingBuffer.GetMappedPointer(), Data.data(), Data.size());
		StagingBuffer.FlushMappedMemory();
		vk::CommandBuffer CmdBuffer = Context.GetCommandBuffer()->GetHandle();
		CmdBuffer.copyBuffer(
			StagingBuffer.GetHandle(), Buffer,
			vk::BufferCopy(0, Offset, Data.size()));

		vk::BufferMemoryBarrier Barrier;
		Barrier.setBuffer(Buffer)
			.setOffset(Offset)
			.setSize(Data.size())
			.setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
			.setDstAccessMask(
				vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite);
		CmdBuffer.pipelineBarrier(
			vk::PipelineStageFlagBits::eTransfer,
			vk::PipelineStageFlagBits::eAllCommands,
			{}, {}, Barrier, {});
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

} // namespace Durin::VulkanRHI
