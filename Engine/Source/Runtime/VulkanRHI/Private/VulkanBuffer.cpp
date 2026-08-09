#include "VulkanBuffer.h"

#include "RHICommandList.h"
#include "VulkanDynamicRHI.h"
#include "VulkanRHIPrivate.h"
#include "VulkanCommandBuffer.h"
#include "VulkanDevice.h"
#include "VulkanContext.h"

namespace Durin::VulkanRHI
{
	namespace
	{
		auto GetCanonicalBufferAccess(EBufferUsageFlags Usage) -> ERHIAccess
		{
			if (EnumHasAnyFlags(Usage, EBufferUsageFlags::UnorderedAccess))
				return ERHIAccess::GraphicsShaderReadWrite;
			ERHIAccess Access = ERHIAccess::None;
			if (EnumHasAnyFlags(Usage, EBufferUsageFlags::VertexBuffer)) Access |= ERHIAccess::VertexBufferRead;
			if (EnumHasAnyFlags(Usage, EBufferUsageFlags::IndexBuffer)) Access |= ERHIAccess::IndexBufferRead;
			if (EnumHasAnyFlags(Usage, EBufferUsageFlags::UniformBuffer)) Access |= ERHIAccess::GraphicsUniformRead;
			if (EnumHasAnyFlags(Usage, EBufferUsageFlags::ShaderResource | EBufferUsageFlags::StructuredBuffer | EBufferUsageFlags::ByteAddressBuffer))
				Access |= ERHIAccess::GraphicsShaderRead;
			if (EnumHasAnyFlags(Usage, EBufferUsageFlags::SourceCopy)) Access |= ERHIAccess::TransferRead;
			return Access;
		}
	}

	FVulkanBuffer::FVulkanBuffer(FVulkanDevice& InDevice, const FRHIBufferCreateDesc& InCreateDesc)
		: FRHIBuffer(InCreateDesc)
		, Device(InDevice)
		, StateTracker(InCreateDesc.Size)
	{
		CheckVulkanRHIThread();
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
		const vk::Result Result = MemoryManager.CreateBuffer(
			Allocation, Buffer, AllocFlags, BufferInfo);
		if (Result != vk::Result::eSuccess)
		{
			throw std::runtime_error(std::format(
				"Vulkan buffer allocation failed: result={}, size={}, usage={}, allocationFlags={}",
				vk::to_string(Result), BufferInfo.size,
				vk::to_string(BufferInfo.usage), static_cast<uint32>(AllocFlags)));
		}
	}

	FVulkanBuffer::~FVulkanBuffer()
	{
		CheckVulkanRHIThread();
		if (Buffer && Allocation.IsValid())
		{
			Device.GetDeferredDeletionQueue().EnqueueResource(
				FDeferredDeletionQueue::EType::Buffer, Buffer, Allocation);
		}
	}

	auto FVulkanBuffer::Write(
		FVulkanCommandListContext& Context,
		uint32 Offset,
		std::span<const uint8> Data) -> void
	{
		CheckVulkanRHIThread();
		check(!Data.empty() && Offset <= Desc.Size && Data.size() <= Desc.Size - Offset);
		if (void* MappedData = Allocation.GetMappedData(); MappedData != nullptr)
		{
			std::memcpy(static_cast<uint8*>(MappedData) + Offset, Data.data(), Data.size());
			FlushMappedMemory(Offset, static_cast<uint32>(Data.size()));
			StateTracker.Apply(Offset, Data.size(), ERHIAccess::HostWrite);
			const ERHIAccess FinalAccess = GetCanonicalBufferAccess(GetUsage());
			if (FinalAccess != ERHIAccess::None)
			{
				const std::array Transition{FRHIBufferTransition{
					this, Offset, Data.size(), ERHIAccess::HostWrite, FinalAccess}};
				Context.RHITransitionBuffers(Transition);
			}
			return;
		}

		FStagingBuffer StagingBuffer(Device, static_cast<uint32>(Data.size()));
		std::memcpy(StagingBuffer.GetMappedPointer(), Data.data(), Data.size());
		StagingBuffer.FlushMappedMemory();
		vk::CommandBuffer CmdBuffer = Context.GetCommandBuffer()->GetHandle();
		const std::array PreCopyTransition{FRHIBufferTransition{
			this, Offset, Data.size(), ERHIAccess::Discard, ERHIAccess::TransferWrite}};
		Context.RHITransitionBuffers(PreCopyTransition);
		CmdBuffer.copyBuffer(
			StagingBuffer.GetHandle(), Buffer,
			vk::BufferCopy(0, Offset, Data.size()));

		const ERHIAccess FinalAccess = GetCanonicalBufferAccess(GetUsage());
		if (FinalAccess != ERHIAccess::None)
		{
			const std::array PostCopyTransition{FRHIBufferTransition{
				this, Offset, Data.size(), ERHIAccess::TransferWrite, FinalAccess}};
			Context.RHITransitionBuffers(PostCopyTransition);
		}
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
		CheckVulkanRHIThread();
		for (uint32 FrameIndex = 0; FrameIndex < Frames.size(); ++FrameIndex)
		{
			ReservePage(FrameIndex, 4 * 1024 * 1024);
		}
	}

	FVulkanDynamicUniformBufferAllocator::~FVulkanDynamicUniformBufferAllocator() = default;

	auto FVulkanDynamicUniformBufferAllocator::BeginFrameProducer(
		uint32 FrameIndex) -> void
	{
		FrameIndex %= kFrameInFlight;
		FFrameState& FrameState = Frames[FrameIndex];
		check(!FrameState.Chunks.empty());
		FrameState.CurrentChunkIndex = 0;
		for (FChunk& Chunk : FrameState.Chunks)
		{
			Chunk.Offset = 0;
		}
	}

	auto FVulkanDynamicUniformBufferAllocator::TryAllocate(
		uint32 FrameIndex,
		const void* Data,
		uint32 Size,
		FRHIUniformBufferRange& OutRange) -> bool
	{
		check(Data);
		check(Size > 0);

		const uint32 Alignment = GetAlignment();
		const uint32 AllocationSize = AlignUp(Size, Alignment);
		FrameIndex %= kFrameInFlight;
		FFrameState& FrameState = Frames[FrameIndex];
		check(!FrameState.Chunks.empty());

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
				return false;
			}
		}

		const uint32 AllocationOffset = AlignUp(Chunk->Offset, Alignment);
		check(AllocationOffset + Size <= Chunk->Buffer->GetSize());
		void* MappedPointer = Chunk->Buffer->GetMappedPointer();
		check(MappedPointer);
		std::memcpy(static_cast<uint8*>(MappedPointer) + AllocationOffset, Data, Size);
		Chunk->Buffer->FlushMappedMemory(AllocationOffset, Size);
		Chunk->Offset = AllocationOffset + AllocationSize;

		OutRange = FRHIUniformBufferRange{
			.Buffer = Chunk->Buffer.GetReference(),
			.Offset = AllocationOffset,
			.Size = Size
		};
		return true;
	}

	auto FVulkanDynamicUniformBufferAllocator::ReservePage(
		uint32 FrameIndex,
		uint32 MinSize) -> void
	{
		CheckVulkanRHIThread();
		FrameIndex %= kFrameInFlight;
		Frames[FrameIndex].Chunks.push_back(CreateChunk(MinSize));
	}

	auto FVulkanDynamicUniformBufferAllocator::CreateChunk(uint32 MinSize) -> FChunk
	{
		CheckVulkanRHIThread();
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
		const vk::Result Result = MemoryManager.CreateBuffer(
			Allocation, Buffer, AllocFlags, BufferInfo);
		if (Result != vk::Result::eSuccess)
		{
			throw std::runtime_error(std::format(
				"Vulkan staging-buffer allocation failed: result={}, size={}",
				vk::to_string(Result), BufferInfo.size));
		}
	}

	FStagingBuffer::~FStagingBuffer()
	{
		if (Buffer && Allocation.IsValid())
		{
			Device.GetDeferredDeletionQueue().EnqueueResource(
				FDeferredDeletionQueue::EType::Buffer, Buffer, Allocation);
		}
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
		TRefCountPtr<FRHIBuffer> Result;
		const FRHIFallibleOperationResult CreationResult =
			ExecuteFallibleVulkanCreationOperation(
				[this, CreateDesc, &Result]() {
					Result = new FVulkanBuffer(*Device, CreateDesc);
				});
		if (!CreationResult.IsSuccess())
		{
			DURIN_ERROR("Failed to create Vulkan RHI buffer '{}': {}",
				CreateDesc.DebugName ? CreateDesc.DebugName : "<unnamed>",
				CreationResult.Diagnostic);
			return nullptr;
		}
		auto* CreatedBuffer = static_cast<FVulkanBuffer*>(Result.GetReference());
		auto& InitialData = CreateDesc.InitialData;
		if (InitialData.Data)
		{
			RHICmdList.WriteBuffer(CreatedBuffer, InitialData.Data, InitialData.Size, 0);
		}
		return Result;
	}

} // namespace Durin::VulkanRHI
